/*
** Lyrore Main Implementation
*/
#include "lyrore_main.hpp"
#include "lyrore_plugin.hpp"
#include "lyrore_custom_op.hpp"

#include <dlfcn.h>
#include <iostream>
#include <algorithm>
#include <cstring>
#include <sstream>
#include <set>

extern "C" {
#include "sqlite3.h"
#include "lyrore_cabi.h"

// Forward declaration for pattern capture
extern "C" void lyrore_pattern_capture_preopt(void* pSelect);
}

namespace lyrore {

// Static singleton members
std::weak_ptr<LyroreMain> LyroreMain::instance_;
std::mutex LyroreMain::instance_mutex_;

LyroreMain::LyroreMain() {
    // Private constructor
}

LyroreMain::~LyroreMain() {
    shutdown();
}

std::shared_ptr<LyroreMain> LyroreMain::instance() {
    std::lock_guard<std::mutex> lock(instance_mutex_);
    auto ptr = instance_.lock();
    if (!ptr) {
        ptr = std::shared_ptr<LyroreMain>(new LyroreMain());
        instance_ = ptr;
        // Register callbacks with SQLite core
        ptr->register_with_sqlite();
    }
    return ptr;
}

void LyroreMain::reset_instance() {
    std::lock_guard<std::mutex> lock(instance_mutex_);
    instance_.reset();
}

// ===== Connection Management =====

sqlite3* LyroreMain::get_db(const std::string& name, const std::string& path, int flags) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    // Check if connection already exists
    auto it = connections_.find(name);
    if (it != connections_.end()) {
        return it->second.db;
    }

    // Use provided path or default to name
    std::string db_path = path.empty() ? name : path;

    // Use default flags if not specified
    if (flags == 0) {
        flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
    }

    // Open new connection
    sqlite3* db = nullptr;
    int rc = sqlite3_open_v2(db_path.c_str(), &db, flags, nullptr);
    if (rc != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return nullptr;
    }

    // Configure lyrore pragmas
    sqlite3_exec(db, "PRAGMA lyrore_enabled = ON;", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "PRAGMA lyrore_plugins = ON;", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "PRAGMA lyrore_cost = ON;", nullptr, nullptr, nullptr);

    // Set pLyroreMain on the connection so hooks can find LyroreMain
    lyrore_main_set_db(db, (LyroreMainHandle*)this);

    // Store connection info
    ConnectionInfo info;
    info.db = db;
    info.name = name;
    info.path = db_path;
    info.in_transaction = false;
    info.version_snapshot = 0;

    connections_[name] = info;
    db_to_name_[db] = name;

    return db;
}

void LyroreMain::close_db(const std::string& name) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    auto it = connections_.find(name);
    if (it == connections_.end()) return;

    sqlite3* db = it->second.db;
    db_to_name_.erase(db);
    connections_.erase(it);

    sqlite3_close(db);
}

void LyroreMain::shutdown() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    // Clear versioned functions FIRST - before closing connections
    versioned_functions_.clear();

    // Clear resources while plugin code is still loaded
    // (resources may contain objects with destructors in plugin code)
    {
        std::lock_guard<std::mutex> rlock(resource_mutex_);
        resources_.clear();
    }

    // Collect db pointers BEFORE closing (need them for registry cleanup)
    std::vector<sqlite3*> dbs_to_cleanup;
    for (auto& [name, info] : connections_) {
        dbs_to_cleanup.push_back(info.db);
    }

    // Close all connections
    for (auto& [name, info] : connections_) {
        sqlite3_close(info.db);
    }
    connections_.clear();
    db_to_name_.clear();

    // Clean up CustomOpRegistry AFTER sqlite3_close but BEFORE dlclose
    // This prevents the static g_registries destructor from accessing freed plugin memory
    for (sqlite3* db : dbs_to_cleanup) {
        lyrore::CustomOpRegistry::cleanup(db);
    }

    // Unload all plugins
    for (auto& [plugin_name, versions] : plugin_versions_) {
        for (auto& ver : versions) {
            if (ver->plugin) {
                ver->plugin->onShutdown();
                void (*destroy)(Plugin*) = (void(*)(Plugin*))dlsym(ver->handle, "lyrore_destroy_plugin");
                if (destroy) {
                    destroy(ver->plugin);
                } else {
                    delete ver->plugin;
                }
                ver->plugin = nullptr;
            }
            if (ver->handle) {
                dlclose(ver->handle);
                ver->handle = nullptr;
            }
        }
    }
    plugin_versions_.clear();
}

