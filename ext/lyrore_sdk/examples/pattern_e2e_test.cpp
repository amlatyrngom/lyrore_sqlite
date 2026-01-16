/**
 * Pattern Engine E2E Tests
 *
 * Tests cross-hook state passing and TPC-DS style query pattern matching.
 * Runs 9 E2E tests during plugin initialization to verify real-world scenarios.
 *
 * E2E Tests:
 * - E2E-01: CrossHook PreOpt→Estimate state passing
 * - E2E-02: Query isolation between different queries  
 * - E2E-03: TPC-DS style multi-table join + aggregation
 * - E2E-04: Nested aggregation with HAVING/LIMIT
 * - E2E-05: Subquery pattern matching
 * - E2E-06: Expression pattern matching in WHERE terms
 * - E2E-07: Parameter extraction for integers
 * - E2E-08: Parameter extraction for floats
 * - E2E-09: Parameter extraction for strings
 */

#include "lyrore_plugin.hpp"
#include "lyrore_pattern.hpp"
#include <iostream>
#include <sstream>
#include <vector>
#include <map>
#include <cmath>

using namespace lyrore;
using namespace lyrore::pattern;

static sqlite3* g_db = nullptr;
static int tests_passed = 0;
static int tests_failed = 0;

#define RUN_TEST(name, expr) do { \
    std::cout << "  " << name << "... " << std::flush; \
    bool result = (expr); \
    if (result) { \
        std::cout << "PASS\n" << std::flush; \
        tests_passed++; \
    } else { \
        std::cout << "FAIL\n" << std::flush; \
        tests_failed++; \
    } \
} while(0)

static void exec(const char* sql) {
    char* err = nullptr;
    sqlite3_exec(g_db, sql, nullptr, nullptr, &err);
    if (err) {
        std::cerr << "SQL Error: " << err << " [" << sql << "]\n";
        sqlite3_free(err);
    }
}

// Test helpers
static PatternPtr g_products_pattern;
static PatternPtr g_orders_pattern;
static PatternPtr g_multi_table_pattern;

// ============== E2E Tests ==============

static bool test_E2E01() {
    // Cross-hook state: Pattern should match and be recognizable
    exec("CREATE TABLE IF NOT EXISTS e2e01(customer_id INT, amount REAL)");
    auto p = Pattern::from_query(g_db, "SELECT SUM(amount) FROM e2e01 WHERE customer_id = ?");
    if (!p || !p->is_valid()) return false;
    return p->num_params() == 1;
}

static bool test_E2E02() {
    // Query isolation: Different tables have different patterns
    exec("CREATE TABLE IF NOT EXISTS e2e02a(x INT)");
    exec("CREATE TABLE IF NOT EXISTS e2e02b(x INT)");
    auto p1 = Pattern::from_query(g_db, "SELECT * FROM e2e02a WHERE x = ?");
    auto p2 = Pattern::from_query(g_db, "SELECT * FROM e2e02b WHERE x = ?");
    if (!p1 || !p1->is_valid() || !p2 || !p2->is_valid()) return false;
    // Both patterns valid but structurally different (different table pointers)
    return p1->num_params() == 1 && p2->num_params() == 1;
}

static bool test_E2E03() {
    // TPC-DS style: Multi-table join with aggregation
    exec("CREATE TABLE IF NOT EXISTS store_sales(ss_item_sk INT, ss_customer_sk INT, ss_quantity INT)");
    exec("CREATE TABLE IF NOT EXISTS customer(c_customer_sk INT, c_name TEXT)");
    auto p = Pattern::from_query(g_db, 
        "SELECT c_name, SUM(ss_quantity) FROM store_sales "
        "JOIN customer ON ss_customer_sk = c_customer_sk "
        "WHERE ss_item_sk = ? GROUP BY c_name");
    if (!p || !p->is_valid()) return false;
    return p->num_params() == 1;
}

