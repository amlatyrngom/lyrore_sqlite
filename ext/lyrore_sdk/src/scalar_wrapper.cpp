/*
** Lyrore Scalar Wrapper
** 
** Wraps CustomOperator with SCALAR output mode as a SQLite function.
** Uses sqlite3_create_function_v2 for registration.
*/

#include "lyrore_custom_op.hpp"

extern "C" {
#include "sqlite3.h"
}

namespace lyrore {

// ============================================================
// Scalar Function Context
// ============================================================

struct ScalarContext {
    std::function<std::unique_ptr<CustomOperator>()> factory;
    sqlite3* db;

    ScalarContext(std::function<std::unique_ptr<CustomOperator>()> f, sqlite3* d)
        : factory(std::move(f)), db(d) {}
};


// ============================================================
// Scalar Function Implementation
// ============================================================

static void scalar_func_impl(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
    ScalarContext* sctx = static_cast<ScalarContext*>(sqlite3_user_data(ctx));
    if (!sctx) {
        sqlite3_result_error(ctx, "Custom op context missing", -1);
        return;
    }

    // Create operator instance
    auto op = sctx->factory();
    if (!op) {
        sqlite3_result_error(ctx, "Failed to create custom operator", -1);
        return;
    }

    op->_set_db(sctx->db);

    // Extract parameters from argv into MatchParams
    MatchParams params;
    for (int i = 0; i < argc; i++) {
        std::string name = "$" + std::to_string(i + 1);
        LyValue val = sqlite3_value_to_lyvalue(argv[i]);
        params.set(name, val);
    }
    op->_set_params(&params);

    // Execute and get result
    op->execute();
    LyValue result = op->compute_scalar();

    // Return result
    set_sqlite3_result(ctx, result);
}

static void scalar_context_destroy(void* p) {
    delete static_cast<ScalarContext*>(p);
}


// ============================================================
// Registration Function
// ============================================================

void register_scalar_wrapper(sqlite3* db, CustomOpEntry* entry) {
    if (!db || !entry || !entry->factory) return;

    // Count parameters in pattern
    int nParams = 0;
    if (entry->pattern) {
        nParams = entry->pattern->num_params();
    }

    // Create context - copies the factory, doesn't depend on entry pointer
    auto* sctx = new ScalarContext(entry->factory, db);

    // Register function with -1 for variable args, or exact count
    int rc = sqlite3_create_function_v2(
        db,
        entry->func_name.c_str(),
        nParams,                    // number of arguments
        SQLITE_UTF8 | SQLITE_DETERMINISTIC,
        sctx,
        scalar_func_impl,
        nullptr,                    // xStep (not aggregate)
        nullptr,                    // xFinal (not aggregate)
        scalar_context_destroy      // destructor
    );

    if (rc != SQLITE_OK) {
        delete sctx;
    }
}

} // namespace lyrore
