// Minimal test plugin - just check if plugin system works
#include "lyrore_plugin.hpp"
#include <iostream>

class SimpleTestPlugin : public lyrore::Plugin {
public:
    std::string name() const override { return "simple_test"; }
    
    void onInit(sqlite3* db) override {
        std::cout << "[SimpleTest] Plugin initialized with db=" << (void*)db << std::endl;
    }
    
    void onPreOpt(lyrore::PreOptContext& ctx) override {
        std::cout << "[SimpleTest] PreOpt called, select_raw=" << (void*)ctx.select_raw() << std::endl;
    }
    
    void onShutdown() override {
        std::cout << "[SimpleTest] Plugin shutdown" << std::endl;
    }
};

LYRORE_REGISTER_PLUGIN(SimpleTestPlugin);
