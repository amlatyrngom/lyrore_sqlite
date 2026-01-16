/*
** Lyrore C++ Plugin SDK
** 
** This header provides the C++ interface for writing Lyrore plugins.
** Plugins use this SDK to hook into query processing.
** 
** Note: For AST manipulation, use the raw SQLite C API (sqliteInt.h).
** The Pattern API (lyrore_pattern.hpp) is provided for query template matching.
*/
#ifndef LYRORE_PLUGIN_HPP
#define LYRORE_PLUGIN_HPP

#include <string>
#include <vector>
#include <memory>
#include <variant>
#include <optional>
#include <cstdint>
#include <functional>
#include <map>
#include <unordered_map>

// Include SQLite internals with C linkage
extern "C" {
#include "sqliteInt.h"
}

// Forward declaration at global scope for C ABI compatibility
struct LyroreCppContext;

namespace lyrore {

namespace pattern {
    class MatchResult;
}

/*
** LyValue - Variant type for literal values
** Used for storing extracted parameter values from pattern matching.
*/
using LyValue = std::variant<
    std::monostate,       // NULL
    int64_t,              // Integer
    double,               // Float  
    std::string,          // String
    std::vector<uint8_t>  // Blob
>;


/*
** Context Classes - Provide safe access to SQLite internals during hooks
*/

/**
 * PreOptContext - Context for pre-optimization hooks
 */
class PreOptContext {
public:
    PreOptContext(sqlite3* db, Parse* parse, Select* select)
        : db_(db), parse_(parse), select_(select), modified_(false) {}

    // Get raw WHERE clause Expr* for direct manipulation
    Expr* where_raw() { return select_ ? select_->pWhere : nullptr; }

    // Get raw Select* for pattern matching and direct manipulation
    Select* select_raw() { return select_; }

    Parse* parse() { return parse_; }
    sqlite3* db() { return db_; }

    void set_modified() { modified_ = true; }
    bool is_modified() const { return modified_; }

private:
    sqlite3* db_;
    Parse* parse_;
    Select* select_;
    bool modified_;
};


/**
 * EstimateContext - Context for estimate hooks
 */
class EstimateContext {
public:
    EstimateContext(sqlite3* db, void* builder, void* loop)
        : db_(db), builder_(builder), loop_(loop), skipped_(false) {}

    // Get table name for this loop
    std::string table_name() const;

    // Get current cardinality estimate
    int64_t cardinality() const;

    // Set new cardinality estimate
    void set_cardinality(int64_t rows);

    // Check if this is a full table scan
    bool is_full_scan() const;

    // Check if this loop uses an index
    bool is_index_scan() const;

    // Skip this loop (don't adjust)
    void skip() { skipped_ = true; }
    bool is_skipped() const { return skipped_; }

    sqlite3* db() { return db_; }

    // Raw access for advanced use
    void* loop_raw() { return loop_; }
    void* builder_raw() { return builder_; }

    // Select* access for cross-hook state and pattern matching
    Select* select_raw();

    // WHERE clause access - returns raw Expr* for pattern matching
    int num_where_terms() const;
    Expr* get_where_term(int index) const;
    std::vector<Expr*> get_where_terms() const;

private:
    sqlite3* db_;
    void* builder_;
    void* loop_;
    bool skipped_;
};


/**
 * ResultSet - Simple query result iterator for AnalyzeContext
 */
class ResultSet {
public:
    explicit ResultSet(sqlite3_stmt* stmt) : stmt_(stmt) {}
    ~ResultSet() { if (stmt_) sqlite3_finalize(stmt_); }

    // Non-copyable
    ResultSet(const ResultSet&) = delete;
    ResultSet& operator=(const ResultSet&) = delete;

    // Movable
    ResultSet(ResultSet&& other) noexcept : stmt_(other.stmt_) { other.stmt_ = nullptr; }
    ResultSet& operator=(ResultSet&& other) noexcept {
        if (stmt_) sqlite3_finalize(stmt_);
        stmt_ = other.stmt_;
        other.stmt_ = nullptr;
        return *this;
    }

    bool next() { return sqlite3_step(stmt_) == SQLITE_ROW; }

