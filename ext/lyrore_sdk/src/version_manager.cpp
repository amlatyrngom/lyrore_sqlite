/*
** Lyrore Version Manager
**
** Manages version tracking for StorageCustomOperator with shadow table persistence.
** Shadow table: <storage>_lyrore_version
** Schema: CREATE TABLE <storage>_lyrore_version(version INTEGER NOT NULL)
*/

#include "lyrore_custom_op.hpp"
#include <cstring>

extern "C" {
#include "sqlite3.h"
}

namespace lyrore {

// ============================================================
// VersionManager Implementation
// ============================================================

VersionManager::VersionManager(sqlite3* db, const std::string& storage_name)
    : db_(db)
    , table_name_(storage_name + "_lyrore_version")
    , current_version_(0)
    , committed_version_(0)
{
}

VersionManager::~VersionManager() = default;

void VersionManager::init() {
    if (!db_) return;

    // Create shadow table if not exists
    std::string create_sql = "CREATE TABLE IF NOT EXISTS " + table_name_ +
                             "(version INTEGER NOT NULL)";
    char* err = nullptr;
    int rc = sqlite3_exec(db_, create_sql.c_str(), nullptr, nullptr, &err);
    if (err) sqlite3_free(err);
    if (rc != SQLITE_OK) return;

    // Check if table has a row
    std::string count_sql = "SELECT COUNT(*) FROM " + table_name_;
    sqlite3_stmt* stmt = nullptr;
    rc = sqlite3_prepare_v2(db_, count_sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return;

    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);

    if (count == 0) {
        // Insert initial version
        std::string insert_sql = "INSERT INTO " + table_name_ + "(version) VALUES(0)";
        sqlite3_exec(db_, insert_sql.c_str(), nullptr, nullptr, nullptr);
    }

    // Read current version from DB
    current_version_ = get_db_version();
    committed_version_ = current_version_;
}

int64_t VersionManager::get_db_version() {
    if (!db_) return 0;

    std::string sql = "SELECT version FROM " + table_name_ + " LIMIT 1";
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return 0;

    int64_t version = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        version = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return version;
}

void VersionManager::increment_version() {
    // Increment pending version (will be written during sync)
    current_version_++;

    // Write to shadow table immediately (durability before commit)
    if (!db_) return;
    std::string sql = "UPDATE " + table_name_ + " SET version = " + 
                      std::to_string(current_version_);
    sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, nullptr);
}

void VersionManager::commit_version() {
    // Version is already persisted during increment_version/sync
    // Just update committed marker
    committed_version_ = current_version_;
}

void VersionManager::rollback_version() {
    // Restore previous version to DB
    if (!db_) return;
    std::string sql = "UPDATE " + table_name_ + " SET version = " + 
                      std::to_string(committed_version_);
    sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, nullptr);
    current_version_ = committed_version_;
}

bool VersionManager::verify_consistency(int64_t plugin_committed_version) {
    int64_t db_version = get_db_version();
    // If DB version > plugin's committed version, there was a crash after sync but before commit
    return db_version <= plugin_committed_version;
}

int64_t VersionManager::get_committed_version() const {
    return committed_version_;
}

int64_t VersionManager::get_current_version() const {
    return current_version_;
}

} // namespace lyrore
