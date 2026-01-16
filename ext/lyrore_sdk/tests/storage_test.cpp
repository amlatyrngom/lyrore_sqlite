/*
** Lyrore Storage Operator Test Suite - Step 4
** Tests: CRUD, Rollback, Crash Recovery, Performance, Replicated, Trigger
*/

#include "lyrore_plugin.hpp"
#include "lyrore_custom_op.hpp"
#include "lyrore_function.hpp"
#include <chrono>
#include <map>
#include <sstream>

extern "C" {
#include "sqlite3.h"
}

using namespace lyrore;

static std::ostringstream test_output;
static int g_passed = 0;
static int g_failed = 0;

static void test_pass(const char* name) { 
    test_output << "PASS: " << name << "\n"; 
    g_passed++; 
}
static void test_fail(const char* name, const char* reason) { 
    test_output << "FAIL: " << name << " - " << reason << "\n"; 
    g_failed++; 
}

// ============================================================
// ColumnarStorage with direct aggregation support
// ============================================================

class ColumnarStorage : public StorageCustomOperator {
public:
    std::vector<int64_t> col_id_, col_cat_, col_qty_, col_rowid_;
    std::vector<int64_t> committed_id_, committed_cat_, committed_qty_, committed_rowid_;
    std::map<int64_t, size_t> rowid_to_index_;
    size_t pos_ = 0;
    int64_t next_rowid_ = 1;
    int64_t committed_version_ = 0, current_version_ = 0;

    StorageMode storage_mode() const override { return StorageMode::OWNED; }
    OutputMode output_mode() const override { return OutputMode::ITERATOR; }

    int on_update(UpdateOp op, int64_t old_rowid, int64_t new_rowid,
                  const std::vector<LyValue>&, const std::vector<LyValue>& new_vals) override {
        switch (op) {
            case UpdateOp::INSERT: {
                int64_t rowid = (new_rowid > 0) ? new_rowid : next_rowid_++;
                if (rowid >= next_rowid_) next_rowid_ = rowid + 1;
                int64_t id = new_vals.size() > 0 ? std::get<int64_t>(new_vals[0]) : 0;
                int64_t cat = new_vals.size() > 1 ? std::get<int64_t>(new_vals[1]) : 0;
                int64_t qty = new_vals.size() > 2 ? std::get<int64_t>(new_vals[2]) : 0;
                size_t idx = col_id_.size();
                col_id_.push_back(id); col_cat_.push_back(cat); 
                col_qty_.push_back(qty); col_rowid_.push_back(rowid);
                rowid_to_index_[rowid] = idx;
                break;
            }
            case UpdateOp::UPDATE: {
                auto it = rowid_to_index_.find(old_rowid);
                if (it == rowid_to_index_.end()) return SQLITE_ERROR;
                size_t idx = it->second;
                if (new_vals.size() > 0) col_id_[idx] = std::get<int64_t>(new_vals[0]);
                if (new_vals.size() > 1) col_cat_[idx] = std::get<int64_t>(new_vals[1]);
                if (new_vals.size() > 2) col_qty_[idx] = std::get<int64_t>(new_vals[2]);
                if (new_rowid != old_rowid) {
                    rowid_to_index_.erase(old_rowid);
                    rowid_to_index_[new_rowid] = idx;
                    col_rowid_[idx] = new_rowid;
                }
                break;
            }
            case UpdateOp::DELETE: {
                auto it = rowid_to_index_.find(old_rowid);
                if (it == rowid_to_index_.end()) return SQLITE_ERROR;
                col_rowid_[it->second] = -1;
                rowid_to_index_.erase(old_rowid);
                break;
            }
        }
        return SQLITE_OK;
    }

    int on_begin() override {
        committed_id_ = col_id_; committed_cat_ = col_cat_;
        committed_qty_ = col_qty_; committed_rowid_ = col_rowid_;
        return SQLITE_OK;
    }
    int on_sync() override { current_version_++; return SQLITE_OK; }
    void on_commit() override {
        committed_id_ = col_id_; committed_cat_ = col_cat_;
        committed_qty_ = col_qty_; committed_rowid_ = col_rowid_;
        committed_version_ = current_version_;
    }
    void on_rollback() override {
        col_id_ = committed_id_; col_cat_ = committed_cat_;
        col_qty_ = committed_qty_; col_rowid_ = committed_rowid_;
        current_version_ = committed_version_;
        rowid_to_index_.clear();
        for (size_t i = 0; i < col_rowid_.size(); i++)
            if (col_rowid_[i] >= 0) rowid_to_index_[col_rowid_[i]] = i;
    }

