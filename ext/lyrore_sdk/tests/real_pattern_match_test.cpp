/**
 * REAL Pattern Matching Tests
 * 
 * Key insight: onPreOpt hooks only work for queries executed AFTER plugin registration.
 * So we need to run test queries after plugin load is complete.
 * 
 * Test approach:
 * - onInit() creates tables and patterns, stores them for later use
 * - A special SQL function lyrore_run_pattern_tests() triggers the actual tests
 * - During test queries, onPreOpt is called, which verifies matching
 */

#include "lyrore_plugin.hpp"
#include "lyrore_pattern.hpp"
#include <iostream>
#include <vector>
#include <map>
#include <cmath>
#include <cstring>

using namespace lyrore;
using namespace lyrore::pattern;

// Global state
static sqlite3* g_db = nullptr;
static int g_tests_passed = 0;
static int g_tests_failed = 0;

// Test case definition
struct TestCase {
    std::string name;
    std::string pattern_sql;
    std::string test_query;
    bool expect_match;
    std::map<std::string, double> expect_int_params;
    std::map<std::string, double> expect_float_params;
    std::map<std::string, std::string> expect_str_params;
    bool is_where_only = false;
    
    // Results filled in by hook
    PatternPtr pattern;
    bool actual_matched = false;
    std::map<std::string, double> actual_num_params;
    std::map<std::string, std::string> actual_str_params;
    bool hook_invoked = false;
};

static std::vector<TestCase> g_tests;
static int g_current_test_idx = -1;
static bool g_test_mode = false;

static void exec(const char* sql) {
    char* err = nullptr;
    sqlite3_exec(g_db, sql, nullptr, nullptr, &err);
    if (err) {
        std::cerr << "SQL Error: " << err << " [" << sql << "]\n";
        sqlite3_free(err);
    }
}

static bool approx_eq(double a, double b, double eps = 0.001) {
    return std::abs(a - b) < eps;
}

static void report_result(const TestCase& tc) {
    bool passed = true;
    std::string details;
    
    if (!tc.hook_invoked) {
        passed = false;
        details = "Hook not invoked!";
    } else if (tc.expect_match != tc.actual_matched) {
        passed = false;
        details = "Match mismatch: expected=" + std::string(tc.expect_match ? "true" : "false") +
                 " actual=" + std::string(tc.actual_matched ? "true" : "false");
    } else if (tc.expect_match) {
        // Check numeric parameters
        for (auto& p : tc.expect_int_params) {
            auto it = tc.actual_num_params.find(p.first);
            if (it == tc.actual_num_params.end()) {
                passed = false;
                details = "Missing param " + p.first;
                break;
            }
            if (!approx_eq(it->second, p.second)) {
                passed = false;
                details = "Param " + p.first + ": expected=" + std::to_string(p.second) +
                         " actual=" + std::to_string(it->second);
                break;
            }
        }
        for (auto& p : tc.expect_float_params) {
            auto it = tc.actual_num_params.find(p.first);
            if (it == tc.actual_num_params.end()) {
                passed = false;
                details = "Missing float param " + p.first;
                break;
            }
            if (!approx_eq(it->second, p.second)) {
                passed = false;
                details = "Float param " + p.first + ": expected=" + std::to_string(p.second) +
                         " actual=" + std::to_string(it->second);
                break;
            }
        }
        // Check string parameters
        for (auto& p : tc.expect_str_params) {
            auto it = tc.actual_str_params.find(p.first);
            if (it == tc.actual_str_params.end()) {
                passed = false;
                details = "Missing string param " + p.first;
                break;
            }
            if (it->second != p.second) {
                passed = false;
                details = "String param " + p.first + ": expected='" + p.second +
                         "' actual='" + it->second + "'";
                break;
            }
        }
    }
    
    std::cout << "  " << tc.name << "... ";
    if (passed) {
        std::cout << "PASS";
        g_tests_passed++;
    } else {
        std::cout << "FAIL";
        g_tests_failed++;
    }
    
    // Show verification details on success too (to prove we're testing)
    if (passed && tc.expect_match) {
        std::cout << " (matched=true";
        for (auto& p : tc.actual_num_params) {
            std::cout << ", " << p.first << "=" << p.second;
        }
        for (auto& p : tc.actual_str_params) {
            std::cout << ", " << p.first << "='" << p.second << "'";
        }
        std::cout << ")";
    } else if (passed && !tc.expect_match) {
        std::cout << " (correctly rejected: matched=false)";
    }
    
    if (!passed && !details.empty()) {
        std::cout << " [" << details << "]";
    }
    std::cout << "\n" << std::flush;
}

