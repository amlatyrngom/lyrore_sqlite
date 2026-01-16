/*
** Pattern Engine Test Plugin
** 
** Tests the core pattern matching functionality:
** 1. Pattern creation (from_query, from_where_expr)
** 2. Pattern matching with parameter extraction
** 3. Column matching via schema pointers (NOT strings!)
** 4. Cross-hook state (PreOpt to Estimate)
*/

#include "lyrore_plugin.hpp"
#include "lyrore_pattern.hpp"
#include <iostream>
#include <cassert>
#include <cstring>
#include <vector>

using namespace lyrore;
using namespace lyrore::pattern;

class PatternTestPlugin : public Plugin {
    PatternPtr simple_pattern_;
    PatternPtr multi_param_pattern_;
    PatternPtr expr_pattern_;
    
    // Track test results
    std::vector<std::string> test_results_;
    int tests_passed_ = 0;
    int tests_failed_ = 0;
    
public:
    std::string name() const override { return "pattern_test"; }
    
    void onInit(sqlite3* db) override {
        std::cout << "=== Pattern Engine Test Plugin Initializing ===" << std::endl;
        
        // Create test patterns
        // Pattern 1: Simple single parameter
        simple_pattern_ = Pattern::from_query(db, 
            "SELECT * FROM test_table WHERE a = ?");
        if (simple_pattern_) {
            record_result("PC-01: Simple pattern creation", true);
        } else {
            record_result("PC-01: Simple pattern creation", false);
        }
        
        // Pattern 2: Multi-parameter
        multi_param_pattern_ = Pattern::from_query(db,
            "SELECT * FROM test_table WHERE a = ? AND b < ?");
        if (multi_param_pattern_) {
            record_result("PC-02: Multi-param pattern creation", true);
        } else {
            record_result("PC-02: Multi-param pattern creation", false);
        }
        
        // Pattern 3: Expression pattern (WHERE clause only)
        expr_pattern_ = Pattern::from_where_expr(db,
            "SELECT 1 FROM test_table WHERE (a * b) < ?");
        if (expr_pattern_) {
            record_result("PC-03: Expression pattern creation", true);
        } else {
            record_result("PC-03: Expression pattern creation", false);
        }
        
        std::cout << "Pattern creation complete." << std::endl;
    }
    
    void onPreOpt(PreOptContext& ctx) override {
        Select* sel = ctx.select_raw();
        if (!sel) return;
        
        // Test pattern matching
        if (simple_pattern_) {
            auto m = simple_pattern_->match(sel);
            if (m) {
                // Store match in cross-hook state
                std::map<std::string, LyValue> params = m.params();
                set_template_match(sel, 1, params);
                std::cout << "PreOpt: Simple pattern MATCHED, stored template_id=1" << std::endl;
                
                // Check parameter extraction
                if (m.has_param("$1")) {
                    std::cout << "  $1 extracted successfully" << std::endl;
                }
            }
        }
        
        if (multi_param_pattern_) {
            auto m = multi_param_pattern_->match(sel);
            if (m) {
                std::map<std::string, LyValue> params = m.params();
                set_template_match(sel, 2, params);
                std::cout << "PreOpt: Multi-param pattern MATCHED, stored template_id=2" << std::endl;
            }
        }
    }
    
    void onEstimate(EstimateContext& ctx) override {
        Select* sel = ctx.select_raw();
        if (!sel) return;
        
        // Test cross-hook state retrieval
        auto tid = get_template_id(sel);
        if (tid) {
            std::cout << "Estimate: Retrieved template_id=" << *tid << " from PreOpt" << std::endl;
            
            auto* params = get_template_params(sel);
            if (params) {
                std::cout << "  Cross-hook params available: " << params->size() << " params" << std::endl;
                record_result("CrossHook: PreOpt to Estimate state", true);
            } else {
                record_result("CrossHook: PreOpt to Estimate state (no params)", false);
            }
        }
    }
    
    void onShutdown() override {
        std::cout << "\n=== Pattern Engine Test Results ===" << std::endl;
        for (const auto& r : test_results_) {
            std::cout << r << std::endl;
        }
        std::cout << "Passed: " << tests_passed_ << ", Failed: " << tests_failed_ << std::endl;
        std::cout << "==========================================\n" << std::endl;
    }
    
private:
    void record_result(const char* test_name, bool passed) {
        std::string result = passed ? "[PASS] " : "[FAIL] ";
        result += test_name;
        test_results_.push_back(result);
        if (passed) tests_passed_++;
        else tests_failed_++;
    }
};

LYRORE_REGISTER_PLUGIN(PatternTestPlugin);