    int64_t get_int(int col) { return sqlite3_column_int64(stmt_, col); }
    double get_double(int col) { return sqlite3_column_double(stmt_, col); }
    std::string get_text(int col) {
        const char* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt_, col));
        return text ? text : "";
    }
    bool is_null(int col) { return sqlite3_column_type(stmt_, col) == SQLITE_NULL; }

private:
    sqlite3_stmt* stmt_;
};


/**
 * AnalyzeContext - Context for ANALYZE hooks
 */
class AnalyzeContext {
public:
    explicit AnalyzeContext(sqlite3* db) : db_(db) {}

    sqlite3* db() { return db_; }

    // Execute a query and get results
    std::unique_ptr<ResultSet> query(const std::string& sql) {
        sqlite3_stmt* stmt = nullptr;
        int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
        if (rc != SQLITE_OK) return nullptr;
        return std::make_unique<ResultSet>(stmt);
    }

    // Execute a statement (no results)
    bool exec(const std::string& sql) {
        return sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK;
    }

private:
    sqlite3* db_;
};


/**
 * PostQueryContext - Context for post-query hooks
 */
class PostQueryContext {
public:
    PostQueryContext(sqlite3* db, void* vdbe)
        : db_(db), vdbe_(vdbe) {}

    sqlite3* db() { return db_; }

    // Get execution time (requires SQLITE_ENABLE_STMT_SCANSTATUS)
    int64_t exec_time_us() const;

    // Get number of VM steps
    int64_t vm_steps() const;

    // Raw access
    void* vdbe_raw() { return vdbe_; }

private:
    sqlite3* db_;
    void* vdbe_;
};


/*
** Plugin - Base class for Lyrore plugins
*/
class Plugin {
public:
    virtual ~Plugin() = default;

    // Plugin identity
    virtual std::string name() const = 0;

    // Lifecycle hooks
    virtual void onInit(sqlite3* db) { (void)db; }
    virtual void onShutdown() {}

    // Processing hooks - override what you need
    virtual void onPreOpt(PreOptContext& ctx) { (void)ctx; }
    virtual void onEstimate(EstimateContext& ctx) { (void)ctx; }
    virtual void onPostQuery(PostQueryContext& ctx) { (void)ctx; }
    virtual void onAnalyze(AnalyzeContext& ctx) { (void)ctx; }

    // Cross-hook template state methods
    // Store match result for retrieval in later hooks
    void set_template_match(Select* sel, int template_id, 
                           const std::map<std::string, LyValue>& params);

    // Retrieve template ID from earlier hook (nullopt if not found)
    std::optional<int> get_template_id(Select* sel) const;

    // Retrieve parameters from earlier hook (nullptr if not found)
    const std::map<std::string, LyValue>* get_template_params(Select* sel) const;

    // Clear match for a Select* (optional cleanup)
    void clear_template_match(Select* sel);

protected:
    // Context pointer set by plugin manager
    friend struct ::LyroreCppContext;
    ::LyroreCppContext* context_ = nullptr;
};


} // namespace lyrore


/*
** Plugin registration macro
*/
#define LYRORE_REGISTER_PLUGIN(PluginClass) \
    extern "C" { \
        lyrore::Plugin* lyrore_create_plugin() { return new PluginClass(); } \
        void lyrore_destroy_plugin(lyrore::Plugin* p) { delete p; } \
    }


/*
** C++ SDK entry points - implemented in cpp_context.cpp
*/
extern "C" {
    struct LyroreCppContext;
    LyroreCppContext* lyrore_cpp_create(sqlite3* db);
    void lyrore_cpp_destroy(LyroreCppContext* ctx);
    int lyrore_cpp_load_plugin(LyroreCppContext* ctx, const char* path);
    int lyrore_cpp_invoke_preopt(LyroreCppContext* ctx, void* pParse, void* pSelect);
    void lyrore_cpp_invoke_estimate(LyroreCppContext* ctx, void* pBuilder, void* pLoop);
    void lyrore_cpp_invoke_analyze(LyroreCppContext* ctx, int iDb);
    void lyrore_cpp_invoke_postquery(LyroreCppContext* ctx, void* pVdbe);
}


#endif /* LYRORE_PLUGIN_HPP */
