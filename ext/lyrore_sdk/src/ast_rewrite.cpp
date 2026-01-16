/*
** Lyrore AST Rewriter
**
** Detects registered custom operator patterns in queries and rewrites
** the AST to use the generated functions or virtual tables.
**
** Supports two rewrite modes:
** 1. Subquery mode: Rewrites subquery expressions to function calls
** 2. Top-level mode: Rewrites entire query to use virtual table (for ITERATOR)
*/

#include "lyrore_custom_op.hpp"
#include "lyrore_cabi.h"
#include <cstring>

extern "C" {
#include "sqliteInt.h"
}

namespace lyrore {

// ============================================================
// AST Rewrite Implementation
// ============================================================

namespace {

// Forward declarations
void walk_expr(sqlite3* db, Parse* pParse, Expr* pExpr, CustomOpRegistry& registry, int& rewrites);
void walk_exprlist(sqlite3* db, Parse* pParse, ExprList* pList, CustomOpRegistry& registry, int& rewrites);
void walk_srclist(sqlite3* db, Parse* pParse, SrcList* pSrc, CustomOpRegistry& registry, int& rewrites);
void walk_select(sqlite3* db, Parse* pParse, Select* pSelect, CustomOpRegistry& registry, int& rewrites);

// Helper to create a TK_COLUMN expression referencing a virtual table column
static Expr* createVtabColumnExpr(sqlite3* db, Table* pTab, int iCursor, int iColumn) {
    Expr* pNew = (Expr*)lyrore_sqlite3DbMallocZero(db, sizeof(Expr));
    if (!pNew) return nullptr;

    pNew->op = TK_COLUMN;
    pNew->iTable = iCursor;
    pNew->iColumn = (ynVar)iColumn;
    pNew->y.pTab = pTab;
    pNew->flags = EP_Leaf;  // No children

    return pNew;
}

// Try to rewrite top-level query with ITERATOR pattern
// Returns true if rewritten
bool try_rewrite_toplevel_iterator(sqlite3* db, Parse* pParse, Select* pSelect,
                                   CustomOpRegistry& registry) {
    if (!pSelect) return false;

    // Only handle simple SELECT without UNION/etc
    if (pSelect->pPrior) return false;
    if (pSelect->op != TK_SELECT) return false;

    // Try to match against registered ITERATOR patterns
    for (const auto& entry : registry.get_entries()) {
        if (!entry->pattern) continue;
        if (entry->mode != OutputMode::ITERATOR) continue;

        auto match = entry->pattern->match(pSelect);
        if (!match.matched()) continue;

        // Get cached Table* from registration
        Table* pVtab = static_cast<Table*>(entry->pVtabTable);
        if (!pVtab) {

            return false;
        }

        // Allocate a cursor number for the vtab
        int iCursor = pParse->nTab++;

        // Step 1: Create new SrcList for vtab
        // Free old SrcList
        if (pSelect->pSrc) {
            lyrore_sqlite3SrcListDelete(db, pSelect->pSrc);
        }

        // Create new SrcList with vtab reference
        SrcList* pNewSrc = lyrore_sqlite3SrcListAppend(pParse, nullptr, nullptr, nullptr);
        if (!pNewSrc) {

            return false;
        }

        // Set vtab name and Table* pointer
        pNewSrc->a[0].zName = lyrore_sqlite3DbStrDup(db, entry->vtab_name.c_str());
        pNewSrc->a[0].iCursor = iCursor;
        pNewSrc->a[0].pSTab = pVtab;

        // CRITICAL: Increment nTabRef to prevent Table* corruption!
        // When sqlite3SrcListDelete() is called later, it calls sqlite3DeleteTable()
        // which decrements nTabRef. Without this increment, nTabRef can reach 0
        // and the Table* gets corrupted/freed.
        pVtab->nTabRef++;

        pSelect->pSrc = pNewSrc;

        // Step 2: Clear GROUP BY clause
        if (pSelect->pGroupBy) {
            lyrore_sqlite3ExprListDelete(db, pSelect->pGroupBy);
            pSelect->pGroupBy = nullptr;

        // Step 2b: Clear aggregate flags - vtab produces plain rows
        // Without this, SQLite treats result as single-row aggregate
        pSelect->selFlags &= ~(SF_Aggregate | SF_HasAgg);
        pSelect->pHaving = nullptr;
        }

        // Step 3: Create new result columns matching vtab output
        // Free old result column list
        if (pSelect->pEList) {
            lyrore_sqlite3ExprListDelete(db, pSelect->pEList);
        }

        // Create new result columns referencing vtab columns using TK_COLUMN
        ExprList* pNewEList = nullptr;
        for (size_t i = 0; i < entry->output_columns.size(); i++) {
            // Create TK_COLUMN expression
            Expr* pCol = createVtabColumnExpr(db, pVtab, iCursor, (int)i);
            if (!pCol) {

                if (pNewEList) lyrore_sqlite3ExprListDelete(db, pNewEList);
                return false;
            }

            pNewEList = lyrore_sqlite3ExprListAppend(pParse, pNewEList, pCol);
            if (!pNewEList) {

                lyrore_sqlite3ExprDelete(db, pCol);
                return false;
            }

            // Set the column alias/name
            if (!entry->output_columns[i].empty()) {
                pNewEList->a[pNewEList->nExpr - 1].zEName =
                    lyrore_sqlite3DbStrDup(db, entry->output_columns[i].c_str());
                pNewEList->a[pNewEList->nExpr - 1].fg.eEName = ENAME_NAME;
            }
        }

        pSelect->pEList = pNewEList;

        return true;
    }

    return false;
}

// Try to rewrite a subquery expression (SCALAR mode only)
bool try_rewrite_subquery(sqlite3* db, Parse* pParse, Expr* pSubqExpr,
                          CustomOpRegistry& registry) {
    if (!pSubqExpr) return false;

    // Check if this is a subquery expression (EP_xIsSelect flag)
    if (!(pSubqExpr->flags & EP_xIsSelect)) return false;
    if (!pSubqExpr->x.pSelect) return false;

    Select* pSubq = pSubqExpr->x.pSelect;

    // Try to match against registered SCALAR patterns
    for (const auto& entry : registry.get_entries()) {
        if (!entry->pattern) continue;
        if (entry->mode != OutputMode::SCALAR) continue;

        auto match = entry->pattern->match(pSubq);
        if (!match.matched()) continue;

        // Found a match! Rewrite to function call
        // Replace subquery with: _lyrore_fn_N($1, $2, ...)

        // Build argument list from matched parameters
        ExprList* pArgs = nullptr;
        auto& params = match.params();
        for (auto& [name, val] : params) {
            Expr* pArg = nullptr;

            // Create expression based on value type
            if (std::holds_alternative<int64_t>(val)) {
                pArg = lyrore_sqlite3Expr(db, TK_INTEGER, nullptr);
                if (pArg) {
                    pArg->u.iValue = std::get<int64_t>(val);
                    pArg->flags |= EP_IntValue;
                }
            } else if (std::holds_alternative<double>(val)) {
                char buf[64];
                snprintf(buf, sizeof(buf), "%g", std::get<double>(val));
                pArg = lyrore_sqlite3Expr(db, TK_FLOAT, buf);
            } else if (std::holds_alternative<std::string>(val)) {
                const std::string& s = std::get<std::string>(val);
                pArg = lyrore_sqlite3Expr(db, TK_STRING, s.c_str());
            } else {
                // NULL or blob - use NULL for simplicity
                pArg = lyrore_sqlite3Expr(db, TK_NULL, nullptr);
            }

            if (pArg) {
                pArgs = lyrore_sqlite3ExprListAppend(pParse, pArgs, pArg);
            }
        }

        // Transform pSubqExpr into a function call
        // First, free the old subquery
        lyrore_sqlite3SelectDelete(db, pSubqExpr->x.pSelect);

        // Now set up as function call
        pSubqExpr->op = TK_FUNCTION;
        pSubqExpr->flags &= ~EP_xIsSelect;

        // Set function name
        pSubqExpr->u.zToken = lyrore_sqlite3DbStrDup(db, entry->func_name.c_str());

        pSubqExpr->x.pList = pArgs;

        return true;
    }

    return false;
}

// Walk expression tree looking for subqueries to rewrite
void walk_expr(sqlite3* db, Parse* pParse, Expr* pExpr,
               CustomOpRegistry& registry, int& rewrites) {
    if (!pExpr) return;

    // Check for subquery
    if ((pExpr->flags & EP_xIsSelect) && pExpr->x.pSelect) {
        if (try_rewrite_subquery(db, pParse, pExpr, registry)) {
            rewrites++;
            return;  // Don't recurse into rewritten expression
        }
        // Recurse into unmatched subquery
        walk_select(db, pParse, pExpr->x.pSelect, registry, rewrites);
    }

    // Check for expression list (function args, IN list, etc)
    if (!(pExpr->flags & EP_xIsSelect) && pExpr->x.pList) {
        walk_exprlist(db, pParse, pExpr->x.pList, registry, rewrites);
    }

    // Recurse to children
    walk_expr(db, pParse, pExpr->pLeft, registry, rewrites);
    walk_expr(db, pParse, pExpr->pRight, registry, rewrites);
}

void walk_exprlist(sqlite3* db, Parse* pParse, ExprList* pList,
                   CustomOpRegistry& registry, int& rewrites) {
    if (!pList) return;
    for (int i = 0; i < pList->nExpr; i++) {
        walk_expr(db, pParse, pList->a[i].pExpr, registry, rewrites);
    }
}

void walk_srclist(sqlite3* db, Parse* pParse, SrcList* pSrc,
                  CustomOpRegistry& registry, int& rewrites) {
    if (!pSrc) return;
    for (int i = 0; i < pSrc->nSrc; i++) {
        // Check if this SrcItem is a subquery
        if (pSrc->a[i].fg.isSubquery && pSrc->a[i].u4.pSubq) {
            Select* pSubSelect = pSrc->a[i].u4.pSubq->pSelect;
            if (pSubSelect) {
                walk_select(db, pParse, pSubSelect, registry, rewrites);
            }
        }
    }
}

void walk_select(sqlite3* db, Parse* pParse, Select* pSelect,
                 CustomOpRegistry& registry, int& rewrites) {
    if (!pSelect) return;

    // Walk result columns
    walk_exprlist(db, pParse, pSelect->pEList, registry, rewrites);

    // Walk FROM clause (subqueries)
    walk_srclist(db, pParse, pSelect->pSrc, registry, rewrites);

    // Walk WHERE
    walk_expr(db, pParse, pSelect->pWhere, registry, rewrites);

    // Walk GROUP BY
    walk_exprlist(db, pParse, pSelect->pGroupBy, registry, rewrites);

    // Walk HAVING
    walk_expr(db, pParse, pSelect->pHaving, registry, rewrites);

    // Walk ORDER BY
    walk_exprlist(db, pParse, pSelect->pOrderBy, registry, rewrites);

    // Walk compound selects
    if (pSelect->pPrior) {
        walk_select(db, pParse, pSelect->pPrior, registry, rewrites);
    }
}

} // anonymous namespace

// ============================================================
// Public API
// ============================================================

int rewrite_custom_ops(PreOptContext& ctx) {
    if (!ctx.select_raw()) return 0;

    sqlite3* db = ctx.db();
    auto& registry = CustomOpRegistry::for_db(db);

    if (registry.get_entries().empty()) {
        return 0;  // No custom ops registered
    }

    int rewrites = 0;

    // First, try to rewrite top-level query for ITERATOR mode
    if (try_rewrite_toplevel_iterator(db, ctx.parse(), ctx.select_raw(), registry)) {
        rewrites++;
        ctx.set_modified();
        return rewrites;  // Top-level rewrite succeeded, no need to walk subqueries
    }

    // Then, walk the entire select tree looking for subquery patterns (SCALAR mode)
    walk_select(db, ctx.parse(), ctx.select_raw(), registry, rewrites);

    if (rewrites > 0) {
        ctx.set_modified();
    }

    return rewrites;
}

} // namespace lyrore
