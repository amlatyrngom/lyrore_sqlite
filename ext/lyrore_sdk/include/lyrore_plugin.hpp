/*
** Lyrore C++ Plugin SDK
** 
** This header provides the C++ interface for writing Lyrore plugins.
** Plugins use this SDK to manipulate SQLite AST and hook into query processing.
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

// Forward declarations
class LyExpr;
class LyroreMain;

namespace pattern {
    class MatchResult;
}
using LyExprPtr = std::shared_ptr<LyExpr>;

/*
** LyOp - Expression operation types
** Maps directly to SQLite TK_* tokens
*/
enum class LyOp : int {
    // Comparisons
    EQ = 54,          // TK_EQ (=)
    NE = 53,          // TK_NE (!=, <>)
    LT = 57,          // TK_LT (<)
    LE = 56,          // TK_LE (<=)
    GT = 55,          // TK_GT (>)
    GE = 58,          // TK_GE (>=)

    // Arithmetic
    ADD = 107,        // TK_PLUS (+)
    SUB = 108,        // TK_MINUS (-)
    MUL = 109,        // TK_STAR (*)
    DIV = 110,        // TK_SLASH (/)
    MOD = 111,        // TK_REM (%)

    // Logical
    AND = 44,         // TK_AND
    OR = 43,          // TK_OR
    NOT = 19,         // TK_NOT

    // Bitwise
    BITAND = 103,     // TK_BITAND (&)
    BITOR = 104,      // TK_BITOR (|)
    BITNOT = 115,     // TK_BITNOT (~)
    LSHIFT = 105,     // TK_LSHIFT (<<)
    RSHIFT = 106,     // TK_RSHIFT (>>)

    // Other operators
    CONCAT = 112,     // TK_CONCAT (||)
    ISNULL = 73,      // TK_ISNULL
    NOTNULL = 74,     // TK_NOTNULL
    IS = 45,          // TK_IS
    ISNOT = 119,      // IS NOT
    BETWEEN = 48,     // TK_BETWEEN
    IN = 49,          // TK_IN
    LIKE = 46,        // TK_LIKE
    GLOB = 47,        // TK_GLOB

    // Leaf node types
    COLUMN = 168,     // TK_COLUMN
    INTEGER = 156,    // TK_INTEGER
    FLOAT = 154,      // TK_FLOAT
    STRING = 118,     // TK_STRING
    BLOB = 125,       // TK_BLOB
    NULL_VAL = 122,   // TK_NULL
    VARIABLE = 157,   // TK_VARIABLE (?)

    // Function types
    FUNCTION = 172,   // TK_FUNCTION
    AGG_FUNCTION = 169, // TK_AGG_FUNCTION

    // Special
    REGISTER = 173,   // TK_REGISTER
    VECTOR = 174,     // TK_VECTOR
    SELECT = 175,     // Subquery

    UNKNOWN = 0
};

/*
** LyValue - Variant type for literal values
*/
using LyValue = std::variant<
    std::monostate,       // NULL
    int64_t,              // Integer
    double,               // Float  
    std::string,          // String
    std::vector<uint8_t>  // Blob
>;

/*
** LyExpr - C++ wrapper for SQLite expression trees
*/
class LyExpr : public std::enable_shared_from_this<LyExpr> {
public:
    LyOp op = LyOp::UNKNOWN;
    LyValue value;
    std::string token;
    std::string table_name;
    std::string column_name;
    int column_index = -1;
    int table_cursor = -1;
    LyExprPtr left;
    LyExprPtr right;
    std::vector<LyExprPtr> args;
    Expr* sqlite_expr = nullptr;

    static LyExprPtr from_sqlite(Expr* pExpr);
    Expr* to_sqlite_new(Parse* pParse) const;
    void to_sqlite(Parse* pParse, Expr* pTarget) const;

    // Builders
    static LyExprPtr add(LyExprPtr l, LyExprPtr r);
    static LyExprPtr subtract(LyExprPtr l, LyExprPtr r);
    static LyExprPtr multiply(LyExprPtr l, LyExprPtr r);
    static LyExprPtr divide(LyExprPtr l, LyExprPtr r);
    static LyExprPtr mod(LyExprPtr l, LyExprPtr r);
    static LyExprPtr equals(LyExprPtr l, LyExprPtr r);
    static LyExprPtr not_equals(LyExprPtr l, LyExprPtr r);
    static LyExprPtr less_than(LyExprPtr l, LyExprPtr r);
    static LyExprPtr less_equal(LyExprPtr l, LyExprPtr r);
    static LyExprPtr greater_than(LyExprPtr l, LyExprPtr r);
    static LyExprPtr greater_equal(LyExprPtr l, LyExprPtr r);
    static LyExprPtr logical_and(LyExprPtr l, LyExprPtr r);
    static LyExprPtr logical_or(LyExprPtr l, LyExprPtr r);
    static LyExprPtr logical_not(LyExprPtr expr);
    static LyExprPtr integer(int64_t val);
    static LyExprPtr floating(double val);
    static LyExprPtr string(const std::string& val);
    static LyExprPtr null();
    static LyExprPtr column(const LyExpr& source);
    static LyExprPtr function(const std::string& name, std::vector<LyExprPtr> args);

