/*
** Lyrore Preupdate Hook Router
**
** Routes SQLite preupdate hooks to REPLICATED mode storage operators.
** Allows storage operators to mirror changes from existing tables.
*/

#include "lyrore_custom_op.hpp"

extern "C" {
#include "sqlite3.h"
}

namespace lyrore {

// ============================================================
// PreupdateRouter Implementation
// ============================================================

PreupdateRouter& PreupdateRouter::instance() {
    static PreupdateRouter singleton;
    return singleton;
}

void PreupdateRouter::register_replica(sqlite3* db, const std::string& source_table, StorageOpEntry* entry) {
    // Install preupdate hook if this is the first replica for this db
    auto db_it = replicas_.find(db);
    if (db_it == replicas_.end()) {
        // First replica for this db - install the hook
        sqlite3_preupdate_hook(db, preupdate_hook_callback, this);
        replicas_[db] = {};
    }

    // Register this entry for the source table
    replicas_[db][source_table] = entry;
}

void PreupdateRouter::unregister_db(sqlite3* db) {
    auto it = replicas_.find(db);
    if (it != replicas_.end()) {
        replicas_.erase(it);
        // Remove the hook
        sqlite3_preupdate_hook(db, nullptr, nullptr);
    }
}

StorageOpEntry* PreupdateRouter::find_by_source(sqlite3* db, const char* table_name) {
    auto db_it = replicas_.find(db);
    if (db_it == replicas_.end()) return nullptr;

    auto& table_map = db_it->second;
    auto it = table_map.find(table_name);
    if (it == table_map.end()) return nullptr;

    return it->second;
}

// ============================================================
// Preupdate Hook Callback
// ============================================================

void PreupdateRouter::preupdate_hook_callback(
    void* pCtx,
    sqlite3* db,
    int op,
    const char* zDb,
    const char* zTable,
    sqlite3_int64 oldRowid,
    sqlite3_int64 newRowid
) {
    (void)zDb;  // We only care about main database for now

    PreupdateRouter* router = static_cast<PreupdateRouter*>(pCtx);
    StorageOpEntry* entry = router->find_by_source(db, zTable);
    if (!entry || !entry->persistent_instance) return;

    // Convert SQLite operation to UpdateOp
    UpdateOp update_op;
    switch (op) {
        case SQLITE_INSERT: update_op = UpdateOp::INSERT; break;
        case SQLITE_UPDATE: update_op = UpdateOp::UPDATE; break;
        case SQLITE_DELETE: update_op = UpdateOp::DELETE; break;
        default: return;  // Unknown operation
    }

    std::vector<LyValue> old_vals, new_vals;
    int nCol = sqlite3_preupdate_count(db);

    // Get old values (for UPDATE and DELETE)
    if (op == SQLITE_UPDATE || op == SQLITE_DELETE) {
        for (int i = 0; i < nCol; i++) {
            sqlite3_value* val = nullptr;
            if (sqlite3_preupdate_old(db, i, &val) == SQLITE_OK && val) {
                old_vals.push_back(sqlite3_value_to_lyvalue(val));
            } else {
                old_vals.push_back(LyValue{});  // NULL
            }
        }
    }

    // Get new values (for UPDATE and INSERT)
    if (op == SQLITE_UPDATE || op == SQLITE_INSERT) {
        for (int i = 0; i < nCol; i++) {
            sqlite3_value* val = nullptr;
            if (sqlite3_preupdate_new(db, i, &val) == SQLITE_OK && val) {
                new_vals.push_back(sqlite3_value_to_lyvalue(val));
            } else {
                new_vals.push_back(LyValue{});  // NULL
            }
        }
    }

    // Call the storage operator's on_update
    entry->persistent_instance->on_update(
        update_op,
        static_cast<int64_t>(oldRowid),
        static_cast<int64_t>(newRowid),
        old_vals,
        new_vals
    );
}

} // namespace lyrore
