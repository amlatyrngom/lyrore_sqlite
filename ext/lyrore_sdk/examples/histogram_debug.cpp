/*
** Histogram Plugin with Debug Output
*/

#include "lyrore_plugin.hpp"
#include <vector>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <cstdio>

class HistogramDebugPlugin : public lyrore::Plugin {
private:
    static constexpr int NUM_BUCKETS = 100;
    std::vector<int64_t> buckets_;
    int64_t total_ = 0;
    double threshold_ = 0;

public:
    std::string name() const override { return "histogram_debug"; }

    void onInit(sqlite3* db) override {
        buckets_.resize(NUM_BUCKETS, 0);
        if (const char* t = std::getenv("HIST_THRESHOLD")) {
            threshold_ = std::atof(t);
            fprintf(stderr, "[histogram] onInit: threshold=%.0f\n", threshold_);
        } else {
            fprintf(stderr, "[histogram] onInit: no HIST_THRESHOLD env var\n");
        }
    }

    void onAnalyze(lyrore::AnalyzeContext& ctx) override {
        fprintf(stderr, "[histogram] onAnalyze called\n");
        std::fill(buckets_.begin(), buckets_.end(), 0);
        total_ = 0;

        auto rs = ctx.query("SELECT price * qty FROM products");
        while (rs && rs->next()) {
            double v = rs->get_double(0);
            int idx = std::min((int)(v / 1200.0), NUM_BUCKETS - 1);
            if (idx >= 0) {
                buckets_[idx]++;
                total_++;
            }
        }
        fprintf(stderr, "[histogram] onAnalyze: built histogram with %ld total rows\n", total_);
    }

    void onEstimate(lyrore::EstimateContext& ctx) override {
        fprintf(stderr, "[histogram] onEstimate called for table: %s\n", ctx.table_name().c_str());
        
        if (total_ == 0) {
            fprintf(stderr, "[histogram] onEstimate: no histogram data\n");
            return;
        }
        if (threshold_ <= 0) {
            fprintf(stderr, "[histogram] onEstimate: threshold=0, skipping\n");
            return;
        }
        if (ctx.table_name() != "products") {
            fprintf(stderr, "[histogram] onEstimate: not products table, skipping\n");
            return;
        }

        int64_t count = 0;
        int maxBucket = std::min((int)(threshold_ / 1200.0), NUM_BUCKETS - 1);
        for (int i = 0; i <= maxBucket; i++) {
            count += buckets_[i];
        }
        fprintf(stderr, "[histogram] onEstimate: setting cardinality to %ld\n", count);
        ctx.set_cardinality(std::max(count, (int64_t)1));
    }
};

LYRORE_REGISTER_PLUGIN(HistogramDebugPlugin);