    // Queries
    bool is_function(const std::string& name = "") const;
    bool is_column() const;
    bool is_literal() const;
    bool is_comparison() const;
    bool is_arithmetic() const;
    bool is_logical() const;
    bool is_integer() const;
    bool is_float() const;
    bool is_string() const;
    bool is_null() const;
    size_t nargs() const;
    LyExprPtr arg(size_t i) const;
    std::optional<int64_t> as_int() const;
    std::optional<double> as_float() const;
    std::optional<std::string> as_string() const;
    LyExprPtr clone() const;
    std::string to_string() const;

private:
    static LyExprPtr binary_op(LyOp op, LyExprPtr l, LyExprPtr r);
    static LyExprPtr unary_op(LyOp op, LyExprPtr expr);
};


/*
** Statistics Structures (NEW in Step 5)
*/

// Statement-level statistics (always available)
struct StmtStats {
    int64_t fullscan_steps = 0;    // SQLITE_STMTSTATUS_FULLSCAN_STEP
    int64_t sorts = 0;             // SQLITE_STMTSTATUS_SORT
    int64_t autoindex_inserts = 0; // SQLITE_STMTSTATUS_AUTOINDEX
    int64_t vm_steps = 0;          // SQLITE_STMTSTATUS_VM_STEP
    int64_t runs = 0;              // SQLITE_STMTSTATUS_RUN
    int64_t filter_hits = 0;       // SQLITE_STMTSTATUS_FILTER_HIT
    int64_t filter_misses = 0;     // SQLITE_STMTSTATUS_FILTER_MISS
    int64_t memory_used = 0;       // SQLITE_STMTSTATUS_MEMUSED
    int64_t reprepares = 0;        // SQLITE_STMTSTATUS_REPREPARE
};

// Per-scan statistics (requires SQLITE_ENABLE_STMT_SCANSTATUS)
struct ScanStats {
    int scan_id = -1;              // SELECTID - matches EQP column 1
    int parent_id = -1;            // PARENTID - matches EQP column 2
    std::string name;              // Table or index name
    std::string explain;           // Full EXPLAIN QUERY PLAN description
    int64_t loops = 0;             // NLOOP - times loop executed
    int64_t visits = 0;            // NVISIT - rows examined
    double estimate = 0.0;         // EST - planner estimate
    int64_t cycles = -1;           // NCYCLE - CPU cycles (-1 if unavailable)
};


/*
** Context Classes - Provide safe access to SQLite internals during hooks
*/

/**
 * PreParseContext - Context for pre-parse SQL transformation
 * 
 * Provides access to the SQL string before parsing, allowing custom dialect
 * transformations. The plugin is fully responsible for any parsing/matching.
 */
class PreParseContext {
public:
    PreParseContext(sqlite3* db, const std::string& sql)
        : db_(db), sql_(sql), modified_(false) {}

    // Get current SQL string
    std::string sql() const { return sql_; }

    // Replace SQL string (e.g., for dialect transformation)
    void set_sql(const std::string& new_sql) {
        sql_ = new_sql;
        modified_ = true;
    }

    bool modified() const { return modified_; }
    sqlite3* db() const { return db_; }

private:
    sqlite3* db_;
    std::string sql_;
    bool modified_;
};

/**
 * PreOptContext - Context for pre-optimization hooks
 */
class PreOptContext {
public:
    PreOptContext(sqlite3* db, Parse* parse, Select* select)
        : db_(db), parse_(parse), select_(select), modified_(false) {}

    LyExprPtr where() const {
        return select_ && select_->pWhere ? LyExpr::from_sqlite(select_->pWhere) : nullptr;
    }

    Expr* where_raw() { return select_ ? select_->pWhere : nullptr; }
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
 * EstimateContext - Context for cardinality estimate hooks
 */
class EstimateContext {
public:
    EstimateContext(sqlite3* db, void* builder, void* loop)
        : db_(db), builder_(builder), loop_(loop), skipped_(false) {}

    std::string table_name() const;
    int64_t cardinality() const;
    void set_cardinality(int64_t rows);
    bool is_full_scan() const;
    bool is_index_scan() const;
    int num_where_terms() const;
    LyExprPtr get_where_term(int index) const;
    std::vector<LyExprPtr> get_where_terms() const;
    Expr* get_where_term_raw(int index) const;
    std::vector<Expr*> get_where_terms_raw() const;
    Select* select_raw();

