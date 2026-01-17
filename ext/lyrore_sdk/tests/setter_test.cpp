/*
** Setter Plugin - For shared resource testing
** Sets a shared value that can be read by another plugin
*/
#include "lyrore_plugin.hpp"
#include "lyrore_main.hpp"
#include <sqlite3.h>
#include <memory>

using namespace lyrore;

// Shared value wrapper
struct SharedValue {
    int64_t value;
    SharedValue(int64_t v = 0) : value(v) {}
};

class SetterPlugin : public Plugin {
public:
    std::string name() const override { return "setter"; }

    void onInit(sqlite3* db) override {
        // Capture 'this' to access lyrore_main()
        Plugin* self = this;
        sqlite3_create_function_v2(db, "set_shared", 1, SQLITE_UTF8, self,
            [](sqlite3_context* ctx, int, sqlite3_value** argv) {
                Plugin* plugin = static_cast<Plugin*>(sqlite3_user_data(ctx));
                int64_t val = sqlite3_value_int64(argv[0]);

                if (plugin && plugin->lyrore_main()) {
                    auto shared = std::make_shared<SharedValue>(val);
                    plugin->lyrore_main()->set_resource<SharedValue>("shared_value", shared);
                    sqlite3_result_int64(ctx, val);
                } else {
                    sqlite3_result_error(ctx, "LyroreMain not available", -1);
                }
            }, nullptr, nullptr, nullptr);
    }
};

LYRORE_REGISTER_PLUGIN(SetterPlugin);
