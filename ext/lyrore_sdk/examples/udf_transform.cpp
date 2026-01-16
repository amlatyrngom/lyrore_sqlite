/*
** UDF Transform Plugin
**
** Transforms: product_filter(price, qty, threshold) = 1
** Into:       (price * qty) < threshold
**
** This eliminates function call overhead and enables native arithmetic.
** Expected: 30x+ speedup (171ms -> <6ms on 10k rows)
**
** Note: The pattern API is best suited for query matching (identifying templates)
** rather than expression transformation. For transformation, direct AST manipulation
** is more efficient since we need access to the actual column expressions.
*/

#include "lyrore_plugin.hpp"

class UdfTransformPlugin : public lyrore::Plugin {
public:
    std::string name() const override { return "udf_transform"; }

    void onPreOpt(lyrore::PreOptContext& ctx) override {
        auto where = ctx.where();
        if (!where) return;

        // Match pattern: product_filter(a, b, c) = 1
        if (where->op != lyrore::LyOp::EQ) return;

        lyrore::LyExpr* func = where->left.get();
        lyrore::LyExpr* val = where->right.get();

        // Handle reversed order: 1 = product_filter(...)
        if (val && val->is_function()) {
            std::swap(func, val);
        }

        // Check: function is product_filter with 3 args
        if (!func || !func->is_function("product_filter")) return;
        if (func->nargs() != 3) return;

        // Check: compared to integer 1
        if (!val || !val->is_integer()) return;
        auto int_val = val->as_int();
        if (!int_val || *int_val != 1) return;

        // Build: (arg0 * arg1) < arg2
        auto mul = lyrore::LyExpr::multiply(func->arg(0), func->arg(1));
        auto cmp = lyrore::LyExpr::less_than(std::move(mul), func->arg(2));

        // Substitute back
        cmp->to_sqlite(ctx.parse(), ctx.where_raw());
        ctx.set_modified();
    }
};

LYRORE_REGISTER_PLUGIN(UdfTransformPlugin);
