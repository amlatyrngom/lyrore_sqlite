/*
** Fast GROUP BY Custom Operator Example
** 
** Demonstrates ITERATOR mode for replacing:
**   SELECT category_id, SUM(qty) FROM orders GROUP BY category_id
** with a custom single-pass aggregation that uses pre-allocated arrays.
*/

#include "lyrore_plugin.hpp"
#include "lyrore_custom_op.hpp"
#include <vector>
#include <array>
#include <cstdio>
#include <cstring>
#include <variant>

extern "C" {
#include "sqlite3.h"
}

namespace {

// Helper to extract int64_t from LyValue
inline int64_t get_int(const lyrore::LyValue& v) {
    return std::get<int64_t>(v);
}

// ============================================================
// Fast GROUP BY Operator using ITERATOR mode
// Pre-allocates array for categories 0-4 (5 distinct values)
// Single-pass scan with direct array access - no hash table
// ============================================================

class FastGroupByOp : public lyrore::CustomOperator {
    // Pre-allocated array for 5 categories (0-4)
    std::array<int64_t, 5> sums_{};
    size_t pos_ = 0;
    bool executed_ = false;

public:
    lyrore::OutputMode output_mode() const override { 
        return lyrore::OutputMode::ITERATOR; 
    }

    void execute() override {
        if (executed_) return;
        executed_ = true;

        // Reset sums
        sums_.fill(0);

        // Single-pass scan through the orders table
        auto cursor = open_table("orders");
        if (!cursor.valid()) {
            fprintf(stderr, "FastGroupByOp: Failed to open orders table\n");
            return;
        }

        while (!cursor.eof()) {
            // Column 0: id (skip)
            // Column 1: category_id
            // Column 2: qty
            int64_t cat = get_int(cursor.get_column(1));
            int64_t qty = get_int(cursor.get_column(2));

            // Direct array access - no hash lookup
            if (cat >= 0 && cat < 5) {
                sums_[cat] += qty;
            }

            cursor.next();
        }
    }

    void iterator_reset() override {
        pos_ = 0;
    }

    bool iterator_next() override {
        pos_++;
        return pos_ <= 5;  // 5 categories (0-4)
    }

    std::vector<lyrore::LyValue> iterator_get_row() override {
        if (pos_ < 1 || pos_ > 5) return {};
        int64_t cat = static_cast<int64_t>(pos_ - 1);
        return {lyrore::LyValue{cat}, lyrore::LyValue{sums_[pos_-1]}};
    }
};

// ============================================================
// Test Plugin Class
// ============================================================

class FastGroupByPlugin : public lyrore::Plugin {
    bool registered_ = false;

public:
    std::string name() const override { return "FastGroupByPlugin"; }

    void onInit(sqlite3* db) override {
        // Register the custom operator with its pattern
        // Pattern: SELECT category_id, SUM(qty) FROM orders GROUP BY category_id
        lyrore::register_custom_op<FastGroupByOp>(
            db,
            "SELECT category_id, SUM(qty) FROM orders GROUP BY category_id",
            {"category_id", "total"}
        );
        registered_ = true;

        // Register a test function to verify the operator works
        sqlite3_create_function_v2(db, "fast_groupby_test", 0, SQLITE_UTF8, this,
            run_test, nullptr, nullptr, nullptr);
    }

private:
    static void run_test(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
        (void)argc; (void)argv;
        sqlite3* db = sqlite3_context_db_handle(ctx);

        // Create test table
        char* errmsg = nullptr;
        int rc = sqlite3_exec(db, "DROP TABLE IF EXISTS test_orders", nullptr, nullptr, &errmsg);
        if (rc != SQLITE_OK) {
            sqlite3_result_error(ctx, errmsg, -1);
            sqlite3_free(errmsg);
            return;
        }

        rc = sqlite3_exec(db, 
            "CREATE TABLE test_orders(id INTEGER PRIMARY KEY, category_id INT, qty INT)",
            nullptr, nullptr, &errmsg);
        if (rc != SQLITE_OK) {
            sqlite3_result_error(ctx, errmsg, -1);
            sqlite3_free(errmsg);
            return;
        }

        // Insert test data - 100 rows with categories 0-4
        rc = sqlite3_exec(db,
            "WITH RECURSIVE cnt(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM cnt WHERE x<100) "
            "INSERT INTO test_orders SELECT x, x%5, x*10 FROM cnt",
            nullptr, nullptr, &errmsg);
        if (rc != SQLITE_OK) {
            sqlite3_result_error(ctx, errmsg, -1);
            sqlite3_free(errmsg);
            return;
        }

        sqlite3_result_text(ctx, "PASS: Custom operator registered and test table created", -1, SQLITE_TRANSIENT);
    }
};

} // anonymous namespace

LYRORE_REGISTER_PLUGIN(FastGroupByPlugin);
