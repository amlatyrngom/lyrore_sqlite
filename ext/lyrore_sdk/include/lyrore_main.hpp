/*
** Lyrore Main - Centralized Entry Point
** 
** Singleton manager for all Lyrore operations:
** - Connection management (named connections with auto-configuration)
** - Plugin hot-reload with transaction-aware versioning
** - Shared resource registry for cross-plugin communication
** - Versioned function registration for transaction-consistent SQL functions
** - Hook dispatch coordination
*/
#ifndef LYRORE_MAIN_HPP
#define LYRORE_MAIN_HPP

#include <memory>
#include <string>
#include <map>
#include <vector>
#include <mutex>
#include <atomic>
#include <functional>
#include <any>

extern "C" {
struct sqlite3;
struct sqlite3_stmt;
struct sqlite3_context;
struct sqlite3_value;
struct Parse;
struct Select;
}

namespace lyrore {

class Plugin;
class PreParseContext;
class PreOptContext;
class EstimateContext;
class PostQueryContext;
class AnalyzeContext;

/**
 * LyroreMain - Centralized Lyrore singleton
 * 
 * Usage:
 *   auto main = LyroreMain::instance();
 *   sqlite3* db = main->get_db("mydb", ":memory:");
 *   main->reload_plugin("mydb", "fast_agg", "/path/to/plugin.so");
 *   // ... use db normally ...
 */
class LyroreMain : public std::enable_shared_from_this<LyroreMain> {
public:
    // Singleton access
    static std::shared_ptr<LyroreMain> instance();
    static void reset_instance();  // For testing only

    ~LyroreMain();

    // ===== Connection Management =====

    /**
     * Get or create a named database connection.
     * If the connection already exists, returns the existing one.
     * Automatically configures lyrore pragmas.
     */
    sqlite3* get_db(const std::string& name, const std::string& path = "",
                    int flags = 0);  // 0 = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE

    /**
     * Close a named database connection.
     */
    void close_db(const std::string& name);


    /**
     * Shutdown all connections and cleanup.
     */
    void shutdown();
    void register_with_sqlite();  // Register callbacks with SQLite core

    // ===== Plugin Management =====

    /**
     * Load or reload a plugin for a specific database.
     * Hot-reload: existing transactions continue with old version.
     * Returns SQLITE_OK on success.
     */
    int reload_plugin(const std::string& db_name, const std::string& plugin_name,
                      const std::string& so_path);

    /**
     * Unload a plugin (all versions).
     */
    void unload_plugin(const std::string& plugin_name);

    /**
     * Get the active plugin version for a database connection.
     * Returns the version appropriate for the current transaction state.
     */
    int64_t get_active_version(sqlite3* db, const std::string& plugin_name);

    // ===== Versioned Function Registration =====
    // These functions provide transaction-aware SQL function registration.
    // When a new plugin version is loaded, old transactions continue to see old function behavior.

    using ScalarFuncCallback = std::function<void(sqlite3_context*, int, sqlite3_value**)>;
    
    /**
     * Register a versioned scalar function.
     * The function will be associated with the current global version.
     * Transaction-aware: queries in transactions see the function version at transaction start.
     * 
     * @param db Database connection
     * @param plugin_name Name of the plugin (for version tracking)
     * @param func_name SQL function name
     * @param nArg Number of arguments (-1 for any)
     * @param callback The function implementation
     */
    void register_versioned_function(sqlite3* db, const std::string& plugin_name,
                                     const std::string& func_name, int nArg,
                                     ScalarFuncCallback callback);

    // ===== Hook Entry Points (called from SQLite core) =====

    int on_pre_parse(sqlite3* db, const char** pzSql, std::string& modified_sql);
    int on_pre_opt(sqlite3* db, void* pParse, void* pSelect);
    void on_estimate(sqlite3* db, void* pBuilder, void* pLoop);
    void on_post_query(sqlite3* db, void* pVdbe);
    void on_analyze(sqlite3* db, int iDb);