// SQL function to run tests after plugin is loaded
static void run_tests_func(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
    (void)argc; (void)argv;
    
    std::cout << "\n========== REAL Pattern Match Tests ==========\n";
    std::cout << "These tests ACTUALLY call pattern->match() and verify extraction\n\n" << std::flush;

    std::cout << "--- Pattern Matching Tests (CALLING match()) ---\n" << std::flush;
    
    g_test_mode = true;
    g_tests_passed = 0;
    g_tests_failed = 0;
    
    for (size_t i = 0; i < g_tests.size(); i++) {
        TestCase& tc = g_tests[i];
        g_current_test_idx = static_cast<int>(i);
        
        // Reset test state
        tc.actual_matched = false;
        tc.actual_num_params.clear();
        tc.actual_str_params.clear();
        tc.hook_invoked = false;
        
        // Execute test query - this triggers onPreOpt hook
        exec(tc.test_query.c_str());
        
        // Report results
        report_result(tc);
    }
    
    g_test_mode = false;
    g_current_test_idx = -1;
    
    // Summary
    int total = g_tests_passed + g_tests_failed;
    std::cout << "\n==========================================\n";
    std::cout << "Results: " << g_tests_passed << "/" << total << " passed\n";
    if (g_tests_failed == 0) {
        std::cout << "ALL TESTS PASSED - Match verification confirmed!\n";
    } else {
        std::cout << "SOME TESTS FAILED!\n";
    }
    std::cout << "==========================================\n" << std::flush;
    
    sqlite3_result_text(ctx, "Tests completed", -1, SQLITE_TRANSIENT);
}

class RealPatternMatchTestPlugin : public Plugin {
public:
    std::string name() const override { return "real_pattern_match_test"; }

    void onPreOpt(PreOptContext& ctx) override {
        if (!g_test_mode || g_current_test_idx < 0 || 
            g_current_test_idx >= static_cast<int>(g_tests.size())) {
            return;
        }
        
        TestCase& tc = g_tests[g_current_test_idx];
        if (!tc.pattern || !tc.pattern->is_valid()) return;
        
        // ACTUALLY CALL match()!
        MatchResult m = tc.pattern->match(ctx.select_raw());
        
        tc.hook_invoked = true;
        tc.actual_matched = m.matched();
        
        if (m.matched()) {
            // Extract all parameters
            for (auto& p : m.params()) {
                if (auto* v = std::get_if<int64_t>(&p.second)) {
                    tc.actual_num_params[p.first] = static_cast<double>(*v);
                } else if (auto* v = std::get_if<double>(&p.second)) {
                    tc.actual_num_params[p.first] = *v;
                } else if (auto* v = std::get_if<std::string>(&p.second)) {
                    tc.actual_str_params[p.first] = *v;
                }
            }
        }
    }

