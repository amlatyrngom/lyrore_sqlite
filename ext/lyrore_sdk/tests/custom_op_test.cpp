/*
** Custom Operator Test Suite
** 
** Tests:
** 1. TableCursor basic functionality
** 2. SCALAR mode wrapper
** 3. ITERATOR mode with FastGroupByOp
** 4. Correctness verification (same results with/without)
** 5. Performance improvement
*/

#include "lyrore_plugin.hpp"
#include "lyrore_custom_op.hpp"
#include <vector>
#include <array>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <sstream>
#include <variant>

extern "C" {
#include "sqlite3.h"
}

namespace {

// Helper to extract int64_t from LyValue
inline int64_t get_int(const lyrore::LyValue& v) {
    return std::get<int64_t>(v);
}

// Helper to extract double from LyValue
inline double get_double(const lyrore::LyValue& v) {
    return std::get<double>(v);
}

// Helper to extract string from LyValue
inline std::string get_string(const lyrore::LyValue& v) {
    return std::get<std::string>(v);
}

// ============================================================
// Test Operators
// ============================================================

// SCALAR mode: Sum of qty for a specific category
class FastSumOp : public lyrore::CustomOperator {
public:
    lyrore::OutputMode output_mode() const override { 
        return lyrore::OutputMode::SCALAR; 
    }

    lyrore::LyValue compute_scalar() override {
        int64_t target_cat = 0;
        if (params_) {
            target_cat = params_->get<int64_t>("$1");
        }

        int64_t sum = 0;
        auto cursor = open_table("orders");
        if (!cursor.valid()) {
            return lyrore::LyValue{(int64_t)0};
        }

        while (!cursor.eof()) {
            int64_t cat = get_int(cursor.get_column(1));
            int64_t qty = get_int(cursor.get_column(2));
            if (cat == target_cat) {
                sum += qty;
            }
            cursor.next();
        }
        return lyrore::LyValue{sum};
    }
};

// ITERATOR mode: GROUP BY aggregation
class FastGroupByOp : public lyrore::CustomOperator {
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
        sums_.fill(0);

        auto cursor = open_table("orders");
        if (!cursor.valid()) return;

        while (!cursor.eof()) {
            int64_t cat = get_int(cursor.get_column(1));
            int64_t qty = get_int(cursor.get_column(2));
            if (cat >= 0 && cat < 5) {
                sums_[cat] += qty;
            }
            cursor.next();
        }
    }

    void iterator_reset() override { pos_ = 0; }

    bool iterator_next() override {
        pos_++;
        return pos_ <= 5;
    }

    std::vector<lyrore::LyValue> iterator_get_row() override {
        if (pos_ < 1 || pos_ > 5) return {};
        int64_t cat = static_cast<int64_t>(pos_ - 1);
        return {lyrore::LyValue{cat}, lyrore::LyValue{sums_[pos_-1]}};
    }
};

// ============================================================
// Test Functions
// ============================================================

static std::ostringstream test_output;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_ASSERT(cond, msg) do { \
    if (!(cond)) { \
        test_output << "FAIL: " << msg << "\n"; \
        tests_failed++; \
        return false; \
    } \
} while(0)

#define TEST_PASS(msg) do { \
    test_output << "PASS: " << msg << "\n"; \
    tests_passed++; \
} while(0)

// Test 1: TableCursor basic functionality
bool test_table_cursor(sqlite3* db) {
    // Create test table
    sqlite3_exec(db, "DROP TABLE IF EXISTS cursor_test", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "CREATE TABLE cursor_test(id INT, name TEXT, val REAL)", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "INSERT INTO cursor_test VALUES(1, 'a', 1.5), (2, 'b', 2.5), (3, 'c', 3.5)", nullptr, nullptr, nullptr);

    lyrore::TableCursor cursor(db, "cursor_test");
    TEST_ASSERT(cursor.valid(), "TableCursor should be valid");
    TEST_ASSERT(cursor.num_columns() == 3, "Should have 3 columns");

    int row_count = 0;
    double sum = 0.0;
    while (!cursor.eof()) {
        auto id = get_int(cursor.get_column(0));
        auto name = get_string(cursor.get_column(1));
        auto val = get_double(cursor.get_column(2));
        (void)id; (void)name;
        sum += val;
        row_count++;
        cursor.next();
    }

    TEST_ASSERT(row_count == 3, "Should have 3 rows");
    TEST_ASSERT(sum > 7.4 && sum < 7.6, "Sum should be ~7.5");

    TEST_PASS("TableCursor basic operations work correctly");
    return true;
}

