/*
** Histogram Plugin
**
** Builds a histogram during ANALYZE for better cardinality estimation.
** Targets the products table with (price * qty) expression.
**
** The plugin:
** 1. During ANALYZE: builds a histogram of price*qty values
** 2. During onEstimate: matches predicates of the form (price * qty) < constant
**    - Verifies column names are exactly "price" and "qty"
**    - Extracts the threshold constant from the WHERE clause
**    - Returns histogram-based cardinality estimate
**
** Expected: Q-error improvement from ~14x to <2x
*/

#include "lyrore_plugin.hpp"
#include <vector>
#include <algorithm>
#include <cstdlib>

class HistogramPlugin : public lyrore::Plugin {
private:
    static constexpr int NUM_BUCKETS = 100;
    static constexpr double BUCKET_SIZE = 1200.0;
    std::vector<int64_t> buckets_;
    int64_t total_ = 0;

public:
    std::string name() const override { return "histogram"; }

    void onInit(sqlite3* /*db*/) override {
        buckets_.resize(NUM_BUCKETS, 0);
    }

    void onAnalyze(lyrore::AnalyzeContext& ctx) override {
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

    void onEstimate(lyrore::EstimateContext& ctx) override {
        if (total_ == 0) return;

        // Only apply to products table
        if (ctx.table_name() != "products") return;

        // Get WHERE clause terms and look for matching pattern
        auto terms = ctx.get_where_terms();
        
        for (const auto& term : terms) {
            double threshold = 0;
            if (matchesPriceQtyPattern(term, threshold)) {
                // Compute cardinality using histogram
                int64_t count = countBelowThreshold(threshold);
                ctx.set_cardinality(std::max(count, (int64_t)1));
                return;
            }
        }
    }

private:
    // Check if expression matches: (price * qty) < constant
    // Returns true if matched and sets threshold to the constant value
    bool matchesPriceQtyPattern(const lyrore::LyExprPtr& expr, double& threshold) const {
        if (!expr) return false;
        
        // Check for LT comparison: (expr) < (constant)
        if (expr->op != lyrore::LyOp::LT) return false;
        
        auto left = expr->left;
        auto right = expr->right;
        
        if (!left || !right) return false;
        
        // Check if right side is a constant (integer or float)
        if (right->op == lyrore::LyOp::INTEGER) {
            auto val = right->as_int();
            if (!val) return false;
            threshold = static_cast<double>(*val);
        } else if (right->op == lyrore::LyOp::FLOAT) {
            auto val = right->as_float();
            if (!val) return false;
            threshold = *val;
        } else {
            return false;
        }
        
        // Check if left side is (price * qty)
        return isMultiplyOfPriceQty(left);
    }
    
    // Check if expression is (price * qty) in either order
    bool isMultiplyOfPriceQty(const lyrore::LyExprPtr& expr) const {
        if (!expr || expr->op != lyrore::LyOp::MUL) return false;
        
        auto left = expr->left;
        auto right = expr->right;
        
        if (!left || !right) return false;
        
        // Both sides must be columns
        if (!left->is_column() || !right->is_column()) return false;
        
        std::string col1 = left->column_name;
        std::string col2 = right->column_name;
        
        // Check for price * qty or qty * price
        return (col1 == "price" && col2 == "qty") ||
               (col1 == "qty" && col2 == "price");
    }
    
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