    void onInit(sqlite3* db) override {
        g_db = db;
        g_tests.clear();

        // Setup schema FIRST
        exec("CREATE TABLE t(a INT, b REAL, c TEXT)");
        exec("INSERT INTO t VALUES (1, 10.5, 'hello')");
        exec("CREATE TABLE t2(a INT)");
        exec("INSERT INTO t2 VALUES (1)");
        exec("CREATE TABLE products(id INT, price REAL, qty INT)");
        exec("INSERT INTO products VALUES (1, 50.0, 10)");

        // Register test runner function
        sqlite3_create_function(db, "run_pattern_tests", 0, SQLITE_UTF8, nullptr,
                               run_tests_func, nullptr, nullptr);

        // Define all test cases
        
        // PM-01: Exact match with integer extraction
        {
            TestCase tc;
            tc.name = "PM-01: Exact match, extract integer 42";
            tc.pattern_sql = "SELECT * FROM t WHERE a = ?";
            tc.test_query = "SELECT * FROM t WHERE a = 42";
            tc.expect_match = true;
            tc.expect_int_params["$1"] = 42;
            tc.pattern = Pattern::from_query(db, tc.pattern_sql.c_str());
            g_tests.push_back(tc);
        }
        
        // PM-02: Alias tolerance
        {
            TestCase tc;
            tc.name = "PM-02: Alias tolerance (t AS x)";
            tc.pattern_sql = "SELECT * FROM t WHERE a = ?";
            tc.test_query = "SELECT * FROM t AS x WHERE x.a = 99";
            tc.expect_match = true;
            tc.expect_int_params["$1"] = 99;
            tc.pattern = Pattern::from_query(db, tc.pattern_sql.c_str());
            g_tests.push_back(tc);
        }
        
        // PM-03: Column mismatch
        {
            TestCase tc;
            tc.name = "PM-03: Column mismatch (pattern:a vs query:b)";
            tc.pattern_sql = "SELECT * FROM t WHERE a = ?";
            tc.test_query = "SELECT * FROM t WHERE b = 5.0";
            tc.expect_match = false;
            tc.pattern = Pattern::from_query(db, tc.pattern_sql.c_str());
            g_tests.push_back(tc);
        }
        
        // PM-04: Table mismatch
        {
            TestCase tc;
            tc.name = "PM-04: Table mismatch (pattern:t vs query:t2)";
            tc.pattern_sql = "SELECT * FROM t WHERE a = ?";
            tc.test_query = "SELECT * FROM t2 WHERE a = 5";
            tc.expect_match = false;
            tc.pattern = Pattern::from_query(db, tc.pattern_sql.c_str());
            g_tests.push_back(tc);
        }
        
        // PM-05: Operator mismatch
        {
            TestCase tc;
            tc.name = "PM-05: Operator mismatch (pattern:< vs query:>)";
            tc.pattern_sql = "SELECT * FROM t WHERE a < ?";
            tc.test_query = "SELECT * FROM t WHERE a > 5";
            tc.expect_match = false;
            tc.pattern = Pattern::from_query(db, tc.pattern_sql.c_str());
            g_tests.push_back(tc);
        }
        
        // PM-06: Extra predicate
        {
            TestCase tc;
            tc.name = "PM-06: Extra predicate mismatch";
            tc.pattern_sql = "SELECT * FROM t WHERE a = ?";
            tc.test_query = "SELECT * FROM t WHERE a = 5 AND b = 10.0";
            tc.expect_match = false;
            tc.pattern = Pattern::from_query(db, tc.pattern_sql.c_str());
            g_tests.push_back(tc);
        }
        
        // PM-07: Multi-param extraction
        {
            TestCase tc;
            tc.name = "PM-07: Multi-param extract ($1=10, $2=20.5)";
            tc.pattern_sql = "SELECT * FROM t WHERE a = ? AND b < ?";
            tc.test_query = "SELECT * FROM t WHERE a = 10 AND b < 20.5";
            tc.expect_match = true;
            tc.expect_int_params["$1"] = 10;
            tc.expect_float_params["$2"] = 20.5;
            tc.pattern = Pattern::from_query(db, tc.pattern_sql.c_str());
            g_tests.push_back(tc);
        }
        
        // PM-08: Float extraction
        {
            TestCase tc;
            tc.name = "PM-08: Float extraction (price < 99.95)";
            tc.pattern_sql = "SELECT * FROM products WHERE price < ?";
            tc.test_query = "SELECT * FROM products WHERE price < 99.95";
            tc.expect_match = true;
            tc.expect_float_params["$1"] = 99.95;
            tc.pattern = Pattern::from_query(db, tc.pattern_sql.c_str());
            g_tests.push_back(tc);
        }
        
        // PM-09: String extraction
        {
            TestCase tc;
            tc.name = "PM-09: String extraction (c='Alice')";
            tc.pattern_sql = "SELECT * FROM t WHERE c = ?";
            tc.test_query = "SELECT * FROM t WHERE c = 'Alice'";
            tc.expect_match = true;
            tc.expect_str_params["$1"] = "Alice";
            tc.pattern = Pattern::from_query(db, tc.pattern_sql.c_str());
            g_tests.push_back(tc);
        }
        
        // Column Binding Tests
        // CB-01: Same table same column
        {
            TestCase tc;
            tc.name = "CB-01: Same table/column matches";
            tc.pattern_sql = "SELECT * FROM t WHERE a = ?";
            tc.test_query = "SELECT * FROM t WHERE a = 777";
            tc.expect_match = true;
            tc.expect_int_params["$1"] = 777;
            tc.pattern = Pattern::from_query(db, tc.pattern_sql.c_str());
            g_tests.push_back(tc);
        }
        
        // CB-02: Same table different column
        {
            TestCase tc;
            tc.name = "CB-02: Same table, diff column -> no match";
            tc.pattern_sql = "SELECT * FROM t WHERE a = ?";
            tc.test_query = "SELECT * FROM t WHERE b = 999.0";
            tc.expect_match = false;
            tc.pattern = Pattern::from_query(db, tc.pattern_sql.c_str());
            g_tests.push_back(tc);
        }
        
        // CB-03: Different table same column name
        {
            TestCase tc;
            tc.name = "CB-03: Diff table, same col name -> no match";
            tc.pattern_sql = "SELECT * FROM t WHERE a = ?";
            tc.test_query = "SELECT * FROM t2 WHERE a = 888";
            tc.expect_match = false;
            tc.pattern = Pattern::from_query(db, tc.pattern_sql.c_str());
            g_tests.push_back(tc);
        }
        
        // CB-04: Aliased table
        {
            TestCase tc;
            tc.name = "CB-04: Aliased table still matches";
            tc.pattern_sql = "SELECT * FROM t WHERE a = ?";
            tc.test_query = "SELECT * FROM t AS alias WHERE alias.a = 555";
            tc.expect_match = true;
            tc.expect_int_params["$1"] = 555;
            tc.pattern = Pattern::from_query(db, tc.pattern_sql.c_str());
            g_tests.push_back(tc);
        }
        
        // Expression pattern test
        {
            TestCase tc;
            tc.name = "EXPR-01: Complex expression (price*qty) < 1000";
            tc.pattern_sql = "SELECT * FROM products WHERE (price * qty) < ?";
            tc.test_query = "SELECT * FROM products WHERE (price * qty) < 1000";
            tc.expect_match = true;
            tc.expect_int_params["$1"] = 1000;
            tc.pattern = Pattern::from_query(db, tc.pattern_sql.c_str());
            g_tests.push_back(tc);
        }

        std::cout << "Pattern Match Test Plugin loaded. Run: SELECT run_pattern_tests();\n" << std::flush;
    }
};

LYRORE_REGISTER_PLUGIN(RealPatternMatchTestPlugin);
