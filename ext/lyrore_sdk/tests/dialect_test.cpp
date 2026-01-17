/*
** Dialect Plugin - For pre-parse hook testing
** Transforms MY_SUM(x) to SUM(x) in SQL strings
*/
#include "lyrore_plugin.hpp"
#include <string>
#include <regex>

using namespace lyrore;

class DialectPlugin : public Plugin {
public:
    std::string name() const override { return "dialect"; }

    void onPreParse(PreParseContext& ctx) override {
        std::string sql = ctx.sql();
        // Transform MY_SUM to SUM
        std::regex my_sum_re("MY_SUM\\(", std::regex_constants::icase);
        std::string transformed = std::regex_replace(sql, my_sum_re, "SUM(");
        if (transformed != sql) {
            ctx.set_sql(transformed);
        }
    }
};

LYRORE_REGISTER_PLUGIN(DialectPlugin);