// ===== Plugin Management =====

int LyroreMain::reload_plugin(const std::string& db_name, const std::string& plugin_name,
                               const std::string& so_path) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    // Get the database connection
    auto conn_it = connections_.find(db_name);
    if (conn_it == connections_.end()) {
        return SQLITE_ERROR;
    }
    sqlite3* db = conn_it->second.db;

    // Load the plugin .so
    void* handle = dlopen(so_path.c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (!handle) {
        std::cerr << "LyroreMain: dlopen failed: " << dlerror() << std::endl;
        return SQLITE_ERROR;
    }

    // Get the create function
    Plugin* (*create)() = (Plugin*(*)())dlsym(handle, "lyrore_create_plugin");
    if (!create) {
        std::cerr << "LyroreMain: lyrore_create_plugin not found" << std::endl;
        dlclose(handle);
        return SQLITE_ERROR;
    }

    // Create plugin instance
    Plugin* plugin = create();
    if (!plugin) {
        dlclose(handle);
        return SQLITE_ERROR;
    }

    // Increment global version
    int64_t new_version = ++global_version_;

    // CRITICAL: Snapshot version for all in-transaction connections that have not been snapshotted yet
    // They should see the version BEFORE this reload
    for (auto& [name, conn] : connections_) {
        bool conn_in_txn = (sqlite3_get_autocommit(conn.db) == 0);
        if (conn_in_txn && !conn.in_transaction) {
            conn.in_transaction = true;
            conn.version_snapshot = new_version - 1;  // Snapshot the PREVIOUS version
        }
    }

    // Create version entry
    auto version_entry = std::make_unique<PluginVersion>();
    version_entry->handle = handle;
    version_entry->plugin = plugin;
    version_entry->version = new_version;
    version_entry->refcount = 0;
    version_entry->so_path = so_path;

    // Set lyrore_main reference on plugin
    plugin->set_lyrore_main(this);

    // Call onInit immediately so versioned functions are registered with the correct version
    // The version was already snapshotted above, so in-transaction connections won't pick up new functions
    plugin->onInit(db);
    version_entry->initialized = true;

    // Add to versions list
    plugin_versions_[plugin_name].push_back(std::move(version_entry));

    // Cleanup old versions with zero refcount
    cleanup_old_versions(plugin_name);

    return SQLITE_OK;
}

void LyroreMain::unload_plugin(const std::string& plugin_name) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    auto it = plugin_versions_.find(plugin_name);
    if (it == plugin_versions_.end()) return;

    for (auto& ver : it->second) {
        if (ver->plugin) {
            ver->plugin->onShutdown();
            void (*destroy)(Plugin*) = (void(*)(Plugin*))dlsym(ver->handle, "lyrore_destroy_plugin");
            if (destroy) {
                destroy(ver->plugin);
            } else {
                delete ver->plugin;
            }
            ver->plugin = nullptr;
        }
        if (ver->handle) {
            dlclose(ver->handle);
            ver->handle = nullptr;
        }
    }

    plugin_versions_.erase(it);
}

void LyroreMain::cleanup_old_versions(const std::string& plugin_name) {
    // Called with mutex held
    auto it = plugin_versions_.find(plugin_name);
    if (it == plugin_versions_.end()) return;

    auto& versions = it->second;

    // Find the minimum version still needed by any in-transaction connection
    int64_t min_needed_version = global_version_.load();
    for (const auto& [name, conn] : connections_) {
        if (conn.in_transaction && conn.version_snapshot < min_needed_version) {
            min_needed_version = conn.version_snapshot;
        }
    }

    // Keep at least the latest version, remove old ones that are no longer needed
    while (versions.size() > 1) {
        auto& oldest = versions.front();
        // Don't remove if this version could still be selected by any snapshot
        if (oldest->version > min_needed_version) {
            break;  // This and newer versions may be needed
        }
        // Also check for version just at the boundary
        if (oldest->version >= min_needed_version && versions.size() > 1) {
            // Check if there's a newer version that could serve min_needed_version
            bool has_suitable_newer = false;
            for (size_t i = 1; i < versions.size(); i++) {
                if (versions[i]->version <= min_needed_version) {
                    has_suitable_newer = true;
                    break;
                }
            }
            if (!has_suitable_newer) {
                break;  // Can't remove - this is the best match for min_needed_version
            }
        }
        if (oldest->refcount.load() == 0) {
            if (oldest->plugin) {
                oldest->plugin->onShutdown();
                void (*destroy)(Plugin*) = (void(*)(Plugin*))dlsym(oldest->handle, "lyrore_destroy_plugin");
                if (destroy) {
                    destroy(oldest->plugin);
                } else {
                    delete oldest->plugin;
                }
            }
            if (oldest->handle) {
                dlclose(oldest->handle);
            }
            versions.erase(versions.begin());
        } else {
            break;  // Can't remove if still in use
        }
    }
}

