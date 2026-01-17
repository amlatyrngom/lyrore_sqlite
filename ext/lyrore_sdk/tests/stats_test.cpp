/*
** Stats Plugin - For statistics collection testing
** Collects and exposes statement/scan statistics via SQL functions
*/
#include "lyrore_plugin.hpp"
#include <sqlite3.h>
#include <map>
#include <string>
#include <mutex>

using namespace lyrore;

// Global to store last collected stats
static std::mutex g_stats_mutex;
static StmtStats g_last_stmt_stats;
static std::vector<ScanStats> g_last_scan_stats;

class StatsPlugin : public Plugin {
public:
    std::string name() const override { return "stats"; }

    void onInit(sqlite3* db) override {
        // Register function to get last vm_steps
        sqlite3_create_function_v2(db, "get_last_vm_steps", 0, SQLITE_UTF8, nullptr,
            [](sqlite3_context* ctx, int, sqlite3_value**) {
                std::lock_guard<std::mutex> lock(g_stats_mutex);
                sqlite3_result_int64(ctx, g_last_stmt_stats.vm_steps);
            }, nullptr, nullptr, nullptr);

        // Register function to get last fullscan_steps
        sqlite3_create_function_v2(db, "get_last_fullscan_steps", 0, SQLITE_UTF8, nullptr,
            [](sqlite3_context* ctx, int, sqlite3_value**) {
                std::lock_guard<std::mutex> lock(g_stats_mutex);
                sqlite3_result_int64(ctx, g_last_stmt_stats.fullscan_steps);
            }, nullptr, nullptr, nullptr);

        // Register function to get scan count
        sqlite3_create_function_v2(db, "get_scan_count", 0, SQLITE_UTF8, nullptr,
            [](sqlite3_context* ctx, int, sqlite3_value**) {
                std::lock_guard<std::mutex> lock(g_stats_mutex);
                sqlite3_result_int64(ctx, (int64_t)g_last_scan_stats.size());
            }, nullptr, nullptr, nullptr);

        // Register function to get first scan's loops
        sqlite3_create_function_v2(db, "get_first_scan_loops", 0, SQLITE_UTF8, nullptr,
            [](sqlite3_context* ctx, int, sqlite3_value**) {
                std::lock_guard<std::mutex> lock(g_stats_mutex);
                if (!g_last_scan_stats.empty()) {
                    sqlite3_result_int64(ctx, g_last_scan_stats[0].loops);
                } else {
                    sqlite3_result_int64(ctx, -1);
                }
            }, nullptr, nullptr, nullptr);
    }

    void onPostQuery(PostQueryContext& ctx) override {
        std::lock_guard<std::mutex> lock(g_stats_mutex);
        g_last_stmt_stats = ctx.stmt_stats();
        g_last_scan_stats = ctx.scan_stats();
    }
};

LYRORE_REGISTER_PLUGIN(StatsPlugin);
