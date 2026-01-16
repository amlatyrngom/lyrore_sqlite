/*
** Simple test plugin for lazy pattern initialization
** This plugin:
** 1. Creates a pattern in onInit() (stores SQL only, no immediate parsing)
** 2. Uses the pattern in onPreOpt() (triggers lazy init)
*/

#include "lyrore_plugin.hpp"
#include "lyrore_pattern.hpp"
#include <iostream>

using namespace lyrore;
using namespace lyrore::pattern;

class LazyPatternTestPlugin : public Plugin {
    PatternPtr pattern_;
    int match_count_ = 0;
    int preopt_calls_ = 0;

public:
    std::string name() const override { return "lazy_pattern_test"; }

    void onInit(sqlite3* db) override {
        std::cerr << "[LazyTest] onInit called" << std::endl;

        // This should NOT crash - it just stores the SQL for later
        pattern_ = Pattern::from_query(db, "SELECT * FROM test_table WHERE value = ?");

        if (pattern_) {
            std::cerr << "[LazyTest] Pattern created (lazy), not initialized yet" << std::endl;
        } else {
            std::cerr << "[LazyTest] ERROR: Pattern creation returned nullptr" << std::endl;
        }
    }

    void onPreOpt(PreOptContext& ctx) override {
        preopt_calls_++;
        std::cerr << "[LazyTest] onPreOpt #" << preopt_calls_ << std::endl;

        if (!pattern_) {
            std::cerr << "[LazyTest] No pattern" << std::endl;
            return;
        }

        // First call to match() triggers lazy initialization
        auto m = pattern_->match(ctx.select_raw());

        if (m) {
            match_count_++;
            std::cerr << "[LazyTest] MATCH #" << match_count_ << std::endl;

            // Try to extract parameter
            int64_t val = m.get<int64_t>("$1");
            std::cerr << "[LazyTest] Extracted $1 = " << val << std::endl;
        } else {
            std::cerr << "[LazyTest] No match (pattern valid=" << pattern_->is_valid() << ")" << std::endl;
        }
    }

    void onShutdown() override {
        std::cerr << "[LazyTest] Shutdown. Total matches: " << match_count_ 
                  << ", PreOpt calls: " << preopt_calls_ << std::endl;
    }
};

LYRORE_REGISTER_PLUGIN(LazyPatternTestPlugin);