    void iterator_reset() override { pos_ = 0; }
    bool iterator_next() override {
        while (pos_ < col_rowid_.size() && col_rowid_[pos_] < 0) pos_++;
        if (pos_ >= col_rowid_.size()) return false;
        pos_++; return true;
    }
    std::vector<LyValue> iterator_get_row() override {
        size_t idx = pos_ - 1;
        return {LyValue{col_id_[idx]}, LyValue{col_cat_[idx]}, LyValue{col_qty_[idx]}};
    }
    int64_t get_current_rowid() const override {
        if (pos_ == 0 || pos_ > col_rowid_.size()) return 0;
        return col_rowid_[pos_ - 1];
    }
    int64_t get_committed_version() const override { return committed_version_; }
    void set_version(int64_t v) override { current_version_ = v; committed_version_ = v; }
    
    // Direct columnar aggregation - O(n) single pass
    std::map<int64_t, int64_t> sum_by_category() const {
        std::map<int64_t, int64_t> sums;
        for (size_t i = 0; i < col_rowid_.size(); i++) {
            if (col_rowid_[i] >= 0) sums[col_cat_[i]] += col_qty_[i];
        }
        return sums;
    }
};

// ============================================================
// Test 1: Basic CRUD
// ============================================================
static void test_basic_crud(sqlite3* db) {
    const char* name = "Test 1: Basic CRUD operations";
    
    register_storage_op<ColumnarStorage>(db, "t1", {"id", "cat", "qty"});
    
    char* err = nullptr;
    int rc = sqlite3_exec(db, "INSERT INTO t1 VALUES(1,10,100)", nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        test_fail(name, err ? err : "INSERT failed");
        if (err) sqlite3_free(err);
        return;
    }
    
    rc = sqlite3_exec(db, "INSERT INTO t1 VALUES(2,20,200)", nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        test_fail(name, err ? err : "INSERT failed");
        if (err) sqlite3_free(err);
        return;
    }
    
    // Verify SELECT
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, "SELECT * FROM t1 ORDER BY id", -1, &stmt, nullptr);
    
    int count = 0;
    bool correct = true;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int64_t id = sqlite3_column_int64(stmt, 0);
        int64_t cat = sqlite3_column_int64(stmt, 1);
        int64_t qty = sqlite3_column_int64(stmt, 2);
        if (count == 0 && (id != 1 || cat != 10 || qty != 100)) correct = false;
        if (count == 1 && (id != 2 || cat != 20 || qty != 200)) correct = false;
        count++;
    }
    sqlite3_finalize(stmt);
    
    if (count != 2 || !correct) {
        test_fail(name, "SELECT returned wrong data");
        return;
    }
    
    // Test UPDATE
    rc = sqlite3_exec(db, "UPDATE t1 SET qty=150 WHERE id=1", nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        test_fail(name, err ? err : "UPDATE failed");
        if (err) sqlite3_free(err);
        return;
    }
    
    sqlite3_prepare_v2(db, "SELECT qty FROM t1 WHERE id=1", -1, &stmt, nullptr);
    int64_t updated_qty = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        updated_qty = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    
    if (updated_qty != 150) {
        test_fail(name, "UPDATE didn't change value");
        return;
    }
    
    // Test DELETE
    rc = sqlite3_exec(db, "DELETE FROM t1 WHERE id=2", nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        test_fail(name, err ? err : "DELETE failed");
        if (err) sqlite3_free(err);
        return;
    }
    
    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM t1", -1, &stmt, nullptr);
    int64_t remaining = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        remaining = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    
    if (remaining != 1) {
        test_fail(name, "DELETE didn't remove row");
        return;
    }
    
    test_pass(name);
}

// ============================================================
// Test 2: Transaction Rollback
// ============================================================
static void test_transaction_rollback(sqlite3* db) {
    const char* name = "Test 2: Transaction Rollback";
    
    register_storage_op<ColumnarStorage>(db, "t2", {"id", "cat", "qty"});
    
    sqlite3_exec(db, "INSERT INTO t2 VALUES(1,10,100)", nullptr, nullptr, nullptr);
    
    sqlite3_exec(db, "BEGIN", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "INSERT INTO t2 VALUES(2,20,200)", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "UPDATE t2 SET qty=999 WHERE id=1", nullptr, nullptr, nullptr);
    
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM t2", -1, &stmt, nullptr);
    int64_t before_rollback = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) before_rollback = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    
    sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
    
    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM t2", -1, &stmt, nullptr);
    int64_t after_rollback = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) after_rollback = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    
    sqlite3_prepare_v2(db, "SELECT qty FROM t2 WHERE id=1", -1, &stmt, nullptr);
    int64_t qty = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) qty = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    
    if (after_rollback == 1 && qty == 100) {
        test_pass(name);
    } else {
        char buf[128];
        snprintf(buf, sizeof(buf), "count=%ld qty=%ld (expected 1, 100)", after_rollback, qty);
        test_fail(name, buf);
    }
}

