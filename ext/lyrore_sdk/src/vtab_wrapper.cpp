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

} // namespace lyrore
