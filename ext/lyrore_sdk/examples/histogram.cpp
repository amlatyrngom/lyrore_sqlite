/*
** Histogram Plugin (Pattern API Version)
**
** Builds a histogram during ANALYZE for better cardinality estimation.
** Uses the Pattern API for WHERE clause matching.
**
** LOC Reduction: ~80 LOC manual matching -> ~10 LOC pattern API
*/

#include "lyrore_plugin.hpp"
#include "lyrore_pattern.hpp"
#include <vector>
#include <algorithm>
#include <cstdlib>

using namespace lyrore;
using namespace lyrore::pattern;

class HistogramPlugin : public Plugin {
private:
    static constexpr int NUM_BUCKETS = 100;
    static constexpr double BUCKET_SIZE = 1200.0;
    std::vector<int64_t> buckets_;
    int64_t total_ = 0;
    PatternPtr pattern_;

public:
    std::string name() const override { return "histogram"; }

    void onInit(sqlite3* db) override {
        buckets_.resize(NUM_BUCKETS, 0);

        // Create pattern for (price * qty) < ? using Pattern API
        pattern_ = Pattern::from_where_expr(db, 
            "SELECT 1 FROM products WHERE (price * qty) < ?");
    }

    void onAnalyze(AnalyzeContext& ctx) override {
        // Reset histogram
        std::fill(buckets_.begin(), buckets_.end(), 0);
        total_ = 0;

        // Build histogram from products table
        auto rs = ctx.query("SELECT price * qty FROM products");
        while (rs && rs->next()) {
            double v = rs->get_double(0);
            int idx = std::min((int)(v / BUCKET_SIZE), NUM_BUCKETS - 1);
            if (idx >= 0) {
                buckets_[idx]++;
                total_++;
            }
        }
    }

    void onEstimate(EstimateContext& ctx) override {
        if (total_ == 0) return;
        if (!pattern_ || !pattern_->is_valid()) return;

        // Only apply to products table
        if (ctx.table_name() != "products") return;

        // Get WHERE clause terms and match against pattern
        auto terms = ctx.get_where_terms();
        for (const auto& term : terms) {
            if (auto m = pattern_->match_expr(term)) {
                // Pattern matched! Extract threshold from $1
                double threshold = m.get<double>("$1");

                // Compute cardinality using histogram
                int64_t count = countBelowThreshold(threshold);
                ctx.set_cardinality(std::max(count, (int64_t)1));
                return;
            }
        }
    }

private:
    // Count rows below threshold using histogram
    int64_t countBelowThreshold(double threshold) const {
        int64_t count = 0;
        int maxBucket = std::min((int)(threshold / BUCKET_SIZE), NUM_BUCKETS - 1);
        for (int i = 0; i <= maxBucket && i < NUM_BUCKETS; i++) {
            count += buckets_[i];
        }
        return count;
    }
};

LYRORE_REGISTER_PLUGIN(HistogramPlugin);
