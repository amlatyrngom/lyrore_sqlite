/*
** Minimal Pattern Debug Test
*/

#include "lyrore_plugin.hpp"
#include "lyrore_pattern.hpp"
#include <iostream>

using namespace lyrore;
using namespace lyrore::pattern;

class PatternDebugPlugin : public Plugin {
public:
    std::string name() const override { return "pattern_debug"; }

    void onInit(sqlite3* db) override {
        std::cout << "PatternDebugPlugin::onInit starting..." << std::endl;

        // Create a simple table first
        char* err = nullptr;
        int rc = sqlite3_exec(db, "CREATE TABLE debug_t(a INT)", nullptr, nullptr, &err);
        if (rc != SQLITE_OK) {
            std::cout << "Table create failed: " << (err ? err : "unknown") << std::endl;
            if (err) sqlite3_free(err);
        } else {
            std::cout << "Table created successfully" << std::endl;
        }

        // Try to create a pattern
        std::cout << "Creating pattern..." << std::endl;
        auto pattern = Pattern::from_query(db, "SELECT * FROM debug_t WHERE a = ?");

        if (!pattern) {
            std::cout << "Pattern creation returned nullptr" << std::endl;
            return;
        }
        std::cout << "Pattern object created (lazy init pending)" << std::endl;

        // Try to access pattern (triggers lazy init)
        std::cout << "Checking is_valid()..." << std::endl;
        bool valid = pattern->is_valid();
        std::cout << "Pattern valid: " << (valid ? "YES" : "NO") << std::endl;

        if (valid) {
            std::cout << "num_params: " << pattern->num_params() << std::endl;
        }

        std::cout << "PatternDebugPlugin::onInit completed" << std::endl;
    }
};

LYRORE_REGISTER_PLUGIN(PatternDebugPlugin);