    // ===== Shared Resources =====

    /**
     * Set a shared resource accessible by all plugins.
     */
    template<typename T>
    void set_resource(const std::string& key, std::shared_ptr<T> resource) {
        std::lock_guard<std::mutex> lock(resource_mutex_);
        resources_[key] = resource;
    }

    /**
     * Get a shared resource.
     */
    template<typename T>
    std::shared_ptr<T> get_resource(const std::string& key) {
        std::lock_guard<std::mutex> lock(resource_mutex_);
        auto it = resources_.find(key);
        if (it == resources_.end()) return nullptr;
        return std::any_cast<std::shared_ptr<T>>(it->second);
    }

    /**
     * Check if a resource exists.
     */
    bool has_resource(const std::string& key) const;

    // ===== Internal: Connection Info =====

    struct ConnectionInfo {
        sqlite3* db = nullptr;
        std::string name;
        std::string path;
        bool in_transaction = false;
        int64_t version_snapshot = 0;
        // Track which plugin versions this connection holds (for refcount management)
        std::map<std::string, int64_t> held_versions;  // plugin_name -> version
    };

    ConnectionInfo* get_connection_info(sqlite3* db);
    ConnectionInfo* get_connection_info_by_name(const std::string& name);

    // Internal: Versioned function dispatch (called from wrapper)
    void dispatch_versioned_function(sqlite3_context* ctx, const std::string& func_key,
                                     int argc, sqlite3_value** argv);

private:
    LyroreMain();

    // Plugin version entry
    struct PluginVersion {
        void* handle = nullptr;
        Plugin* plugin = nullptr;
        int64_t version = 0;
        std::atomic<int> refcount{0};
        std::string so_path;
        bool initialized = false;  // Lazy init: onInit called when first selected
    };

    // Versioned function entry
    struct VersionedFuncEntry {
        int64_t version;
        ScalarFuncCallback callback;
    };

    // Cleanup old plugin versions with zero refcount
    void cleanup_old_versions(const std::string& plugin_name);
    
    // Cleanup all plugin versions that can be cleaned up
    void cleanup_all_old_versions();

    // Get plugins for a connection (respecting transaction versioning)
    std::vector<Plugin*> get_active_plugins(sqlite3* db, bool allow_init = true);

    // Update transaction state based on autocommit
    void update_transaction_state(sqlite3* db);
    
    // Snapshot plugin versions for a connection entering a transaction
    // Increments refcount on all snapshotted versions
    void snapshot_versions_for_connection(ConnectionInfo& conn);
    
    // Release held versions when a transaction ends
    // Decrements refcount on all held versions
    void release_held_versions(ConnectionInfo& conn);
    
    // Find the PluginVersion that would be selected for a given version number
    PluginVersion* find_version_for_snapshot(const std::string& plugin_name, int64_t version);

    // Data members
    std::map<std::string, ConnectionInfo> connections_;  // by name
    std::map<sqlite3*, std::string> db_to_name_;  // reverse lookup

    // Plugin versions: plugin_name -> list of versions (latest last)
    std::map<std::string, std::vector<std::unique_ptr<PluginVersion>>> plugin_versions_;

    // Versioned functions: func_key (db_ptr:plugin_name:func_name) -> list of versioned entries (latest last)
    std::map<std::string, std::vector<VersionedFuncEntry>> versioned_functions_;

    // Shared resources
    std::map<std::string, std::any> resources_;

    // Global version counter (incremented on each plugin load)
    std::atomic<int64_t> global_version_{0};

    // Thread safety
    mutable std::recursive_mutex mutex_;
    mutable std::mutex resource_mutex_;

    // Singleton instance
    static std::weak_ptr<LyroreMain> instance_;
    static std::mutex instance_mutex_;
};

} // namespace lyrore

#endif // LYRORE_MAIN_HPP
