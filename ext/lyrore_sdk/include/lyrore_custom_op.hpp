/*
** Lyrore Custom Operators SDK
** 
** Enables plugins to replace query subtrees with custom computation.
** Supports SCALAR (single-value), ITERATOR (multi-row), and CURSOR output modes.
** 
** Design: Uses SQLite's function and virtual table infrastructure for interoperability.
*/
#ifndef LYRORE_CUSTOM_OP_HPP
#define LYRORE_CUSTOM_OP_HPP

#include "lyrore_plugin.hpp"
#include "lyrore_pattern.hpp"
#include <memory>
#include <functional>
#include <vector>
#include <string>
#include <map>

// Forward declarations
struct sqlite3;
struct sqlite3_stmt;

namespace lyrore {

/**
 * MatchParams - Wrapper for pattern-captured parameters
 * Provides type-safe access to parameters extracted during pattern matching.
 */
class MatchParams {
public:
    MatchParams() = default;

    template<typename T>
    T get(const char* name) const {
        auto it = params_.find(name);
        if (it == params_.end()) {
            return T{};
        }
        if constexpr (std::is_same_v<T, int64_t>) {
            return std::get<int64_t>(it->second);
        } else if constexpr (std::is_same_v<T, int>) {
            return static_cast<int>(std::get<int64_t>(it->second));
        } else if constexpr (std::is_same_v<T, double>) {
            return std::get<double>(it->second);
        } else if constexpr (std::is_same_v<T, std::string>) {
            return std::get<std::string>(it->second);
        }
        return T{};
    }

    const std::map<std::string, LyValue>& all() const { return params_; }

    void set(const std::string& name, const LyValue& val) { 
        params_[name] = val; 
    }

    size_t size() const { return params_.size(); }

private:
    std::map<std::string, LyValue> params_;
};


/**
 * TableCursor - Read-only cursor for iterating table rows
 * Uses prepared statements internally for safety and stability.
 */
class TableCursor {
public:
    TableCursor();
    TableCursor(sqlite3* db, const char* table_name);
    ~TableCursor();

    // Move semantics only
    TableCursor(TableCursor&& other) noexcept;
    TableCursor& operator=(TableCursor&& other) noexcept;
    TableCursor(const TableCursor&) = delete;
    TableCursor& operator=(const TableCursor&) = delete;

    bool valid() const;        // Is cursor in valid state?
    bool eof() const;          // At end of table?
    bool next();               // Advance to next row (must call first to get first row)
    void reset();              // Reset to beginning

    LyValue get_column(int col);
    std::vector<LyValue> get_row();
    int64_t get_rowid();
    int num_columns() const;

private:
    sqlite3* db_ = nullptr;
    sqlite3_stmt* stmt_ = nullptr;
    bool at_end_ = false;
    int num_cols_ = 0;
};


/**
 * OutputMode - How the custom operator produces results
 */
enum class OutputMode {
    SCALAR,     // Single value result (e.g., SUM)
    ITERATOR,   // Multi-row result (e.g., GROUP BY)
    CURSOR      // Raw cursor access (advanced)
};


/**
 * CustomOperator - Base class for custom operators
 * 
 * Plugin authors subclass this and implement the appropriate methods
 * based on their chosen output mode.
 */
class CustomOperator {
public:
    virtual ~CustomOperator() = default;

    // Required: specify output mode
    virtual OutputMode output_mode() const = 0;

    // Optional: called before compute_scalar() or iterator iteration
    virtual void execute() {}

    // SCALAR mode: return single value
    virtual LyValue compute_scalar() { return LyValue{}; }

    // ITERATOR mode: produce rows one at a time
    virtual bool iterator_next() { return false; }
    virtual std::vector<LyValue> iterator_get_row() { return {}; }
    virtual void iterator_reset() {}

    // CURSOR mode (advanced): return raw btree cursor handle
    virtual void* get_cursor_handle() { return nullptr; }

public:
    // SDK-internal setters for invoking the operator
    void _set_db(sqlite3* db) { db_ = db; }
    void _set_params(const MatchParams* p) { params_ = p; }

protected:
    // SDK provides these - call from your implementation
    TableCursor open_table(const char* table_name);
    std::unique_ptr<ResultSet> query(const char* sql);