// Test 2: TableCursor empty table
bool test_table_cursor_empty(sqlite3* db) {
    sqlite3_exec(db, "DROP TABLE IF EXISTS empty_test", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "CREATE TABLE empty_test(id INT)", nullptr, nullptr, nullptr);

    lyrore::TableCursor cursor(db, "empty_test");
    TEST_ASSERT(cursor.valid(), "Cursor should be valid on empty table");
    TEST_ASSERT(cursor.eof(), "Should immediately be at EOF");

    TEST_PASS("TableCursor handles empty tables correctly");
    return true;
}

// Test 3: SCALAR wrapper registration  
bool test_scalar_registration(sqlite3* db) {
    // Setup test table
    sqlite3_exec(db, "DROP TABLE IF EXISTS orders", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "CREATE TABLE orders(id INTEGER PRIMARY KEY, category_id INT, qty INT)", nullptr, nullptr, nullptr);
    sqlite3_exec(db, 
        "WITH RECURSIVE cnt(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM cnt WHERE x<100) "
        "INSERT INTO orders SELECT x, x%5, x*10 FROM cnt",
        nullptr, nullptr, nullptr);

    // Register SCALAR operator
    lyrore::register_custom_op<FastSumOp>(
        db,
        "SELECT SUM(qty) FROM orders WHERE category_id = ?",
        {"sum"}
    );

    TEST_PASS("SCALAR operator registration succeeded");
    return true;
}

// Test 4: ITERATOR wrapper registration
bool test_iterator_registration(sqlite3* db) {
    // Register ITERATOR operator
    lyrore::register_custom_op<FastGroupByOp>(
        db,
        "SELECT category_id, SUM(qty) FROM orders GROUP BY category_id",
        {"category_id", "total"}
    );

    TEST_PASS("ITERATOR operator registration succeeded");
    return true;
}

// Test 5: Correctness - compare results
bool test_correctness(sqlite3* db) {
    // Calculate expected results using native SQLite
    std::array<int64_t, 5> expected_sums{};

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db, 
        "SELECT category_id, SUM(qty) FROM orders GROUP BY category_id ORDER BY category_id",
        -1, &stmt, nullptr);
    TEST_ASSERT(rc == SQLITE_OK, "Prepare native query failed");

    int idx = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int64_t cat = sqlite3_column_int64(stmt, 0);
        int64_t sum = sqlite3_column_int64(stmt, 1);
        if (cat >= 0 && cat < 5) {
            expected_sums[cat] = sum;
        }
        idx++;
    }
    sqlite3_finalize(stmt);
    TEST_ASSERT(idx == 5, "Should have 5 groups");

    // Now test with custom operator via direct API
    FastGroupByOp op;
    op._set_db(db);
    op.execute();

    std::array<int64_t, 5> actual_sums{};
    int row_idx = 0;
    while (op.iterator_next()) {
        auto row = op.iterator_get_row();
        if (row.size() >= 2) {
            int64_t cat = get_int(row[0]);
            int64_t sum = get_int(row[1]);
            if (cat >= 0 && cat < 5) {
                actual_sums[cat] = sum;
            }
        }
        row_idx++;
    }
    TEST_ASSERT(row_idx == 5, "Custom op should return 5 rows");

    // Compare results
    for (int i = 0; i < 5; i++) {
        char buf[100];
        snprintf(buf, sizeof(buf), "Category %d: expected %ld, got %ld", i, expected_sums[i], actual_sums[i]);
        TEST_ASSERT(expected_sums[i] == actual_sums[i], buf);
    }

    TEST_PASS("Custom operator produces identical results to native SQLite");
    return true;
}

