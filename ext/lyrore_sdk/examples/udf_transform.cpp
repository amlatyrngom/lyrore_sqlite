/*
** UDF Transform Plugin
**
** Transforms: product_filter(price, qty, threshold) = 1
** Into:       (price * qty) < threshold
**
** Uses raw SQLite C API for AST manipulation (no LyExpr).
** Expected: 30x+ speedup (171ms -> <6ms on 10k rows)
*/

#include "lyrore_plugin.hpp"
#include "lyrore_cabi.h"
#include <cstring>

class UdfTransformPlugin : public lyrore::Plugin {
public:
    std::string name() const override { return "udf_transform"; }

    void onPreOpt(lyrore::PreOptContext& ctx) override {
        Expr* pWhere = ctx.where_raw();
        if (!pWhere) return;

        // Match pattern: product_filter(a, b, c) = 1  (or 1 = product_filter(...))
        if (pWhere->op != TK_EQ) return;

        Expr* pFunc = pWhere->pLeft;
        Expr* pVal = pWhere->pRight;

        // Handle reversed order: 1 = product_filter(...)
        if (pVal && pVal->op == TK_FUNCTION) {
            Expr* tmp = pFunc;
            pFunc = pVal;
            pVal = tmp;
        }

        // Check: left side is function named "product_filter"
        if (!pFunc || pFunc->op != TK_FUNCTION) return;
        if (!pFunc->u.zToken || lyrore_sqlite3StrICmp(pFunc->u.zToken, "product_filter") != 0) return;

        // Check: function has 3 arguments
        ExprList* pArgs = pFunc->x.pList;
        if (!pArgs || pArgs->nExpr != 3) return;

        // Check: compared to integer 1
        if (!pVal || pVal->op != TK_INTEGER) return;
        if (pVal->u.iValue != 1) return;

        // Get the arguments
        Expr* pPrice = pArgs->a[0].pExpr;
        Expr* pQty = pArgs->a[1].pExpr;
        Expr* pThreshold = pArgs->a[2].pExpr;

        if (!pPrice || !pQty || !pThreshold) return;

        // Build: (price * qty) < threshold
        // Using raw SQLite C API via lyrore_cabi wrappers
        sqlite3* db = ctx.db();

        // Duplicate the column expressions to preserve Table* bindings
        Expr* pPriceDup = lyrore_sqlite3ExprDup(db, pPrice, 0);
        Expr* pQtyDup = lyrore_sqlite3ExprDup(db, pQty, 0);
        Expr* pThreshDup = lyrore_sqlite3ExprDup(db, pThreshold, 0);

        if (!pPriceDup || !pQtyDup || !pThreshDup) {
            // Cleanup on failure
            if (pPriceDup) lyrore_sqlite3ExprDelete(db, pPriceDup);
            if (pQtyDup) lyrore_sqlite3ExprDelete(db, pQtyDup);
            if (pThreshDup) lyrore_sqlite3ExprDelete(db, pThreshDup);
            return;
        }

        // Create: price * qty
        Expr* pMul = lyrore_sqlite3Expr(db, TK_STAR, nullptr);
        if (!pMul) {
            lyrore_sqlite3ExprDelete(db, pPriceDup);
            lyrore_sqlite3ExprDelete(db, pQtyDup);
            lyrore_sqlite3ExprDelete(db, pThreshDup);
            return;
        }
        pMul->pLeft = pPriceDup;
        pMul->pRight = pQtyDup;

        // Create: (price * qty) < threshold
        Expr* pLt = lyrore_sqlite3Expr(db, TK_LT, nullptr);
        if (!pLt) {
            lyrore_sqlite3ExprDelete(db, pMul);
            lyrore_sqlite3ExprDelete(db, pThreshDup);
            return;
        }
        pLt->pLeft = pMul;
        pLt->pRight = pThreshDup;

        // In-place substitution into pWhere
        // Free old children of pWhere
        lyrore_sqlite3ExprDelete(db, pWhere->pLeft);
        lyrore_sqlite3ExprDelete(db, pWhere->pRight);

        // Copy new expression into pWhere
        pWhere->op = pLt->op;
        pWhere->pLeft = pLt->pLeft;
        pWhere->pRight = pLt->pRight;

        // Detach children from pLt before freeing wrapper
        pLt->pLeft = nullptr;
        pLt->pRight = nullptr;
        lyrore_sqlite3DbFree(db, pLt);

        ctx.set_modified();
    }
};

LYRORE_REGISTER_PLUGIN(UdfTransformPlugin);