int64_t LyroreMain::get_active_version(sqlite3* db, const std::string& plugin_name) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    auto db_it = db_to_name_.find(db);
    if (db_it == db_to_name_.end()) return -1;

    auto conn_it = connections_.find(db_it->second);
    if (conn_it == connections_.end()) return -1;

    auto& conn = conn_it->second;

    // Check transaction state
    bool in_txn = (sqlite3_get_autocommit(db) == 0);

    if (in_txn && !conn.in_transaction) {
        // Just entered transaction - snapshot version
        conn.in_transaction = true;
        conn.version_snapshot = global_version_.load();
    } else if (!in_txn && conn.in_transaction) {
        // Transaction ended
        conn.in_transaction = false;
    }

    int64_t use_version = in_txn ? conn.version_snapshot : global_version_.load();

    auto ver_it = plugin_versions_.find(plugin_name);
    if (ver_it == plugin_versions_.end() || ver_it->second.empty()) return -1;

    // Find the highest version <= use_version
    for (auto rit = ver_it->second.rbegin(); rit != ver_it->second.rend(); ++rit) {
        if ((*rit)->version <= use_version) {
            return (*rit)->version;
        }
    }

    return -1;
}

std::vector<Plugin*> LyroreMain::get_active_plugins(sqlite3* db, bool allow_init) {
    // Called with mutex held
    std::vector<Plugin*> result;

    auto db_it = db_to_name_.find(db);
    if (db_it == db_to_name_.end()) return result;

    auto conn_it = connections_.find(db_it->second);
    if (conn_it == connections_.end()) return result;

    auto& conn = conn_it->second;

    // Update transaction state
    bool in_txn = (sqlite3_get_autocommit(db) == 0);

    if (in_txn && !conn.in_transaction) {
        conn.in_transaction = true;
        conn.version_snapshot = global_version_.load();
    } else if (!in_txn && conn.in_transaction) {
        conn.in_transaction = false;
    }

    int64_t use_version = in_txn ? conn.version_snapshot : global_version_.load();

    // Get the appropriate version of each plugin
    for (auto& [name, versions] : plugin_versions_) {
        if (versions.empty()) continue;

        // Find highest version <= use_version
        PluginVersion* selected = nullptr;
        for (auto rit = versions.rbegin(); rit != versions.rend(); ++rit) {
            if ((*rit)->version <= use_version) {
                selected = rit->get();
                break;
            }
        }

        if (selected && selected->plugin) {
            // Lazy initialization: call onInit when first selected (only if safe)
            if (!selected->initialized && allow_init) {
                selected->plugin->onInit(conn.db);
                selected->initialized = true;
            }
            // Only add to result if initialized (skip uninitialized during post_query)
            if (selected->initialized) {
                result.push_back(selected->plugin);
            }
        }
    }

    return result;
}

LyroreMain::ConnectionInfo* LyroreMain::get_connection_info(sqlite3* db) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto db_it = db_to_name_.find(db);
    if (db_it == db_to_name_.end()) return nullptr;
    auto conn_it = connections_.find(db_it->second);
    if (conn_it == connections_.end()) return nullptr;
    return &conn_it->second;
}

LyroreMain::ConnectionInfo* LyroreMain::get_connection_info_by_name(const std::string& name) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto it = connections_.find(name);
    if (it == connections_.end()) return nullptr;
    return &it->second;
}