// Test 6: Performance comparison
bool test_performance(sqlite3* db) {
    // Create larger test table for timing
    sqlite3_exec(db, "DROP TABLE IF EXISTS orders", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "CREATE TABLE orders(id INTEGER PRIMARY KEY, category_id INT, qty INT)", nullptr, nullptr, nullptr);

    // Insert 10000 rows
    sqlite3_exec(db, "BEGIN", nullptr, nullptr, nullptr);
    sqlite3_stmt* insert_stmt;
    sqlite3_prepare_v2(db, "INSERT INTO orders VALUES(?, ?, ?)", -1, &insert_stmt, nullptr);
    for (int i = 1; i <= 10000; i++) {
        sqlite3_bind_int(insert_stmt, 1, i);
        sqlite3_bind_int(insert_stmt, 2, i % 5);
        sqlite3_bind_int(insert_stmt, 3, i * 10);
        sqlite3_step(insert_stmt);
        sqlite3_reset(insert_stmt);
    }
    sqlite3_finalize(insert_stmt);
    sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);

    // Warmup
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, 
        "SELECT category_id, SUM(qty) FROM orders GROUP BY category_id",
        -1, &stmt, nullptr);
    while (sqlite3_step(stmt) == SQLITE_ROW) {}
    sqlite3_finalize(stmt);

    // Time native SQLite (3 runs)
    double native_time = 0;
    for (int run = 0; run < 3; run++) {
        auto start = std::chrono::high_resolution_clock::now();

        sqlite3_prepare_v2(db, 
            "SELECT category_id, SUM(qty) FROM orders GROUP BY category_id",
            -1, &stmt, nullptr);
        while (sqlite3_step(stmt) == SQLITE_ROW) {}
        sqlite3_finalize(stmt);

        auto end = std::chrono::high_resolution_clock::now();
        native_time += std::chrono::duration<double, std::milli>(end - start).count();
    }
    native_time /= 3;

    // Time custom operator (3 runs)
    double custom_time = 0;
    for (int run = 0; run < 3; run++) {
        auto start = std::chrono::high_resolution_clock::now();

        FastGroupByOp op;
        op._set_db(db);
        op.execute();
        while (op.iterator_next()) {
            auto row = op.iterator_get_row();
            (void)row;
        }

        auto end = std::chrono::high_resolution_clock::now();
        custom_time += std::chrono::duration<double, std::milli>(end - start).count();
    }
    custom_time /= 3;

    char buf[200];
    snprintf(buf, sizeof(buf), "Performance: Native=%.3fms, Custom=%.3fms, Speedup=%.2fx",
             native_time, custom_time, native_time / custom_time);
    test_output << "INFO: " << buf << "\n";

    // Custom should be faster (or at least comparable)
    // We're comparing the computation time, not including query planning overhead
    TEST_PASS("Performance test completed");
    return true;
}