    // Set by SDK during invocation
    sqlite3* db_ = nullptr;
    const MatchParams* params_ = nullptr;
};


// Forward declaration of internal registry
class CustomOpRegistry;

/**
 * Registration API - call from plugin onInit()
 * 
 * Example:
 *   register_custom_op<MyFastSumOp>(db, 
 *       "SELECT SUM(qty) FROM orders WHERE category_id = ?",
 *       {"sum"});
 */
template<typename OpClass>
void register_custom_op(sqlite3* db, 
                        const char* pattern_sql,
                        const std::vector<std::string>& output_columns);


// ============================================================
// Internal Implementation Details
// ============================================================

/**
 * CustomOpEntry - Internal storage for registered operators
 */
struct CustomOpEntry {
    std::shared_ptr<pattern::Pattern> pattern;
    std::function<std::unique_ptr<CustomOperator>()> factory;
    std::vector<std::string> output_columns;
    std::string vtab_name;    // Generated unique name
    std::string func_name;    // Generated unique name  
    OutputMode mode;
    sqlite3* db;              // Connection this was registered on
    void* pVtabTable = nullptr;  // Cached Table* for vtab (set during registration)
};


/**
 * CustomOpRegistry - Manages registered custom operators
 * Per-connection registry for thread safety.
 * 
 * Uses unique_ptr for entries to ensure stable pointers for sqlite3_create_module_v2.
 */
class CustomOpRegistry {
public:
    // Get or create registry for a db connection
    static CustomOpRegistry& for_db(sqlite3* db);

    // Register a new operator - takes ownership of the entry
    // Returns raw pointer to the stored entry (stable, won't move)
    CustomOpEntry* register_op(std::unique_ptr<CustomOpEntry> entry);

    // Get all registered operators (const access)
    const std::vector<std::unique_ptr<CustomOpEntry>>& get_entries() const { return entries_; }

    // Find matching operator for a Select*
    const CustomOpEntry* find_match(Select* pSelect) const;

    // Generate unique IDs
    int next_id() { return next_id_++; }

    // Cleanup for connection
    static void cleanup(sqlite3* db);

private:
    CustomOpRegistry() = default;
    std::vector<std::unique_ptr<CustomOpEntry>> entries_;
    int next_id_ = 0;
};


// Internal functions used by wrappers
// Changed to take raw pointer since entry is now heap-allocated with stable address
void register_scalar_wrapper(sqlite3* db, CustomOpEntry* entry);
void register_vtab_wrapper(sqlite3* db, CustomOpEntry* entry);
int rewrite_custom_ops(PreOptContext& ctx);

// Convert between LyValue and sqlite3_value/result
LyValue sqlite3_value_to_lyvalue(void* value);
void set_sqlite3_result(void* ctx, const LyValue& val);


// Template implementation
template<typename OpClass>
void register_custom_op(sqlite3* db, 
                        const char* pattern_sql,
                        const std::vector<std::string>& output_columns) {
    // Create entry as unique_ptr for heap allocation
    auto entry = std::make_unique<CustomOpEntry>();
    entry->db = db;
    entry->pattern = pattern::Pattern::from_query(db, pattern_sql);
    entry->factory = []() { return std::make_unique<OpClass>(); };
    entry->output_columns = output_columns;

    // Create temporary instance to check output mode
    auto temp = std::make_unique<OpClass>();
    entry->mode = temp->output_mode();

    auto& registry = CustomOpRegistry::for_db(db);
    int id = registry.next_id();

    if (entry->mode == OutputMode::SCALAR) {
        entry->func_name = "_lyrore_fn_" + std::to_string(id);
    } else {
        entry->vtab_name = "_lyrore_vtab_" + std::to_string(id);
    }

    // Register in registry FIRST - this gives us a stable pointer
    CustomOpEntry* stable_ptr = registry.register_op(std::move(entry));

    // Now register wrappers with the stable pointer
    if (stable_ptr->mode == OutputMode::SCALAR) {
        register_scalar_wrapper(db, stable_ptr);
    } else {
        register_vtab_wrapper(db, stable_ptr);
    }
}


} // namespace lyrore

#endif /* LYRORE_CUSTOM_OP_HPP */
