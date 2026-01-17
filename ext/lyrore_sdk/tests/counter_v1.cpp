/*
** Counter Plugin V1 - For hot-reload testing
** Returns version 1 from get_version() function
** Uses versioned function registration for transaction-aware versioning.
*/
#include "lyrore_plugin.hpp"
#include "lyrore_main.hpp"
#include <sqlite3.h>

using namespace lyrore;

class CounterPluginV1 : public Plugin {
public:
    std::string name() const override { return "counter"; }

    void onInit(sqlite3* db) override {
        // Use versioned function registration for transaction-aware versioning
        LyroreMain* main = lyrore_main();
        if (main) {
            main->register_versioned_function(db, "counter", "get_version", 0,
                [](sqlite3_context* ctx, int, sqlite3_value**) {
                    sqlite3_result_int64(ctx, 1);
                });
        } else {
            // Fallback for direct loading without LyroreMain
            sqlite3_create_function_v2(db, "get_version", 0, SQLITE_UTF8, nullptr,
                [](sqlite3_context* ctx, int, sqlite3_value**) {
                    sqlite3_result_int64(ctx, 1);
                }, nullptr, nullptr, nullptr);
        }
    }
};

LYRORE_REGISTER_PLUGIN(CounterPluginV1);
