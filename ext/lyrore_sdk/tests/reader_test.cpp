/*
** Reader Plugin - For shared resource testing
** Reads a shared value set by the setter plugin
*/
#include "lyrore_plugin.hpp"
#include "lyrore_main.hpp"
#include <sqlite3.h>
#include <memory>

using namespace lyrore;

// Must match setter's SharedValue definition
struct SharedValue {
    int64_t value;
    SharedValue(int64_t v = 0) : value(v) {}
};

class ReaderPlugin : public Plugin {
public:
    std::string name() const override { return "reader"; }

    void onInit(sqlite3* db) override {
        Plugin* self = this;
        sqlite3_create_function_v2(db, "get_shared", 0, SQLITE_UTF8, self,
            [](sqlite3_context* ctx, int, sqlite3_value**) {
                Plugin* plugin = static_cast<Plugin*>(sqlite3_user_data(ctx));

                if (plugin && plugin->lyrore_main()) {
                    auto shared = plugin->lyrore_main()->get_resource<SharedValue>("shared_value");
                    if (shared) {
                        sqlite3_result_int64(ctx, shared->value);
                    } else {
                        sqlite3_result_null(ctx);
                    }
                } else {
                    sqlite3_result_error(ctx, "LyroreMain not available", -1);
                }
            }, nullptr, nullptr, nullptr);
    }
};

LYRORE_REGISTER_PLUGIN(ReaderPlugin);
