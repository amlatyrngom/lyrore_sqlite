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
** 
** Provides safe manipulation of SQLite AST nodes with automatic
** memory management and builder pattern for creating expressions.
*/
class LyExpr : public std::enable_shared_from_this<LyExpr> {
public:
    // Node type
    LyOp op = LyOp::UNKNOWN;
    
    // Value for literals
    LyValue value;
    
    // Token string (for function names, etc.)
    std::string token;
    
    // Column information
    std::string table_name;
    std::string column_name;
    int column_index = -1;
    int table_cursor = -1;
    
    // Tree structure
    LyExprPtr left;
    LyExprPtr right;
    std::vector<LyExprPtr> args;  // Function arguments
    
    // CRITICAL: Original Expr* for column binding preservation
    // Only valid for COLUMN nodes that came from from_sqlite()
    Expr* sqlite_expr = nullptr;
    
    // ===== CONVERSION =====
    
    /**
     * Convert SQLite Expr* to LyExpr.
     * Deep copies the tree structure, preserving sqlite_expr for columns.
     */
    static LyExprPtr from_sqlite(Expr* pExpr);
    
    /**
     * Create a new SQLite Expr* from this LyExpr.
     * Caller is responsible for memory management.
     */
    Expr* to_sqlite_new(Parse* pParse) const;
    
    /**
     * In-place substitute this LyExpr into an existing Expr* target.
     * This is the safe way to replace expressions in the AST.
     */
    void to_sqlite(Parse* pParse, Expr* pTarget) const;
    
    // ===== BUILDERS =====
    
    // Arithmetic
    static LyExprPtr add(LyExprPtr l, LyExprPtr r);
    static LyExprPtr subtract(LyExprPtr l, LyExprPtr r);
    static LyExprPtr multiply(LyExprPtr l, LyExprPtr r);
    static LyExprPtr divide(LyExprPtr l, LyExprPtr r);
    static LyExprPtr mod(LyExprPtr l, LyExprPtr r);
    
    // Comparison
    static LyExprPtr equals(LyExprPtr l, LyExprPtr r);
    static LyExprPtr not_equals(LyExprPtr l, LyExprPtr r);
    static LyExprPtr less_than(LyExprPtr l, LyExprPtr r);
    static LyExprPtr less_equal(LyExprPtr l, LyExprPtr r);
    static LyExprPtr greater_than(LyExprPtr l, LyExprPtr r);
    static LyExprPtr greater_equal(LyExprPtr l, LyExprPtr r);
    
    // Logical
    static LyExprPtr logical_and(LyExprPtr l, LyExprPtr r);
    static LyExprPtr logical_or(LyExprPtr l, LyExprPtr r);
    static LyExprPtr logical_not(LyExprPtr expr);
    
    // Literals
    static LyExprPtr integer(int64_t val);
    static LyExprPtr floating(double val);
    static LyExprPtr string(const std::string& val);
    static LyExprPtr null();
    
    // Column - copies binding info from source
    static LyExprPtr column(const LyExpr& source);
    
    // Function call
    static LyExprPtr function(const std::string& name, std::vector<LyExprPtr> args);
    
    // ===== QUERIES =====
    
    bool is_function(const std::string& name = "") const;
    bool is_column() const;
    bool is_literal() const;
    bool is_comparison() const;
    bool is_arithmetic() const;
    bool is_logical() const;
    
    // Type checks for literals
    bool is_integer() const;
    bool is_float() const;
    bool is_string() const;
    bool is_null() const;
    
    size_t nargs() const;
    LyExprPtr arg(size_t i) const;
    
    // Get integer value (std::nullopt if not integer)
    std::optional<int64_t> as_int() const;
    
    // Get float value (std::nullopt if not float)
    std::optional<double> as_float() const;
    
    // Get string value (std::nullopt if not string)
    std::optional<std::string> as_string() const;
    
    // ===== TREE OPERATIONS =====
    
    LyExprPtr clone() const;
    
    // Debug string representation
    std::string to_string() const;

private:
    static LyExprPtr binary_op(LyOp op, LyExprPtr l, LyExprPtr r);
    static LyExprPtr unary_op(LyOp op, LyExprPtr expr);
};


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
    
    LyExprPtr where() const {
        return select_ && select_->pWhere ? LyExpr::from_sqlite(select_->pWhere) : nullptr;
    }
    
    Expr* where_raw() { return select_ ? select_->pWhere : nullptr; }
    Select* select_raw() { return select_; }  // Direct access to Select*
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
 * Uses void* for internal SQLite types since plugins don't have full headers
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
    
    // Raw access (advanced use) - returns void* since types not available
    void* loop_raw() { return loop_; }
    void* builder_raw() { return builder_; }

    // Select* access for cross-hook state (implemented in cpp_context.cpp)
    Select* select_raw();
    
    // WHERE clause access for predicate pattern matching
    // Get number of WHERE clause terms
    int num_where_terms() const;
    
    // Get a WHERE term as LyExpr by index
    LyExprPtr get_where_term(int index) const;
    
    // Get all WHERE terms as LyExpr vector
    std::vector<LyExprPtr> get_where_terms() const;

    // Raw Expr* access (for Pattern API which needs raw pointers)
    Expr* get_where_term_raw(int index) const;
    std::vector<Expr*> get_where_terms_raw() const;

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
 * Uses void* for Vdbe since it's internal
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
    // These allow storing match results in one hook and retrieving in another
    // The state is keyed by Select* pointer which is stable across hooks
    
    // Store match result for retrieval in later hooks
    void set_template_match(Select* sel, int template_id, 
                           const std::map<std::string, LyValue>& params,
                           const std::map<std::string, LyExprPtr>& expr_params = {});
    
    // Retrieve template ID from earlier hook (nullopt if not found)
    std::optional<int> get_template_id(Select* sel) const;
    
    // Retrieve parameters from earlier hook (nullptr if not found)
    const std::map<std::string, LyValue>* get_template_params(Select* sel) const;
    
    // Retrieve expression parameters from earlier hook (nullptr if not found)
    const std::map<std::string, LyExprPtr>* get_template_expr_params(Select* sel) const;
    
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
** Use this at the end of your plugin file to export the required symbols
*/
#define LYRORE_REGISTER_PLUGIN(PluginClass) \
    extern "C" { \
        lyrore::Plugin* lyrore_create_plugin() { return new PluginClass(); } \
        void lyrore_destroy_plugin(lyrore::Plugin* p) { delete p; } \
    }


/*
** C++ SDK entry points - implemented in cpp_context.cpp
** These are exported for the C glue layer to call via dlsym
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