bool LyroreMain::has_resource(const std::string& key) const {
    std::lock_guard<std::mutex> lock(resource_mutex_);
    return resources_.find(key) != resources_.end();
}

// ===== Versioned Function Registration =====

// Structure to hold function dispatch info (stored as user data for SQLite function)
struct VersionedFuncUserData {
    LyroreMain* main;
    std::string func_key;  // "db_ptr:plugin:func_name"
};

// Static wrapper function that dispatches to the correct version
static void versioned_func_wrapper(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
    auto* ud = static_cast<VersionedFuncUserData*>(sqlite3_user_data(ctx));
    if (ud && ud->main) {
        ud->main->dispatch_versioned_function(ctx, ud->func_key, argc, argv);
    }
}

// Static destructor for user data
static void versioned_func_destroy(void* ptr) {
    delete static_cast<VersionedFuncUserData*>(ptr);
}

void LyroreMain::register_versioned_function(sqlite3* db, const std::string& plugin_name,
                                              const std::string& func_name, int nArg,
                                              ScalarFuncCallback callback) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    // Create the function key
    std::ostringstream oss;
    oss << reinterpret_cast<uintptr_t>(db) << ":" << plugin_name << ":" << func_name;
    std::string func_key = oss.str();

    // Get the current global version
    int64_t current_version = global_version_.load();

    // Add the versioned entry
    VersionedFuncEntry entry;
    entry.version = current_version;
    entry.callback = callback;
    versioned_functions_[func_key].push_back(entry);

    // Always register/re-register the wrapper function
    // SQLite will replace existing functions with the same name
    // Create user data for the wrapper
    auto* ud = new VersionedFuncUserData{this, func_key};

    // Register the wrapper function with SQLite
    int rc = sqlite3_create_function_v2(
        db,
        func_name.c_str(),
        nArg,
        SQLITE_UTF8,
        ud,
        versioned_func_wrapper,
        nullptr,
        nullptr,
        versioned_func_destroy
    );

    if (rc != SQLITE_OK) {
        delete ud;
    }
}

void LyroreMain::dispatch_versioned_function(sqlite3_context* ctx, const std::string& func_key,
                                              int argc, sqlite3_value** argv) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    sqlite3* db = sqlite3_context_db_handle(ctx);

    // Determine the version to use based on transaction state
    int64_t use_version = global_version_.load();

    auto db_it = db_to_name_.find(db);
    if (db_it != db_to_name_.end()) {
        auto conn_it = connections_.find(db_it->second);
        if (conn_it != connections_.end()) {
            auto& conn = conn_it->second;
            bool in_txn = (sqlite3_get_autocommit(db) == 0);

            if (in_txn && !conn.in_transaction) {
                conn.in_transaction = true;
                conn.version_snapshot = global_version_.load();
            } else if (!in_txn && conn.in_transaction) {
                conn.in_transaction = false;
            }

            if (in_txn) {
                use_version = conn.version_snapshot;
            }
        }
    }

    // Find the function entry for this key
    auto it = versioned_functions_.find(func_key);
    if (it == versioned_functions_.end() || it->second.empty()) {
        sqlite3_result_error(ctx, "Versioned function not found", -1);
        return;
    }

    // Find the highest version <= use_version
    const VersionedFuncEntry* selected = nullptr;
    for (auto rit = it->second.rbegin(); rit != it->second.rend(); ++rit) {
        if (rit->version <= use_version) {
            selected = &(*rit);
            break;
        }
    }

    if (!selected) {
        sqlite3_result_error(ctx, "No matching function version found", -1);
        return;
    }

    // Call the selected callback
    selected->callback(ctx, argc, argv);
}

// ===== Hook Entry Points =====

int LyroreMain::on_pre_parse(sqlite3* db, const char** pzSql, std::string& modified_sql) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    auto plugins = get_active_plugins(db, true);  // allow_init=true
    if (plugins.empty()) return SQLITE_OK;

    // Create PreParseContext
    PreParseContext ctx(db, *pzSql);

    for (auto* plugin : plugins) {
        plugin->onPreParse(ctx);
    }

    if (ctx.modified()) {
        modified_sql = ctx.sql();
        *pzSql = modified_sql.c_str();
    }

    return SQLITE_OK;
}