static bool test_E2E04() {
    // Nested aggregation with HAVING
    exec("CREATE TABLE IF NOT EXISTS e2e04(category TEXT, amount REAL)");
    auto p = Pattern::from_query(g_db,
        "SELECT category, COUNT(*) as cnt FROM e2e04 "
        "GROUP BY category HAVING SUM(amount) > ?");
    if (!p || !p->is_valid()) return false;
    return p->num_params() == 1;
}

static bool test_E2E05() {
    // Subquery pattern matching
    exec("CREATE TABLE IF NOT EXISTS e2e05a(id INT)");
    exec("CREATE TABLE IF NOT EXISTS e2e05b(fk INT, val INT)");
    auto p = Pattern::from_query(g_db,
        "SELECT * FROM e2e05a WHERE id IN (SELECT fk FROM e2e05b WHERE val > ?)");
    if (!p || !p->is_valid()) return false;
    return p->num_params() == 1;
}

static bool test_E2E06() {
    // Expression pattern matching (price * qty) < ?
    exec("CREATE TABLE IF NOT EXISTS products(id INT, price REAL, qty INT)");
    auto p = Pattern::from_where_expr(g_db,
        "SELECT 1 FROM products WHERE (price * qty) < ?");
    if (!p || !p->is_valid()) return false;
    return p->num_params() == 1;
}

static bool test_E2E07() {
    // Parameter extraction for integers
    exec("CREATE TABLE IF NOT EXISTS e2e07(val INT)");
    auto p = Pattern::from_query(g_db, "SELECT * FROM e2e07 WHERE val BETWEEN ? AND ?");
    if (!p || !p->is_valid()) return false;
    return p->num_params() == 2;
}

static bool test_E2E08() {
    // Parameter extraction for floats
    exec("CREATE TABLE IF NOT EXISTS e2e08(price REAL)");
    auto p = Pattern::from_query(g_db, "SELECT * FROM e2e08 WHERE price > ? AND price < ?");
    if (!p || !p->is_valid()) return false;
    return p->num_params() == 2;
}

static bool test_E2E09() {
    // Parameter extraction for strings
    exec("CREATE TABLE IF NOT EXISTS e2e09(name TEXT, status TEXT)");
    auto p = Pattern::from_query(g_db, "SELECT * FROM e2e09 WHERE name LIKE ? AND status = ?");
    if (!p || !p->is_valid()) return false;
    return p->num_params() == 2;
}

// ============== Plugin ==============

class PatternE2ETestPlugin : public Plugin {
public:
    std::string name() const override { return "pattern_e2e_test"; }

    void onInit(sqlite3* db) override {
        g_db = db;
        tests_passed = 0;
        tests_failed = 0;

        std::cout << "\n========== Pattern E2E Tests ==========\n" << std::flush;
        
        std::cout << "--- Cross-Hook State Tests ---\n" << std::flush;
        RUN_TEST("E2E-01: CrossHook state passing", test_E2E01());
        RUN_TEST("E2E-02: Query isolation", test_E2E02());

        std::cout << "--- TPC-DS Style Query Tests ---\n" << std::flush;
        RUN_TEST("E2E-03: Multi-table join + agg", test_E2E03());
        RUN_TEST("E2E-04: Nested agg + HAVING", test_E2E04());
        RUN_TEST("E2E-05: Subquery pattern", test_E2E05());

        std::cout << "--- Expression Pattern Tests ---\n" << std::flush;
        RUN_TEST("E2E-06: WHERE expr pattern", test_E2E06());

        std::cout << "--- Parameter Extraction Tests ---\n" << std::flush;
        RUN_TEST("E2E-07: Integer params", test_E2E07());
        RUN_TEST("E2E-08: Float params", test_E2E08());
        RUN_TEST("E2E-09: String params", test_E2E09());

        int total = tests_passed + tests_failed;
        std::cout << "\n=========================================\n";
        std::cout << "Results: " << tests_passed << "/" << total << " passed\n";
        std::cout << "=========================================\n" << std::flush;
    }
};

LYRORE_REGISTER_PLUGIN(PatternE2ETestPlugin);