    void skip_default() { skipped_ = true; }
    bool is_skipped() const { return skipped_; }
    sqlite3* db() { return db_; }

private:
    sqlite3* db_;
    void* builder_;
    void* loop_;
    bool skipped_;
};


/**
 * PostQueryContext - Context for post-query statistics hooks (Enhanced)
 */
class PostQueryContext {
public:
    PostQueryContext(sqlite3* db, void* pVdbe)
        : db_(db), pVdbe_(pVdbe) {}

    sqlite3* db() const { return db_; }

    // Enhanced statistics (NEW in Step 5)
    StmtStats stmt_stats() const;
    std::vector<ScanStats> scan_stats() const;
    int scan_count() const;
    int64_t total_cycles() const;  // -1 if unavailable
    bool scanstatus_enabled() const;

    // Legacy methods (kept for compatibility but deprecated)
    int64_t exec_time_us() const;
    int64_t vm_steps() const;

private:
    sqlite3* db_;
    void* pVdbe_;
};



/**
 * ResultSet - Simple wrapper for SQL query results
 */
class ResultSet {
public:
    explicit ResultSet(sqlite3_stmt* stmt) : stmt_(stmt), has_row_(false) {}
    ~ResultSet() { if (stmt_) sqlite3_finalize(stmt_); }
    
    ResultSet(const ResultSet&) = delete;
    ResultSet& operator=(const ResultSet&) = delete;
    ResultSet(ResultSet&& o) noexcept : stmt_(o.stmt_), has_row_(o.has_row_) { o.stmt_ = nullptr; }
    
    bool next() {
        if (!stmt_) return false;
        has_row_ = (sqlite3_step(stmt_) == SQLITE_ROW);
        return has_row_;
    }
    bool has_row() const { return has_row_; }
    int column_count() const { return stmt_ ? sqlite3_column_count(stmt_) : 0; }
    int64_t get_int64(int col) const { return stmt_ ? sqlite3_column_int64(stmt_, col) : 0; }
    double get_double(int col) const { return stmt_ ? sqlite3_column_double(stmt_, col) : 0.0; }
    const char* get_text(int col) const { return stmt_ ? (const char*)sqlite3_column_text(stmt_, col) : nullptr; }
    
private:
    sqlite3_stmt* stmt_;
    bool has_row_;
};

/**
 * AnalyzeContext - Context for ANALYZE hooks
 */
class AnalyzeContext {
public:
    explicit AnalyzeContext(sqlite3* db) : db_(db) {}
    sqlite3* db() const { return db_; }
    
    // Execute a SQL query and return results
    std::unique_ptr<ResultSet> query(const char* sql) {
        if (!db_ || !sql) return nullptr;
        sqlite3_stmt* stmt = nullptr;
        int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
        if (rc != SQLITE_OK || !stmt) return nullptr;
        return std::make_unique<ResultSet>(stmt);
    }

private:
    sqlite3* db_;
};


/**
 * Plugin - Base class for Lyrore plugins
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
    virtual void onPreParse(PreParseContext& ctx) { (void)ctx; }  // NEW
    virtual void onPreOpt(PreOptContext& ctx) { (void)ctx; }
    virtual void onEstimate(EstimateContext& ctx) { (void)ctx; }
    virtual void onPostQuery(PostQueryContext& ctx) { (void)ctx; }
    virtual void onAnalyze(AnalyzeContext& ctx) { (void)ctx; }

    // LyroreMain access (NEW in Step 5)
    void set_lyrore_main(LyroreMain* main) { lyrore_main_ = main; }
    LyroreMain* lyrore_main() const { return lyrore_main_; }

    // Cross-hook template state methods
    void set_template_match(Select* sel, int template_id, 
                           const std::map<std::string, LyValue>& params,
                           const std::map<std::string, LyExprPtr>& expr_params = {});
    std::optional<int> get_template_id(Select* sel) const;
    const std::map<std::string, LyValue>* get_template_params(Select* sel) const;
    const std::map<std::string, LyExprPtr>* get_template_expr_params(Select* sel) const;
    void clear_template_match(Select* sel);

protected:
    friend struct ::LyroreCppContext;
    ::LyroreCppContext* context_ = nullptr;
    LyroreMain* lyrore_main_ = nullptr;
};


// Plugin registration macro
#define LYRORE_REGISTER_PLUGIN(PluginClass) \
    extern "C" { \
        lyrore::Plugin* lyrore_create_plugin() { return new PluginClass(); } \
        void lyrore_destroy_plugin(lyrore::Plugin* p) { delete p; } \
        void lyrore_plugin_init(lyrore::Plugin* p, sqlite3* db) { p->onInit(db); } \
    }


// Custom operator AST rewrite function (defined in ast_rewrite.cpp)
int rewrite_custom_ops(PreOptContext& ctx);

} // namespace lyrore

// Pattern capture mode (defined in pattern.cpp) - extern "C" at global scope
extern "C" void lyrore_pattern_capture_preopt(void* pSelect);

#endif // LYRORE_PLUGIN_HPP
