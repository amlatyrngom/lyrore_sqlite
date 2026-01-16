// Test pattern creation specifically
#include "lyrore_plugin.hpp"
#include "lyrore_pattern.hpp"
#include <iostream>

using namespace lyrore;
using namespace lyrore::pattern;

class PatternCreateTestPlugin : public Plugin {
public:
    std::string name() const override { return "pattern_create_test"; }
    
    void onInit(sqlite3* db) override {
        std::cout << "[PatternCreate] Testing pattern creation..." << std::endl;
        std::cout << "[PatternCreate] db=" << (void*)db << std::endl;
        
        // Test 1: Check if capture state works
        std::cout << "[PatternCreate] capture_mode before: " << PatternCaptureState::capture_mode << std::endl;
        
        // Test 2: Try to create a simple pattern
        std::cout << "[PatternCreate] Calling Pattern::from_query..." << std::endl;
        
        auto pat = Pattern::from_query(db, "SELECT * FROM test_table WHERE a = ?");
        
        std::cout << "[PatternCreate] Pattern created: " << (pat ? "SUCCESS" : "FAILED") << std::endl;
        
        if (pat) {
            std::cout << "[PatternCreate] Pattern has " << pat->num_params() << " parameters" << std::endl;
        }
    }
    
    void onShutdown() override {
        std::cout << "[PatternCreate] Shutdown" << std::endl;
    }
};

LYRORE_REGISTER_PLUGIN(PatternCreateTestPlugin);