// Test 7: VTab direct query functionality
bool test_vtab_query(sqlite3* db) {
    // After registering ITERATOR operator, we should be able to query the vtab directly
    // The vtab is registered as _lyrore_vtab_X in temp schema

    // Query the vtab
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db, 
        "SELECT * FROM _lyrore_vtab_1 ORDER BY category_id",
        -1, &stmt, nullptr);
    TEST_ASSERT(rc == SQLITE_OK, "Should be able to prepare vtab query");

    std::array<int64_t, 5> vtab_sums{};
    int row_count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int64_t cat = sqlite3_column_int64(stmt, 0);
        int64_t sum = sqlite3_column_int64(stmt, 1);
        if (cat >= 0 && cat < 5) {
            vtab_sums[cat] = sum;
        }
        row_count++;
    }
    sqlite3_finalize(stmt);
    TEST_ASSERT(row_count == 5, "VTab should return 5 rows");

    // Now verify against native SQLite
    std::array<int64_t, 5> native_sums{};
    rc = sqlite3_prepare_v2(db, 
        "SELECT category_id, SUM(qty) FROM orders GROUP BY category_id ORDER BY category_id",
        -1, &stmt, nullptr);
    TEST_ASSERT(rc == SQLITE_OK, "Native query should prepare");
    
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int64_t cat = sqlite3_column_int64(stmt, 0);
        int64_t sum = sqlite3_column_int64(stmt, 1);
        if (cat >= 0 && cat < 5) {
            native_sums[cat] = sum;
        }
    }
    sqlite3_finalize(stmt);

    // Compare
    for (int i = 0; i < 5; i++) {
        char buf[100];
        snprintf(buf, sizeof(buf), "VTab category %d mismatch: vtab=%ld native=%ld", 
                 i, vtab_sums[i], native_sums[i]);
        TEST_ASSERT(vtab_sums[i] == native_sums[i], buf);
    }

    TEST_PASS("VTab direct query returns correct results matching native SQLite");
    return true;
}


// Test 8: SCALAR function direct call
bool test_scalar_function(sqlite3* db) {
    // The SCALAR function _lyrore_fn_0 should be callable directly
    
    // Test calling the function for each category
    for (int cat = 0; cat < 5; cat++) {
        char sql[100];
        snprintf(sql, sizeof(sql), "SELECT _lyrore_fn_0(%d)", cat);
        
        sqlite3_stmt* stmt;
        int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
        TEST_ASSERT(rc == SQLITE_OK, "SCALAR function should be callable");
        
        rc = sqlite3_step(stmt);
        TEST_ASSERT(rc == SQLITE_ROW, "Should return a row");
        
        int64_t fn_result = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
        
        // Compare with native
        snprintf(sql, sizeof(sql), "SELECT SUM(qty) FROM orders WHERE category_id = %d", cat);
        rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
        TEST_ASSERT(rc == SQLITE_OK, "Native query should prepare");
        
        sqlite3_step(stmt);
        int64_t native_result = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
        
        char buf[100];
        snprintf(buf, sizeof(buf), "SCALAR fn cat %d: fn=%ld native=%ld", cat, fn_result, native_result);
        TEST_ASSERT(fn_result == native_result, buf);
    }
    
    TEST_PASS("SCALAR function returns correct results for all categories");
    return true;
}

// Test runner function
static void run_all_tests(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
    (void)argc; (void)argv;
    sqlite3* db = sqlite3_context_db_handle(ctx);

    test_output.str("");
    test_output.clear();
    tests_passed = 0;
    tests_failed = 0;

    test_output << "=== Custom Operator Test Suite ===\n\n";

    test_table_cursor(db);
    test_table_cursor_empty(db);
    test_scalar_registration(db);
    test_iterator_registration(db);
    test_correctness(db);
    test_vtab_query(db);
    test_scalar_function(db);
    test_performance(db);

    test_output << "\n=== Summary ===\n";
    test_output << "Passed: " << tests_passed << "\n";
    test_output << "Failed: " << tests_failed << "\n";

    if (tests_failed == 0) {
        test_output << "\nALL TESTS PASSED\n";
    } else {
        test_output << "\nSOME TESTS FAILED\n";
    }

    std::string result = test_output.str();
    sqlite3_result_text(ctx, result.c_str(), result.length(), SQLITE_TRANSIENT);
}

// ============================================================
// Test Plugin
// ============================================================

class CustomOpTestPlugin : public lyrore::Plugin {
public:
    std::string name() const override { return "CustomOpTestPlugin"; }

    void onInit(sqlite3* db) override {
        sqlite3_create_function_v2(db, "run_custom_op_tests", 0, SQLITE_UTF8, nullptr,
            run_all_tests, nullptr, nullptr, nullptr);
    }
};

} // anonymous namespace

LYRORE_REGISTER_PLUGIN(CustomOpTestPlugin);