// ============================================================
// Test 3: Crash Recovery Detection (Uses file-based DB)
// ============================================================

// Global to track crash detection
static bool g_crash_detected = false;

// CrashTestStorage - tracks crash detection callback
class CrashTestStorage : public StorageCustomOperator {
public:
    std::vector<int64_t> col_id_, col_rowid_;
    std::map<int64_t, size_t> rowid_to_index_;
    size_t pos_ = 0;
    int64_t next_rowid_ = 1;

    StorageMode storage_mode() const override { return StorageMode::OWNED; }
    OutputMode output_mode() const override { return OutputMode::ITERATOR; }

    int on_update(UpdateOp op, int64_t old_rowid, int64_t new_rowid,
                  const std::vector<LyValue>&, const std::vector<LyValue>& new_vals) override {
        if (op == UpdateOp::INSERT) {
            int64_t rowid = (new_rowid > 0) ? new_rowid : next_rowid_++;
            if (rowid >= next_rowid_) next_rowid_ = rowid + 1;
            int64_t id = new_vals.size() > 0 ? std::get<int64_t>(new_vals[0]) : 0;
            size_t idx = col_id_.size();
            col_id_.push_back(id);
            col_rowid_.push_back(rowid);
            rowid_to_index_[rowid] = idx;
        }
        return SQLITE_OK;
    }

    void iterator_reset() override { pos_ = 0; }
    bool iterator_next() override {
        while (pos_ < col_rowid_.size() && col_rowid_[pos_] < 0) pos_++;
        if (pos_ >= col_rowid_.size()) return false;
        pos_++; return true;
    }
    std::vector<LyValue> iterator_get_row() override {
        return {LyValue{col_id_[pos_-1]}};
    }
    int64_t get_current_rowid() const override {
        if (pos_ == 0 || pos_ > col_rowid_.size()) return 0;
        return col_rowid_[pos_-1];
    }

    // ACID Version Tracking - this returns 0 for new instance (simulating crash)
    int64_t get_committed_version() const override { return 0; }
    void verify_consistency(int64_t db_version) override { 
        // This is called when db_version > committed_version
        // Indicates a crash occurred after sync but before commit
        if (db_version > 0) {
            g_crash_detected = true;
        }
    }
};

