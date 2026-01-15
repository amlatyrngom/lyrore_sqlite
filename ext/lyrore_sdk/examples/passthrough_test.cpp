/*
** Passthrough Test Plugin
**
** Tests that LyExpr from_sqlite -> clone -> to_sqlite produces identical results.
** This is CRITICAL for proving correctness of the SDK.
*/

#include "lyrore_plugin.hpp"

class PassthroughTestPlugin : public lyrore::Plugin {
public:
    std::string name() const override { return "passthrough_test"; }

    void onPreOpt(lyrore::PreOptContext& ctx) override {
        // Get WHERE clause
        auto where = ctx.where();
        if (!where) return;

        // Clone it (deep copy)
        auto copy = where->clone();

        // Substitute back into original location
        copy->to_sqlite(ctx.parse(), ctx.where_raw());

        // Mark as modified (even though result should be identical)
        ctx.set_modified();
    }
};

LYRORE_REGISTER_PLUGIN(PassthroughTestPlugin);
