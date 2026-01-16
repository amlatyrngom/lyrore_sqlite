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
// Step 4: Storage Customization Additions
// ============================================================

/**
 * StorageMode - How the storage operator handles data
 */
enum class StorageMode { 
    OWNED,      // Virtual table IS the storage (xUpdate -> on_update)
    REPLICATED  // Mirrors existing table (preupdate_hook -> on_update)
};

/**
 * UpdateOp - Type of data modification
 */
enum class UpdateOp { INSERT, UPDATE, DELETE };


/**
 * VersionManager - Manages version tracking with shadow table persistence
 * 
 * Shadow table: <storage>_lyrore_version
 * Used to detect crash-interrupted transactions on reconnect.
 */
class VersionManager {
public:
    VersionManager(sqlite3* db, const std::string& storage_name);
    ~VersionManager();

    // Create shadow table if not exists
    void init();

    // Get current version from DB shadow table
    int64_t get_db_version();

    // Increment and write version (called during on_sync)
    void increment_version();

    // Called on commit to finalize version
    void commit_version();

    // Called on rollback to restore previous version  
    void rollback_version();

    // Verify consistency - returns false if crash detected
    bool verify_consistency(int64_t plugin_committed_version);

    // Getters
    int64_t get_committed_version() const;
    int64_t get_current_version() const;

private:
    sqlite3* db_;
    std::string table_name_;  // e.g., "orders_col_lyrore_version"
    int64_t current_version_;
    int64_t committed_version_;
};


/**
 * StorageCustomOperator - Base for custom storage backends
 * 
 * OWNED mode: Virtual table IS the storage (xUpdate -> on_update)
 * REPLICATED mode: Mirrors existing table (preupdate_hook -> on_update)
 * 
 * Transaction lifecycle:
 *   on_begin()   - Called when transaction starts
 *   on_update()  - Called for each INSERT/UPDATE/DELETE
 *   on_sync()    - Called before commit (MUST ensure durability)
 *   on_commit()  - Called on successful commit
 *   on_rollback()- Called on rollback
 */
class StorageCustomOperator : public CustomOperator {
public:
    virtual ~StorageCustomOperator() = default;

    // === Required ===
    virtual StorageMode storage_mode() const = 0;
    virtual std::string source_table() const { return ""; }  // For REPLICATED mode

    // === Write Handling ===
    virtual int on_update(
        UpdateOp op,
        int64_t old_rowid, int64_t new_rowid,
        const std::vector<LyValue>& old_vals,
        const std::vector<LyValue>& new_vals
    ) = 0;

    // === Transaction Lifecycle ===
    virtual int on_begin() { return 0; }   // SQLITE_OK
    virtual int on_sync() { return 0; }    // MUST ensure durability before returning
    virtual void on_commit() {}
    virtual void on_rollback() {}

    // === Row ID for vtab operations ===
    // Must return the rowid of the current row during iteration.
    // Called by xRowid after iterator_next() returns true.
    virtual int64_t get_current_rowid() const { return 0; }

    // === ACID Version Tracking ===
    virtual int64_t get_committed_version() const { return 0; }
    virtual void set_version(int64_t v) { (void)v; }
    virtual void verify_consistency(int64_t db_version) { (void)db_version; }

    // Check if this is a storage operator
    bool is_storage_operator() const { return true; }
};

// ============================================================
// Internal Implementation Details
// ============================================================

/**
 * CustomOpEntry - Internal storage for registered operators
 */
struct CustomOpEntry {
    virtual ~CustomOpEntry() = default;
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

// ============================================================
// Step 4: Storage Customization - Types and Declarations
// ============================================================

/**
 * StorageOpEntry - Internal storage for registered storage operators
 * Extends CustomOpEntry with storage-specific fields.
 */
struct StorageOpEntry : public CustomOpEntry {
    StorageMode storage_mode_val;
    std::string source_table_name;  // For REPLICATED mode
    std::unique_ptr<StorageCustomOperator> persistent_instance;
    std::unique_ptr<VersionManager> version_manager;  // Shadow table version tracking

    StorageOpEntry() : CustomOpEntry(), storage_mode_val(StorageMode::OWNED) {}
};

// Forward declaration for storage vtab registration
void register_storage_vtab_wrapper(sqlite3* db, StorageOpEntry* entry);

/**
 * PreupdateRouter - Routes preupdate hooks to REPLICATED storage operators
 */
class PreupdateRouter {
public:
    static PreupdateRouter& instance();

    // Register a storage operator for REPLICATED mode
    void register_replica(sqlite3* db, const std::string& source_table, StorageOpEntry* entry);

    // Unregister when connection closes
    void unregister_db(sqlite3* db);

    // Get entry for a source table (called from hook)
    StorageOpEntry* find_by_source(sqlite3* db, const char* table_name);

    // Static callback for sqlite3_preupdate_hook
    static void preupdate_hook_callback(
        void* pCtx,
        sqlite3* db,
        int op,
        const char* zDb,
        const char* zTable,
        sqlite3_int64 oldRowid,
        sqlite3_int64 newRowid
    );

private:
    PreupdateRouter() = default;
    // Map: db -> (source_table -> entry)
    std::map<sqlite3*, std::map<std::string, StorageOpEntry*>> replicas_;
};

/**
 * Registration API for Storage Operators - call from plugin onInit()
 * 
 * Example:
 *   register_storage_op<ColumnarStorage>(db, "orders_col", {"id", "cat", "qty"});
 */
template<typename OpClass>
void register_storage_op(sqlite3* db, 
                         const std::string& name,
                         const std::vector<std::string>& cols);


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


// ============================================================
// Step 4: Storage Operator Registration Template Implementation  
// ============================================================

template<typename OpClass>
void register_storage_op(sqlite3* db, 
                         const std::string& name,
                         const std::vector<std::string>& cols) {
    // Create storage-specific entry
    auto entry = std::make_unique<StorageOpEntry>();
    entry->db = db;
    entry->output_columns = cols;
    entry->vtab_name = name;

    // Create persistent instance (storage operators persist across queries)
    auto instance = std::make_unique<OpClass>();
    entry->mode = instance->output_mode();
    entry->storage_mode_val = instance->storage_mode();

    if (entry->storage_mode_val == StorageMode::REPLICATED) {
        entry->source_table_name = instance->source_table();
    }

    // Store the persistent instance
    entry->persistent_instance = std::move(instance);
    entry->persistent_instance->_set_db(db);

    // Factory is not used for storage ops (we use persistent_instance)
    entry->factory = nullptr;

    auto& registry = CustomOpRegistry::for_db(db);

    // Register in registry - get stable pointer
    // Use static_cast since we know the entry is a StorageOpEntry
    StorageOpEntry* stable_ptr = static_cast<StorageOpEntry*>(
        registry.register_op(std::unique_ptr<CustomOpEntry>(entry.release()))
    );

    // Register with appropriate wrapper based on storage mode
    if (stable_ptr->storage_mode_val == StorageMode::OWNED) {
        register_storage_vtab_wrapper(db, stable_ptr);
    } else {
        // REPLICATED mode: register with preupdate hook router
        PreupdateRouter::instance().register_replica(db, stable_ptr->source_table_name, stable_ptr);
        // Also register vtab for query rewriting
        register_storage_vtab_wrapper(db, stable_ptr);
    }
}

} // namespace lyrore

#endif /* LYRORE_CUSTOM_OP_HPP */