static void test_crash_recovery(sqlite3* db) {
    const char* name = "Test 3: Crash Recovery Detection";
    (void)db;  // We don't use the passed db for this test
    
    g_crash_detected = false;
    
    // Use a temp file for this test
    const char* test_db_path = "/tmp/lyrore_crash_test.db";
    
    // Remove old test db if exists
    remove(test_db_path);
    
    // Phase 1: Create DB, insert data, commit (version 1), then sync without commit (version 2)
    {
        sqlite3* db1 = nullptr;
        int rc = sqlite3_open(test_db_path, &db1);
        if (rc != SQLITE_OK) {
            test_fail(name, "Failed to open test database");
            return;
        }
        
        // Enable Lyrore
        sqlite3_exec(db1, "PRAGMA lyrore_enabled=ON", nullptr, nullptr, nullptr);
        sqlite3_exec(db1, "PRAGMA lyrore_plugins=ON", nullptr, nullptr, nullptr);
        
        // Register storage operator (creates shadow table with version 0)
        register_storage_op<CrashTestStorage>(db1, "crash_test", {"id"});
        
        // Insert and commit - version becomes 1
        char* err = nullptr;
        sqlite3_exec(db1, "INSERT INTO crash_test VALUES(1)", nullptr, nullptr, &err);
        if (err) { sqlite3_free(err); err = nullptr; }
        sqlite3_exec(db1, "COMMIT", nullptr, nullptr, &err);
        if (err) { sqlite3_free(err); err = nullptr; }
        
        // Start new transaction, insert, but don't commit
        // The sync happens automatically on next statement that modifies
        sqlite3_exec(db1, "BEGIN", nullptr, nullptr, &err);
        if (err) { sqlite3_free(err); err = nullptr; }
        sqlite3_exec(db1, "INSERT INTO crash_test VALUES(2)", nullptr, nullptr, &err);
        if (err) { sqlite3_free(err); err = nullptr; }
        
        // Manually trigger sync by preparing to commit (xSync is called)
        // Actually in SQLite, xSync is called as part of COMMIT processing
        // For this test, we directly manipulate the shadow table to simulate version 2 being written
        // during sync but crash before commit
        sqlite3_exec(db1, "UPDATE crash_test_lyrore_version SET version = 2", nullptr, nullptr, &err);
        if (err) { sqlite3_free(err); err = nullptr; }
        
        // Simulate crash - close without COMMIT

        sqlite3_close(db1);
        CustomOpRegistry::cleanup(db1);  // Clean up registry after close
    }
    
    // Phase 2: Reopen DB and check crash detection
    {
        sqlite3* db2 = nullptr;
        int rc = sqlite3_open(test_db_path, &db2);
        if (rc != SQLITE_OK) {
            test_fail(name, "Failed to reopen test database");
            return;
        }
        
        // Enable Lyrore
        sqlite3_exec(db2, "PRAGMA lyrore_enabled=ON", nullptr, nullptr, nullptr);
        sqlite3_exec(db2, "PRAGMA lyrore_plugins=ON", nullptr, nullptr, nullptr);
        
        // Register storage operator - this should detect crash via verify_consistency
        // because db_version (2) > plugin_committed_version (0)
        register_storage_op<CrashTestStorage>(db2, "crash_test", {"id"});
        

        sqlite3_close(db2);
        CustomOpRegistry::cleanup(db2);  // Clean up registry after close
    }
    
    // Clean up
    remove(test_db_path);
    
    if (g_crash_detected) {
        test_pass(name);
    } else {
        test_fail(name, "Crash detection not triggered");
    }
}

// ============================================================
// Test 4: Performance (GROUP BY Speedup)
// ============================================================

// Global pointer for fast aggregation access  
static ColumnarStorage* g_perf_storage = nullptr;

// FastColumnarGroupBy - Custom operator bypassing SQLite query engine
class FastColumnarGroupBy : public CustomOperator {
    std::vector<std::pair<int64_t, int64_t>> results_;
    size_t pos_ = 0;
    
public:
    OutputMode output_mode() const override { return OutputMode::ITERATOR; }
    
    void execute() override {
        results_.clear();
        if (g_perf_storage) {
            auto sums = g_perf_storage->sum_by_category();
            for (auto& p : sums) results_.push_back(p);
        }
    }
    
    void iterator_reset() override { pos_ = 0; }
    bool iterator_next() override { return ++pos_ <= results_.size(); }
    std::vector<LyValue> iterator_get_row() override {
        if (pos_ == 0 || pos_ > results_.size()) return {};
        return {LyValue{results_[pos_-1].first}, LyValue{results_[pos_-1].second}};
    }
};

