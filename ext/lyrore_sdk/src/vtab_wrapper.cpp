/*
** Lyrore Virtual Table Wrapper
**
** Wraps CustomOperator with ITERATOR/CURSOR output mode as a SQLite virtual table.
** Uses sqlite3_create_module_v2 for registration.
*/

#include "lyrore_custom_op.hpp"
#include <cstring>

extern "C" {
#include "sqlite3.h"
#include "lyrore_cabi.h"
}

namespace lyrore {

// ============================================================
// Virtual Table Structures
// ============================================================

struct LyroreVtab : sqlite3_vtab {
    sqlite3* db;
    CustomOpEntry* entry;

    LyroreVtab() {
        memset(static_cast<sqlite3_vtab*>(this), 0, sizeof(sqlite3_vtab));
        db = nullptr;
        entry = nullptr;
    }
};

struct LyroreVtabCursor : sqlite3_vtab_cursor {
    LyroreVtab* vtab;
    std::unique_ptr<CustomOperator> op;
    MatchParams params;
    int64_t rowid;
    bool eof;
    std::vector<LyValue> current_row;

    LyroreVtabCursor() {
        memset(static_cast<sqlite3_vtab_cursor*>(this), 0, sizeof(sqlite3_vtab_cursor));
        vtab = nullptr;
        rowid = 0;
        eof = true;
    }
};

// ============================================================
// Virtual Table Module Implementation
// ============================================================

static int vtab_connect(
    sqlite3* db,
    void* pAux,
    int argc,
    const char* const* argv,
    sqlite3_vtab** ppVtab,
    char** pzErr
) {
    (void)argc; (void)pzErr;

        CustomOpEntry* entry = static_cast<CustomOpEntry*>(pAux);
    if (!entry) {

        return SQLITE_ERROR;
    }

    // Build schema from output_columns
    std::string schema = "CREATE TABLE x(";
    for (size_t i = 0; i < entry->output_columns.size(); i++) {
        if (i > 0) schema += ", ";
        schema += "\"" + entry->output_columns[i] + "\"";
    }
    schema += ")";

    int rc = sqlite3_declare_vtab(db, schema.c_str());

    if (rc != SQLITE_OK) return rc;

    LyroreVtab* vtab = new LyroreVtab();
    vtab->db = db;
    vtab->entry = entry;
    *ppVtab = vtab;

    return SQLITE_OK;
}

static int vtab_disconnect(sqlite3_vtab* pVtab) {
    LyroreVtab* vtab = static_cast<LyroreVtab*>(pVtab);
    delete vtab;
    return SQLITE_OK;
}

static int vtab_best_index(sqlite3_vtab* pVtab, sqlite3_index_info* pInfo) {
    (void)pVtab;

    // Mark all constraints as usable and to be passed to xFilter
    int argvIndex = 1;
    for (int i = 0; i < pInfo->nConstraint; i++) {
        if (pInfo->aConstraint[i].usable) {
            pInfo->aConstraintUsage[i].argvIndex = argvIndex++;
            pInfo->aConstraintUsage[i].omit = 1;  // We handle the constraint
        }
    }

    // Low estimated cost since we're optimized
    pInfo->estimatedCost = 1.0;
    pInfo->estimatedRows = 10;

    return SQLITE_OK;
}

static int vtab_open(sqlite3_vtab* pVtab, sqlite3_vtab_cursor** ppCursor) {
    LyroreVtab* vtab = static_cast<LyroreVtab*>(pVtab);
    LyroreVtabCursor* cur = new LyroreVtabCursor();
    cur->vtab = vtab;
    cur->pVtab = pVtab;
    *ppCursor = cur;
    return SQLITE_OK;
}

static int vtab_close(sqlite3_vtab_cursor* pCursor) {
    LyroreVtabCursor* cur = static_cast<LyroreVtabCursor*>(pCursor);
    delete cur;
    return SQLITE_OK;
}

static int vtab_filter(
    sqlite3_vtab_cursor* pCursor,
    int idxNum,
    const char* idxStr,
    int argc,
    sqlite3_value** argv
) {
    (void)idxNum; (void)idxStr;

    LyroreVtabCursor* cur = static_cast<LyroreVtabCursor*>(pCursor);
    LyroreVtab* vtab = cur->vtab;

    // Create operator instance
    cur->op = vtab->entry->factory();
    if (!cur->op) {
        return SQLITE_ERROR;
    }

    cur->op->_set_db(vtab->db);

    // Set parameters from argv
    cur->params = MatchParams();
    for (int i = 0; i < argc; i++) {
        std::string name = "$" + std::to_string(i + 1);
        LyValue val = sqlite3_value_to_lyvalue(argv[i]);
        cur->params.set(name, val);
    }
    cur->op->_set_params(&cur->params);

    // Execute operator
    cur->op->execute();

    // Start iteration
    cur->rowid = 0;
    cur->eof = !cur->op->iterator_next();
    if (!cur->eof) {
        cur->current_row = cur->op->iterator_get_row();
    }

    return SQLITE_OK;
}

static int vtab_next(sqlite3_vtab_cursor* pCursor) {
    LyroreVtabCursor* cur = static_cast<LyroreVtabCursor*>(pCursor);

    cur->rowid++;
    cur->eof = !cur->op->iterator_next();
    if (!cur->eof) {
        cur->current_row = cur->op->iterator_get_row();
    }

    return SQLITE_OK;
}

static int vtab_eof(sqlite3_vtab_cursor* pCursor) {
    LyroreVtabCursor* cur = static_cast<LyroreVtabCursor*>(pCursor);
    return cur->eof ? 1 : 0;
}

static int vtab_column(sqlite3_vtab_cursor* pCursor, sqlite3_context* ctx, int col) {
    LyroreVtabCursor* cur = static_cast<LyroreVtabCursor*>(pCursor);

    if (col >= 0 && col < static_cast<int>(cur->current_row.size())) {
        set_sqlite3_result(ctx, cur->current_row[col]);
    } else {
        sqlite3_result_null(ctx);
    }

    return SQLITE_OK;
}

static int vtab_rowid(sqlite3_vtab_cursor* pCursor, sqlite3_int64* pRowid) {
    LyroreVtabCursor* cur = static_cast<LyroreVtabCursor*>(pCursor);
    *pRowid = cur->rowid;
    return SQLITE_OK;
}

// ============================================================
// Module Definition
// ============================================================

static sqlite3_module lyrore_vtab_module = {
    0,                    // iVersion
    vtab_connect,         // xCreate - same as xConnect for proper CREATE VIRTUAL TABLE support
    vtab_connect,         // xConnect
    vtab_best_index,      // xBestIndex
    vtab_disconnect,      // xDisconnect
    vtab_disconnect,      // xDestroy - same as xDisconnect for proper DROP TABLE support
    vtab_open,            // xOpen
    vtab_close,           // xClose
    vtab_filter,          // xFilter
    vtab_next,            // xNext
    vtab_eof,             // xEof
    vtab_column,          // xColumn
    vtab_rowid,           // xRowid
    nullptr,              // xUpdate
    nullptr,              // xBegin
    nullptr,              // xSync
    nullptr,              // xCommit
    nullptr,              // xRollback
    nullptr,              // xFindFunction
    nullptr,              // xRename
    nullptr,              // xSavepoint
    nullptr,              // xRelease
    nullptr,              // xRollbackTo
    nullptr,              // xShadowName
    nullptr               // xIntegrity
};

// ============================================================
// Registration Function
// ============================================================

void register_vtab_wrapper(sqlite3* db, CustomOpEntry* entry) {
    if (!db || !entry || !entry->factory) return;

    // Entry is heap-allocated via unique_ptr in registry
    // Pointer is stable for lifetime of connection

    // For eponymous virtual tables, we need to register AND create the table
    // Register the module first
    int rc = sqlite3_create_module_v2(
        db,
        entry->vtab_name.c_str(),
        &lyrore_vtab_module,
        entry,              // pAux - stable pointer to entry
        nullptr             // xDestroy - registry handles cleanup
    );

    if (rc != SQLITE_OK) {

        return;
    }

    // For eponymous-only vtabs, we need to create a virtual table statement
    // The module registration alone doesn't make it queryable without CREATE VIRTUAL TABLE
    std::string create_sql = "CREATE VIRTUAL TABLE " + entry->vtab_name +
                             " USING " + entry->vtab_name;

    char* errmsg = nullptr;
    rc = sqlite3_exec(db, create_sql.c_str(), nullptr, nullptr, &errmsg);
    if (rc != SQLITE_OK) {

    }
    if (errmsg) {
        sqlite3_free(errmsg);
    }
    // Immediately capture Table* after vtab creation
    // This works because schema is loaded during CREATE VIRTUAL TABLE

    Table* pTab = lyrore_sqlite3FindTable(db, entry->vtab_name.c_str(), "main");

    if (!pTab) {
        // Try without schema qualifier

        pTab = lyrore_sqlite3FindTable(db, entry->vtab_name.c_str(), nullptr);

    }
    if (pTab) {
        entry->pVtabTable = pTab;

    } else {

    }
}



// ============================================================
// Step 4: Storage Virtual Table Module
// ============================================================

struct StorageLyroreVtab : sqlite3_vtab {
    sqlite3* db;
    StorageOpEntry* entry;
    StorageCustomOperator* op_instance;  // Persistent operator instance