int LyroreMain::on_pre_opt(sqlite3* db, void* pParse, void* pSelect) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    // Check for pattern capture mode (needed for Pattern::from_query initialization)
    // This must be called BEFORE creating PreOptContext
    lyrore_pattern_capture_preopt(pSelect);

    PreOptContext ctx(db, static_cast<Parse*>(pParse), static_cast<Select*>(pSelect));

    // Call plugin hooks
    auto plugins = get_active_plugins(db, true);  // allow_init=true
    for (auto* plugin : plugins) {
        plugin->onPreOpt(ctx);
    }

    // Perform custom operator AST rewrites
    // IMPORTANT: Call even if no plugins - custom ops are in separate registry
    lyrore::rewrite_custom_ops(ctx);

    return SQLITE_OK;
}

void LyroreMain::on_estimate(sqlite3* db, void* pBuilder, void* pLoop) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    auto plugins = get_active_plugins(db, false);  // allow_init=false
    if (plugins.empty()) return;

    EstimateContext ctx(db, pBuilder, pLoop);

    for (auto* plugin : plugins) {
        plugin->onEstimate(ctx);
    }
}

void LyroreMain::on_post_query(sqlite3* db, void* pVdbe) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    auto plugins = get_active_plugins(db, false);  // allow_init=false (SQLITE_BUSY risk)
    if (plugins.empty()) return;

    PostQueryContext ctx(db, pVdbe);

    for (auto* plugin : plugins) {
        plugin->onPostQuery(ctx);
    }
}

void LyroreMain::on_analyze(sqlite3* db, int iDb) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    auto plugins = get_active_plugins(db, true);  // allow_init=true
    if (plugins.empty()) return;

    AnalyzeContext ctx(db);

    for (auto* plugin : plugins) {
        plugin->onAnalyze(ctx);
    }
}

} // namespace lyrore


// ===== Callback Functions for SQLite Core =====

namespace {

// Flag to track if callbacks are registered
static bool g_callbacks_registered = false;

// Callback function for pre-parse hook
int preparse_callback(sqlite3* db, const char* zSql, char** pzModified) {
    *pzModified = nullptr;

    auto main = lyrore::LyroreMain::instance();
    if (!main) return SQLITE_OK;

    std::string modified_sql;
    int rc = main->on_pre_parse(db, &zSql, modified_sql);

    if (rc != SQLITE_OK) return rc;

    if (!modified_sql.empty()) {
        // Allocate with sqlite3_malloc so caller can free with sqlite3_free
        *pzModified = (char*)sqlite3_malloc64(modified_sql.size() + 1);
        if (*pzModified) {
            memcpy(*pzModified, modified_sql.c_str(), modified_sql.size() + 1);
        } else {
            return SQLITE_NOMEM;
        }
    }

    return SQLITE_OK;
}

// Callback function for pre-opt hook
int preopt_callback(void* pLyroreMain, sqlite3* db, void* pParse, void* pSelect) {
    auto main = lyrore::LyroreMain::instance();
    if (!main) return SQLITE_OK;
    return main->on_pre_opt(db, pParse, pSelect);
}

// Callback function for estimate hook
void estimate_callback(void* pLyroreMain, sqlite3* db, void* pBuilder, void* pLoop) {
    auto main = lyrore::LyroreMain::instance();
    if (!main) return;
    main->on_estimate(db, pBuilder, pLoop);
}

// Callback function for post-query hook
void postquery_callback(void* pLyroreMain, sqlite3* db, void* pVdbe) {
    auto main = lyrore::LyroreMain::instance();
    if (!main) return;
    main->on_post_query(db, pVdbe);
}

// Callback function for analyze hook
void analyze_callback(void* pLyroreMain, sqlite3* db, int iDb) {
    auto main = lyrore::LyroreMain::instance();
    if (!main) return;
    main->on_analyze(db, iDb);
}

// Register callbacks with SQLite core
void register_callbacks_if_needed() {
    if (g_callbacks_registered) return;

    lyrore_register_main_callbacks(
        preparse_callback,
        preopt_callback,
        estimate_callback,
        postquery_callback,
        analyze_callback
    );

    g_callbacks_registered = true;
}

} // anonymous namespace

namespace lyrore {

// Update instance() to register callbacks
void LyroreMain::register_with_sqlite() {
    register_callbacks_if_needed();
}

} // namespace lyrore