static void test_performance(sqlite3* db) {
    const char* name = "Test 4: Performance (GROUP BY Speedup)";
    
    // Create native table with 100K rows
    sqlite3_exec(db, "CREATE TABLE native(id INT, cat INT, qty INT)", nullptr, nullptr, nullptr);
    sqlite3_exec(db, 
        "WITH RECURSIVE cnt(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM cnt WHERE x<100000) "
        "INSERT INTO native SELECT x, x%100, x*10 FROM cnt",
        nullptr, nullptr, nullptr);
    
    // Create columnar storage and populate
    register_storage_op<ColumnarStorage>(db, "col", {"id", "cat", "qty"});
    sqlite3_exec(db, "INSERT INTO col SELECT * FROM native", nullptr, nullptr, nullptr);
    
    // Get pointer to the storage instance for direct aggregation
    auto& registry = CustomOpRegistry::for_db(db);
    const auto& entries = registry.get_entries();
    for (const auto& entry : entries) {
        if (entry && entry->vtab_name == "col") {
            auto* storage_entry = dynamic_cast<StorageOpEntry*>(entry.get());
            if (storage_entry && storage_entry->persistent_instance) {
                g_perf_storage = dynamic_cast<ColumnarStorage*>(storage_entry->persistent_instance.get());
            }
            break;
        }
    }
    
    if (!g_perf_storage) {
        test_fail(name, "Failed to get storage pointer");
        return;
    }
    
    // Register custom operator for fast GROUP BY
    register_custom_op<FastColumnarGroupBy>(db, 
        "SELECT cat, SUM(qty) FROM col GROUP BY cat", 
        {"cat", "total"});
    
    // Warm up
    sqlite3_exec(db, "SELECT cat, SUM(qty) FROM native GROUP BY cat", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "SELECT cat, SUM(qty) FROM col GROUP BY cat", nullptr, nullptr, nullptr);
    
    // Benchmark native SQLite GROUP BY
    auto t1 = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < 3; i++) {
        sqlite3_exec(db, "SELECT cat, SUM(qty) FROM native GROUP BY cat", nullptr, nullptr, nullptr);
    }
    auto t2 = std::chrono::high_resolution_clock::now();
    double native_ms = std::chrono::duration<double, std::milli>(t2 - t1).count() / 3.0;
    
    // Benchmark columnar with custom operator (should match pattern and use direct aggregation)
    t1 = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < 3; i++) {
        sqlite3_exec(db, "SELECT cat, SUM(qty) FROM col GROUP BY cat", nullptr, nullptr, nullptr);
    }
    t2 = std::chrono::high_resolution_clock::now();
    double col_ms = std::chrono::duration<double, std::milli>(t2 - t1).count() / 3.0;
    
    double speedup = native_ms / col_ms;
    test_output << "  Native: " << native_ms << " ms, Columnar: " << col_ms << " ms, Speedup: " << speedup << "x\n";
    
    // Verify correctness
    std::map<int64_t, int64_t> native_results, col_results;
    sqlite3_stmt* stmt;
    
    sqlite3_prepare_v2(db, "SELECT cat, SUM(qty) FROM native GROUP BY cat ORDER BY cat", -1, &stmt, nullptr);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        native_results[sqlite3_column_int64(stmt, 0)] = sqlite3_column_int64(stmt, 1);
    }
    sqlite3_finalize(stmt);
    
    sqlite3_prepare_v2(db, "SELECT cat, SUM(qty) FROM col GROUP BY cat ORDER BY cat", -1, &stmt, nullptr);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        col_results[sqlite3_column_int64(stmt, 0)] = sqlite3_column_int64(stmt, 1);
    }
    sqlite3_finalize(stmt);
    
    if (native_results != col_results) {
        test_fail(name, "Results differ");
        return;
    }
    
    if (speedup >= 2.0) {
        test_pass(name);
    } else {
        char buf[128];
        snprintf(buf, sizeof(buf), "Speedup %.2fx < 2x required", speedup);
        test_fail(name, buf);
    }
}

// ============================================================
// Test 5: Replicated Mode Sync
// ============================================================

class TestReplica : public StorageCustomOperator {
    std::vector<int64_t> col_a_, col_b_, col_rowid_;
    std::map<int64_t, size_t> rowid_to_index_;
    size_t pos_ = 0;
    int64_t next_rowid_ = 1;
public:
    StorageMode storage_mode() const override { return StorageMode::REPLICATED; }
    OutputMode output_mode() const override { return OutputMode::ITERATOR; }
    std::string source_table() const override { return "src"; }

    int on_update(UpdateOp op, int64_t old_rowid, int64_t new_rowid,
                  const std::vector<LyValue>&, const std::vector<LyValue>& new_vals) override {
        switch (op) {
            case UpdateOp::INSERT: {
                int64_t rowid = (new_rowid > 0) ? new_rowid : next_rowid_++;
                if (rowid >= next_rowid_) next_rowid_ = rowid + 1;
                int64_t a = new_vals.size() > 0 ? std::get<int64_t>(new_vals[0]) : 0;
                int64_t b = new_vals.size() > 1 ? std::get<int64_t>(new_vals[1]) : 0;
                size_t idx = col_a_.size();
                col_a_.push_back(a); col_b_.push_back(b); col_rowid_.push_back(rowid);
                rowid_to_index_[rowid] = idx;
                break;
            }
            case UpdateOp::UPDATE: {
                auto it = rowid_to_index_.find(old_rowid);
                if (it == rowid_to_index_.end()) return SQLITE_ERROR;
                size_t idx = it->second;
                if (new_vals.size() > 0) col_a_[idx] = std::get<int64_t>(new_vals[0]);
                if (new_vals.size() > 1) col_b_[idx] = std::get<int64_t>(new_vals[1]);
                break;
            }
            case UpdateOp::DELETE: {
                auto it = rowid_to_index_.find(old_rowid);
                if (it == rowid_to_index_.end()) return SQLITE_ERROR;
                col_rowid_[it->second] = -1;
                rowid_to_index_.erase(old_rowid);
                break;
            }
        }
        return SQLITE_OK;
    }