    StorageLyroreVtab() {
        memset(static_cast<sqlite3_vtab*>(this), 0, sizeof(sqlite3_vtab));
        db = nullptr;
        entry = nullptr;
        op_instance = nullptr;
    }
};

struct StorageLyroreVtabCursor : sqlite3_vtab_cursor {
    StorageLyroreVtab* vtab;
    int64_t rowid;
    bool eof;
    std::vector<LyValue> current_row;
    bool first_next_called;

    StorageLyroreVtabCursor() {
        memset(static_cast<sqlite3_vtab_cursor*>(this), 0, sizeof(sqlite3_vtab_cursor));
        vtab = nullptr;
        rowid = 0;
        eof = true;
        first_next_called = false;
    }
};

// ============================================================
// Storage VTab Method Implementations
// ============================================================

static int storage_vtab_connect(
    sqlite3* db,
    void* pAux,
    int argc,
    const char* const* argv,
    sqlite3_vtab** ppVtab,
    char** pzErr
) {
    (void)argc; (void)argv; (void)pzErr;

    StorageOpEntry* entry = static_cast<StorageOpEntry*>(pAux);
    if (!entry || !entry->persistent_instance) {
        return SQLITE_ERROR;
    }

    // Build schema from output_columns
    std::string schema = "CREATE TABLE x(";
    for (size_t i = 0; i < entry->output_columns.size(); i++) {
        if (i > 0) schema += ", ";
        schema += "\"" + entry->output_columns[i] + "\"";
    }
    schema += ")";

    int rc = sqlite3_declare_vtab(db, schema.c_str());
    if (rc != SQLITE_OK) return rc;

    StorageLyroreVtab* vtab = new StorageLyroreVtab();
    vtab->db = db;
    vtab->entry = entry;
    vtab->op_instance = entry->persistent_instance.get();
    *ppVtab = vtab;

    return SQLITE_OK;
}

static int storage_vtab_disconnect(sqlite3_vtab* pVtab) {
    StorageLyroreVtab* vtab = static_cast<StorageLyroreVtab*>(pVtab);
    delete vtab;
    return SQLITE_OK;
}

static int storage_vtab_open(sqlite3_vtab* pVtab, sqlite3_vtab_cursor** ppCursor) {
    StorageLyroreVtab* vtab = static_cast<StorageLyroreVtab*>(pVtab);
    StorageLyroreVtabCursor* cur = new StorageLyroreVtabCursor();
    cur->vtab = vtab;
    cur->pVtab = pVtab;
    *ppCursor = cur;
    return SQLITE_OK;
}

static int storage_vtab_close(sqlite3_vtab_cursor* pCursor) {
    StorageLyroreVtabCursor* cur = static_cast<StorageLyroreVtabCursor*>(pCursor);
    delete cur;
    return SQLITE_OK;
}

static int storage_vtab_best_index(sqlite3_vtab* pVtab, sqlite3_index_info* pInfo) {
    (void)pVtab;
    // Low estimated cost - we're optimized
    pInfo->estimatedCost = 1.0;
    pInfo->estimatedRows = 100;
    return SQLITE_OK;
}

static int storage_vtab_filter(
    sqlite3_vtab_cursor* pCursor,
    int idxNum,
    const char* idxStr,
    int argc,
    sqlite3_value** argv
) {
    (void)idxNum; (void)idxStr; (void)argc; (void)argv;

    StorageLyroreVtabCursor* cur = static_cast<StorageLyroreVtabCursor*>(pCursor);
    StorageLyroreVtab* vtab = cur->vtab;

    // Reset iterator
    vtab->op_instance->iterator_reset();
    cur->rowid = 0;
    cur->first_next_called = false;
    cur->eof = false;

    // Get first row
    cur->eof = !vtab->op_instance->iterator_next();
    cur->first_next_called = true;
    if (!cur->eof) {
        cur->current_row = vtab->op_instance->iterator_get_row();
    }

    return SQLITE_OK;
}

static int storage_vtab_next(sqlite3_vtab_cursor* pCursor) {
    StorageLyroreVtabCursor* cur = static_cast<StorageLyroreVtabCursor*>(pCursor);
    StorageLyroreVtab* vtab = cur->vtab;

    cur->rowid++;
    cur->eof = !vtab->op_instance->iterator_next();
    if (!cur->eof) {
        cur->current_row = vtab->op_instance->iterator_get_row();
    }

    return SQLITE_OK;
}

static int storage_vtab_eof(sqlite3_vtab_cursor* pCursor) {
    StorageLyroreVtabCursor* cur = static_cast<StorageLyroreVtabCursor*>(pCursor);
    return cur->eof ? 1 : 0;
}

static int storage_vtab_column(sqlite3_vtab_cursor* pCursor, sqlite3_context* ctx, int col) {
    StorageLyroreVtabCursor* cur = static_cast<StorageLyroreVtabCursor*>(pCursor);

    if (col >= 0 && col < static_cast<int>(cur->current_row.size())) {
        set_sqlite3_result(ctx, cur->current_row[col]);
    } else {
        sqlite3_result_null(ctx);
    }

    return SQLITE_OK;
}

static int storage_vtab_rowid(sqlite3_vtab_cursor* pCursor, sqlite3_int64* pRowid) {
    StorageLyroreVtabCursor* cur = static_cast<StorageLyroreVtabCursor*>(pCursor);
    // Get the actual rowid from the storage operator
    *pRowid = cur->vtab->op_instance->get_current_rowid();
    return SQLITE_OK;
}

// ============================================================
// xUpdate - Handle INSERT/UPDATE/DELETE
// ============================================================

static int storage_vtab_update(
    sqlite3_vtab* pVtab,
    int argc,
    sqlite3_value** argv,
    sqlite3_int64* pRowid
) {
    StorageLyroreVtab* vtab = static_cast<StorageLyroreVtab*>(pVtab);
    UpdateOp op;
    int64_t old_rowid = -1, new_rowid = -1;
    std::vector<LyValue> old_vals, new_vals;

    // Decode xUpdate arguments per SQLite vtab protocol:
    // DELETE: argc=1, argv[0]=rowid_to_delete
    // INSERT: argc>1, argv[0]=NULL, argv[1]=new_rowid (or NULL for auto), argv[2+]=values
    // UPDATE: argc>1, argv[0]=old_rowid, argv[1]=new_rowid, argv[2+]=new_values

    if (argc == 1) {
        // DELETE
        op = UpdateOp::DELETE;
        old_rowid = sqlite3_value_int64(argv[0]);
    } else if (sqlite3_value_type(argv[0]) == SQLITE_NULL) {
        // INSERT
        op = UpdateOp::INSERT;
        if (sqlite3_value_type(argv[1]) != SQLITE_NULL) {
            new_rowid = sqlite3_value_int64(argv[1]);
        }
        for (int i = 2; i < argc; i++) {
            new_vals.push_back(sqlite3_value_to_lyvalue(argv[i]));
        }
    } else {
        // UPDATE
        op = UpdateOp::UPDATE;
        old_rowid = sqlite3_value_int64(argv[0]);
        new_rowid = sqlite3_value_int64(argv[1]);
        for (int i = 2; i < argc; i++) {
            new_vals.push_back(sqlite3_value_to_lyvalue(argv[i]));
        }
    }

    int rc = vtab->op_instance->on_update(op, old_rowid, new_rowid, old_vals, new_vals);
    if (rc == SQLITE_OK && pRowid && op == UpdateOp::INSERT) {
        *pRowid = new_rowid;
    }
    return rc;
}

// ============================================================
// Transaction Hooks
// ============================================================

static int storage_vtab_begin(sqlite3_vtab* pVtab) {
    StorageLyroreVtab* vtab = static_cast<StorageLyroreVtab*>(pVtab);
    if (!vtab->op_instance) return SQLITE_OK;  // Entry cleaned up
    return vtab->op_instance->on_begin();
}

static int storage_vtab_sync(sqlite3_vtab* pVtab) {
    StorageLyroreVtab* vtab = static_cast<StorageLyroreVtab*>(pVtab);
    if (!vtab->op_instance) return SQLITE_OK;  // Entry cleaned up
    int rc = vtab->op_instance->on_sync();
    if (rc == SQLITE_OK && vtab->entry && vtab->entry->version_manager) {
        vtab->entry->version_manager->increment_version();
    }
    return rc;
}

static int storage_vtab_commit(sqlite3_vtab* pVtab) {
    StorageLyroreVtab* vtab = static_cast<StorageLyroreVtab*>(pVtab);
    if (!vtab->op_instance) return SQLITE_OK;  // Entry cleaned up
    vtab->op_instance->on_commit();
    if (vtab->entry && vtab->entry->version_manager) {
        vtab->entry->version_manager->commit_version();
    }
    return SQLITE_OK;
}

static int storage_vtab_rollback(sqlite3_vtab* pVtab) {
    StorageLyroreVtab* vtab = static_cast<StorageLyroreVtab*>(pVtab);
    if (!vtab->op_instance) return SQLITE_OK;  // Entry cleaned up
    vtab->op_instance->on_rollback();
    if (vtab->entry && vtab->entry->version_manager) {
        vtab->entry->version_manager->rollback_version();
    }
    return SQLITE_OK;
}

// ============================================================
// Storage Module Definition
// ============================================================

static sqlite3_module lyrore_storage_vtab_module = {
    0,                          // iVersion
    storage_vtab_connect,       // xCreate
    storage_vtab_connect,       // xConnect
    storage_vtab_best_index,    // xBestIndex
    storage_vtab_disconnect,    // xDisconnect
    storage_vtab_disconnect,    // xDestroy
    storage_vtab_open,          // xOpen
    storage_vtab_close,         // xClose
    storage_vtab_filter,        // xFilter
    storage_vtab_next,          // xNext
    storage_vtab_eof,           // xEof
    storage_vtab_column,        // xColumn
    storage_vtab_rowid,         // xRowid
    storage_vtab_update,        // xUpdate
    storage_vtab_begin,         // xBegin
    storage_vtab_sync,          // xSync
    storage_vtab_commit,        // xCommit
    storage_vtab_rollback,      // xRollback
    nullptr,                    // xFindFunction
    nullptr,                    // xRename
    nullptr,                    // xSavepoint
    nullptr,                    // xRelease
    nullptr,                    // xRollbackTo
    nullptr,                    // xShadowName
    nullptr                     // xIntegrity
};

// ============================================================
// Storage Registration Function
// ============================================================

void register_storage_vtab_wrapper(sqlite3* db, StorageOpEntry* entry) {
    if (!db || !entry || !entry->persistent_instance) return;

    // Register the module
    int rc = sqlite3_create_module_v2(
        db,
        entry->vtab_name.c_str(),
        &lyrore_storage_vtab_module,
        entry,              // pAux - stable pointer to entry
        nullptr             // xDestroy - registry handles cleanup
    );

    if (rc != SQLITE_OK) {
        return;
    }

    // Create the virtual table
    std::string create_sql = "CREATE VIRTUAL TABLE " + entry->vtab_name +
                             " USING " + entry->vtab_name;

    char* errmsg = nullptr;
    rc = sqlite3_exec(db, create_sql.c_str(), nullptr, nullptr, &errmsg);
    if (errmsg) {
        sqlite3_free(errmsg);
    }

    // Capture Table* after vtab creation
    Table* pTab = lyrore_sqlite3FindTable(db, entry->vtab_name.c_str(), "main");
    if (!pTab) {
        pTab = lyrore_sqlite3FindTable(db, entry->vtab_name.c_str(), nullptr);
    }
    if (pTab) {
        entry->pVtabTable = pTab;
    }

    // Initialize VersionManager with shadow table
    entry->version_manager = std::make_unique<VersionManager>(db, entry->vtab_name);
    entry->version_manager->init();

    // Verify consistency on reconnect (check for crash-interrupted transactions)
    int64_t db_version = entry->version_manager->get_db_version();
    int64_t plugin_version = entry->persistent_instance->get_committed_version();
    if (db_version > plugin_version) {
        // Crash detected - DB has a higher version than plugin's committed state
        entry->persistent_instance->verify_consistency(db_version);
    }
}


} // namespace lyrore
