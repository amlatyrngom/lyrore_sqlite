/*
** UDF Transform Plugin
**
** Transforms: product_filter(price, qty, threshold) = 1
** Into:       (price * qty) < threshold
**
** Uses Pattern API for structure matching, LyExpr for clean AST rewriting.
** Expected: 30x+ speedup (171ms -> <6ms on 10k rows)
*/

#include "lyrore_plugin.hpp"
#include "lyrore_pattern.hpp"
#include "lyrore_cabi.h"
#include <cstring>

class UdfTransformPlugin : public lyrore::Plugin {
    lyrore::pattern::PatternPtr pattern_;
    sqlite3* db_ = nullptr;
    bool pattern_init_attempted_ = false;

public:
    std::string name() const override { return "udf_transform"; }

    void onInit(sqlite3* db) override {
        db_ = db;
        // Defer pattern creation until first query (table may not exist yet)
    }

    void onPreOpt(lyrore::PreOptContext& ctx) override {
        Expr* pWhere = ctx.where_raw();
        if (!pWhere) return;

        // Quick structural check first (avoid pattern init overhead for non-matching queries)
        if (pWhere->op != TK_EQ) return;

        Expr* pFunc = pWhere->pLeft;
        Expr* pVal = pWhere->pRight;

        // Handle reversed: 1 = product_filter(...)
        if (pVal && pVal->op == TK_FUNCTION) {
            Expr* tmp = pFunc;
            pFunc = pVal;
            pVal = tmp;
        }

        if (!pFunc || pFunc->op != TK_FUNCTION) return;
        if (!pFunc->u.zToken || lyrore_sqlite3StrICmp(pFunc->u.zToken, "product_filter") != 0) return;

        // Try lazy pattern initialization for full structure validation
        if (!pattern_init_attempted_) {
            pattern_init_attempted_ = true;
            // Get table name from the first column reference to build pattern dynamically
            ExprList* pArgs = pFunc->x.pList;
            if (pArgs && pArgs->nExpr > 0 && pArgs->a[0].pExpr) {
                Expr* firstCol = pArgs->a[0].pExpr;
                if (firstCol->y.pTab && firstCol->y.pTab->zName) {
                    char sql[256];
                    snprintf(sql, sizeof(sql),
                        "SELECT 1 FROM %s WHERE product_filter(?, ?, ?) = 1",
                        firstCol->y.pTab->zName);
                    pattern_ = lyrore::pattern::Pattern::from_where_expr(db_, sql);
                }
            }
        }

        // If pattern exists and is valid, use it for structure validation
        if (pattern_ && pattern_->is_valid()) {
            if (!pattern_->match_expr(pWhere)) {
                return;  // Structure doesn't match pattern
            }
        }
        // Even without pattern, we already validated basic structure above

        // Verify function has 3 arguments and is compared to 1
        ExprList* pArgs = pFunc->x.pList;
        if (!pArgs || pArgs->nExpr != 3) return;
        if (!pVal || pVal->op != TK_INTEGER || pVal->u.iValue != 1) return;

        // Get argument expressions
        Expr* pPrice = pArgs->a[0].pExpr;
        Expr* pQty = pArgs->a[1].pExpr;
        Expr* pThreshold = pArgs->a[2].pExpr;
        if (!pPrice || !pQty || !pThreshold) return;

        // ===== AST REWRITE USING LyExpr =====
        // Convert arguments to LyExpr (preserves Table* bindings for columns)
        auto lyPrice = lyrore::LyExpr::from_sqlite(pPrice);
        auto lyQty = lyrore::LyExpr::from_sqlite(pQty);
        auto lyThreshold = lyrore::LyExpr::from_sqlite(pThreshold);

        if (!lyPrice || !lyQty || !lyThreshold) return;

        // Build: (price * qty) < threshold
        auto lyMul = lyrore::LyExpr::multiply(lyPrice, lyQty);
        auto lyResult = lyrore::LyExpr::less_than(lyMul, lyThreshold);

        // In-place substitution into pWhere - LyExpr handles memory safely
        lyResult->to_sqlite(ctx.parse(), pWhere);

        ctx.set_modified();
    }
};

LYRORE_REGISTER_PLUGIN(UdfTransformPlugin);