    void iterator_reset() override { pos_ = 0; }
    bool iterator_next() override {
        while (pos_ < col_rowid_.size() && col_rowid_[pos_] < 0) pos_++;
        if (pos_ >= col_rowid_.size()) return false;
        pos_++; return true;
    }
    std::vector<LyValue> iterator_get_row() override {
        size_t idx = pos_ - 1;
        return {LyValue{col_a_[idx]}, LyValue{col_b_[idx]}};
    }
    int64_t get_current_rowid() const override {
        if (pos_ == 0 || pos_ > col_rowid_.size()) return 0;
        return col_rowid_[pos_ - 1];
    }
};

static void test_replicated_sync(sqlite3* db) {
    const char* name = "Test 5: Replicated Mode Sync";
    
    sqlite3_exec(db, "CREATE TABLE src(a INT, b INT)", nullptr, nullptr, nullptr);
    register_storage_op<TestReplica>(db, "rep", {"a", "b"});
    
    sqlite3_exec(db, "INSERT INTO src VALUES(1,100)", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "INSERT INTO src VALUES(2,200)", nullptr, nullptr, nullptr);
    
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, "SELECT * FROM rep ORDER BY a", -1, &stmt, nullptr);
    
    int count = 0;
    bool correct = true;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int64_t a = sqlite3_column_int64(stmt, 0);
        int64_t b = sqlite3_column_int64(stmt, 1);
        if (count == 0 && (a != 1 || b != 100)) correct = false;
        if (count == 1 && (a != 2 || b != 200)) correct = false;
        count++;
    }
    sqlite3_finalize(stmt);
    
    if (count == 2 && correct) {
        test_pass(name);
    } else {
        char buf[64];
        snprintf(buf, sizeof(buf), "Got %d rows (expected 2)", count);
        test_fail(name, buf);
    }
}

// ============================================================
// Test 6: Trigger Function
// ============================================================

static int g_trigger_count = 0;

static void test_trigger_function(sqlite3* db) {
    const char* name = "Test 6: Trigger Function";
    
    g_trigger_count = 0;
    
    std::function<int64_t(int64_t)> audit_fn = [](int64_t x) -> int64_t { 
        g_trigger_count++; 
        return x; 
    };
    register_scalar_function<int64_t, int64_t>(db, "audit_fn", audit_fn, false, true);
    
    sqlite3_exec(db, "CREATE TABLE d(id INT)", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "CREATE TRIGGER t AFTER INSERT ON d BEGIN SELECT audit_fn(NEW.id); END", nullptr, nullptr, nullptr);
    
    sqlite3_exec(db, "INSERT INTO d VALUES(1)", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "INSERT INTO d VALUES(2)", nullptr, nullptr, nullptr);
    
    if (g_trigger_count == 2) {
        test_pass(name);
    } else {
        char buf[64];
        snprintf(buf, sizeof(buf), "Expected 2 calls, got %d", g_trigger_count);
        test_fail(name, buf);
    }
}

// ============================================================
// Plugin Class
// ============================================================

class StorageTestPlugin : public Plugin {
public:
    std::string name() const override { return "storage_test"; }
    
    void onInit(sqlite3* db) override {
        sqlite3_create_function_v2(db, "run_storage_tests", 0, SQLITE_UTF8, db,
            [](sqlite3_context* ctx, int, sqlite3_value**) {
                sqlite3* db = static_cast<sqlite3*>(sqlite3_user_data(ctx));
                
                test_output.str("");
                test_output.clear();
                g_passed = 0;
                g_failed = 0;
                g_perf_storage = nullptr;
                
                test_output << "=== Storage Operator Test Suite ===\n";
                
                test_basic_crud(db);
                test_transaction_rollback(db);
                test_crash_recovery(db);
                test_performance(db);
                test_replicated_sync(db);
                test_trigger_function(db);
                
                test_output << "=== Results: " << g_passed << " passed, " << g_failed << " failed ===\n";
                
                std::string output = test_output.str();
                printf("%s", output.c_str());
                
                if (g_failed == 0) {
                    sqlite3_result_text(ctx, "ALL TESTS PASSED", -1, SQLITE_STATIC);
                } else {
                    sqlite3_result_text(ctx, "SOME TESTS FAILED", -1, SQLITE_STATIC);
                }
            },
            nullptr, nullptr, nullptr);
    }
};

LYRORE_REGISTER_PLUGIN(StorageTestPlugin);
