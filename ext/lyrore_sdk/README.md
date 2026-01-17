# Lyrore C++ Plugin SDK

A C++ SDK for SQLite query optimization plugins. Build plugins that transform queries, adjust cost estimates, collect statistics, replace query subtrees with custom computation, and implement custom storage backends.

## Quick Start

```bash
# Build SQLite with Lyrore (one-time)
mkdir -p ~/sqlite_build && cd ~/sqlite_build
~/sqlite/configure
make -j4 "OPTS=-DSQLITE_ENABLE_LYRORE=1 -DSQLITE_ENABLE_STMT_SCANSTATUS=1 -DSQLITE_ENABLE_PREUPDATE_HOOK=1" "LDFLAGS=-rdynamic"

# Build plugins
cd ~/sqlite/ext/lyrore_sdk && make
```

## Loading Plugins (LyroreMain API)

Plugins are loaded through the centralized LyroreMain singleton. This provides:
- Plugin hot-reload without restart
- Transaction-aware versioning
- Shared resource registry across plugins
- Comprehensive post-query statistics

```cpp
#include "lyrore_main.hpp"

int main() {
    auto main = lyrore::LyroreMain::instance();
    sqlite3* db = main->get_db("mydb", ":memory:");

    // Load a plugin
    main->reload_plugin("mydb", "my_plugin", "./my_plugin.so");

    // Use db as normal sqlite3*...
    // Plugin hooks are automatically invoked during query processing.

    main->shutdown();
    return 0;
}
```

## Plugin Hooks

| Hook | Purpose |
|------|---------|
| `onInit` | Initialize patterns, register custom operators |
| `onPreParse` | Transform SQL before parsing (custom dialects) |
| `onPreOpt` | Transform AST before optimization |
| `onEstimate` | Override cardinality estimates |
| `onAnalyze` | Build models during ANALYZE |
| `onPostQuery` | Collect execution stats |

---

## Pattern API

Match SQL queries using SQLite's own parser. No manual AST traversal needed.

```cpp
#include "lyrore_plugin.hpp"
#include "lyrore_pattern.hpp"

class MyPlugin : public lyrore::Plugin {
    lyrore::pattern::PatternPtr pattern_;

public:
    std::string name() const override { return "my_plugin"; }

    void onInit(sqlite3* db) override {
        // '?' marks parameters to extract
        pattern_ = lyrore::pattern::Pattern::from_query(db,
            "SELECT * FROM orders WHERE customer_id = ? AND amount > ?");
    }

    void onPreOpt(lyrore::PreOptContext& ctx) override {
        if (auto m = pattern_->match(ctx.select_raw())) {
            int64_t cust_id = m.get<int64_t>("$1");
            double min_amt = m.get<double>("$2");
            // Use for optimization...
        }
    }
};
LYRORE_REGISTER_PLUGIN(MyPlugin);
```

### Pattern Methods

```cpp
// Full query pattern
auto p1 = Pattern::from_query(db, "SELECT SUM(x) FROM t WHERE a = ?");

// WHERE expression only
auto p2 = Pattern::from_where_expr(db, "SELECT 1 FROM t WHERE (price * qty) < ?");

// Match and extract
if (auto m = pattern->match(select)) {
    int64_t val1 = m.get<int64_t>("$1");  // By position
    double val2 = m.get<double>("$2");
}
```

---

## Custom Operators

Replace query subtrees with custom computation. Three output modes:

| Mode | Use Case | Returns |
|------|----------|---------|
| `SCALAR` | Single value (SUM, COUNT) | `LyValue` |
| `ITERATOR` | Multi-row result (GROUP BY) | Row iterator |
| `CURSOR` | Raw access (advanced) | Cursor handle |

### SCALAR Mode Example

```cpp
#include "lyrore_custom_op.hpp"

class FastSumOp : public lyrore::CustomOperator {
public:
    lyrore::OutputMode output_mode() const override { 
        return lyrore::OutputMode::SCALAR; 
    }

    lyrore::LyValue compute_scalar() override {
        int64_t cat = params_->get<int64_t>("$1");
        int64_t sum = 0;
        auto cursor = open_table("orders");
        while (!cursor.eof()) {
            if (cursor.get_column(1).get<int64_t>() == cat) {
                sum += cursor.get_column(2).get<int64_t>();
            }
            cursor.next();
        }
        return lyrore::LyValue{sum};
    }
};

// Register in onInit:
lyrore::register_custom_op<FastSumOp>(db,
    "SELECT SUM(qty) FROM orders WHERE category_id = ?",
    {"sum"});
```

### ITERATOR Mode Example

```cpp
class FastGroupByOp : public lyrore::CustomOperator {
    std::array<int64_t, 5> sums_{};
    size_t pos_ = 0;

public:
    lyrore::OutputMode output_mode() const override { 
        return lyrore::OutputMode::ITERATOR; 
    }

    void execute() override {
        sums_.fill(0);
        auto cursor = open_table("orders");
        while (!cursor.eof()) {
            int64_t cat = cursor.get_column(1).get<int64_t>();
            int64_t qty = cursor.get_column(2).get<int64_t>();
            if (cat >= 0 && cat < 5) sums_[cat] += qty;
            cursor.next();
        }
    }

    void iterator_reset() override { pos_ = 0; }
    bool iterator_next() override { return ++pos_ <= 5; }
    std::vector<lyrore::LyValue> iterator_get_row() override {
        return {lyrore::LyValue{(int64_t)(pos_-1)}, lyrore::LyValue{sums_[pos_-1]}};
    }
};

// Register in onInit:
lyrore::register_custom_op<FastGroupByOp>(db,
    "SELECT category_id, SUM(qty) FROM orders GROUP BY category_id",
    {"category_id", "total"});
```

---

## Storage Customization

Implement custom storage backends with full INSERT/UPDATE/DELETE support and ACID transactions.

| Mode | Data Authority | Write Path | Use Case |
|------|---------------|------------|----------|
| `OWNED` | Plugin is source | xUpdate → on_update() | Columnar storage, custom indexes |
| `REPLICATED` | Original table | preupdate_hook → on_update() | Materialized views, caches |

### Storage Operator Example

```cpp
#include "lyrore_custom_op.hpp"

class ColumnarStorage : public lyrore::StorageCustomOperator {
    std::vector<int64_t> col_id_, col_cat_, col_qty_;
    size_t pos_ = 0;

public:
    lyrore::StorageMode storage_mode() const override { return lyrore::StorageMode::OWNED; }
    lyrore::OutputMode output_mode() const override { return lyrore::OutputMode::ITERATOR; }

    int on_update(lyrore::UpdateOp op, int64_t old_rowid, int64_t new_rowid,
                  const std::vector<lyrore::LyValue>&,
                  const std::vector<lyrore::LyValue>& new_vals) override {
        if (op == lyrore::UpdateOp::INSERT) {
            col_id_.push_back(std::get<int64_t>(new_vals[0]));
            col_cat_.push_back(std::get<int64_t>(new_vals[1]));
            col_qty_.push_back(std::get<int64_t>(new_vals[2]));
        }
        return SQLITE_OK;
    }

    // Transaction hooks for ACID compliance
    int on_sync() override { /* ensure durability */ return SQLITE_OK; }
    void on_commit() override { /* finalize changes */ }
    void on_rollback() override { /* revert changes */ }

    // Iterator for reads
    void iterator_reset() override { pos_ = 0; }
    bool iterator_next() override { return ++pos_ <= col_id_.size(); }
    std::vector<lyrore::LyValue> iterator_get_row() override {
        return {lyrore::LyValue{col_id_[pos_-1]}, lyrore::LyValue{col_cat_[pos_-1]}, 
                lyrore::LyValue{col_qty_[pos_-1]}};
    }
};

// Register in onInit:
lyrore::register_storage_op<ColumnarStorage>(db, "orders_col", {"id", "cat", "qty"});
```

### Transaction Hooks

| Hook | Called | Purpose |
|------|--------|---------|
| `on_begin()` | Transaction start | Snapshot state |
| `on_update()` | Each INSERT/UPDATE/DELETE | Apply changes |
| `on_sync()` | Before commit | **Must ensure durability** |
| `on_commit()` | Commit success | Finalize |
| `on_rollback()` | Rollback | Revert to snapshot |

### Function Registration for Triggers

Register C++ functions with INNOCUOUS flag for safe use in triggers:

```cpp
#include "lyrore_function.hpp"

int count = 0;
lyrore::register_scalar_function<int64_t, int64_t>(db, "audit",
    [&](int64_t id) { count++; return id; }, false, true);

// SQL: CREATE TRIGGER t AFTER INSERT ON tbl BEGIN SELECT audit(NEW.id); END;
```

---

## Low-Level AST Manipulation

For direct AST transformation, use `lyrore_cabi.h` wrappers:

```cpp
#include "lyrore_cabi.h"

void onPreOpt(PreOptContext& ctx) {
    Expr* pWhere = ctx.where_raw();
    sqlite3* db = ctx.db();

    // Duplicate (preserves Table* bindings)
    Expr* pDup = lyrore_sqlite3ExprDup(db, expr, 0);

    // Create expressions
    Expr* pMul = lyrore_sqlite3Expr(db, TK_STAR, nullptr);
    pMul->pLeft = lyrore_sqlite3ExprDup(db, col1, 0);
    pMul->pRight = lyrore_sqlite3ExprDup(db, col2, 0);

    // In-place substitution
    lyrore_sqlite3ExprDelete(db, pWhere->pLeft);
    pWhere->op = pMul->op;
    pWhere->pLeft = pMul->pLeft;
    pWhere->pRight = pMul->pRight;
    pMul->pLeft = pMul->pRight = nullptr;
    lyrore_sqlite3DbFree(db, pMul);

    ctx.set_modified();
}
```

### C API Wrappers

| Function | Purpose |
|----------|---------|
| `lyrore_sqlite3Expr(db, op, token)` | Create expression |
| `lyrore_sqlite3ExprDup(db, expr, flags)` | Duplicate (preserves bindings) |
| `lyrore_sqlite3ExprDelete(db, expr)` | Free expression tree |
| `lyrore_sqlite3StrICmp(s1, s2)` | Case-insensitive compare |
| `lyrore_sqlite3DbFree(db, ptr)` | Free memory |

---

## Files

```
ext/lyrore_sdk/
├── include/
│   ├── lyrore_main.hpp       # LyroreMain singleton (entry point)
│   ├── lyrore_plugin.hpp     # Plugin base, contexts, LyValue
│   ├── lyrore_pattern.hpp    # Pattern matching API
│   ├── lyrore_custom_op.hpp  # Custom operators + Storage API
│   └── lyrore_function.hpp   # C++ function registration
├── src/                      # SDK implementation
├── examples/
│   ├── histogram.cpp         # Cardinality estimation
│   ├── udf_transform.cpp     # AST transformation
│   └── fast_groupby.cpp      # Custom GROUP BY operator
└── tests/
    ├── lyrore_main_test.cpp  # LyroreMain integration tests (8 tests)
    ├── custom_op_test.cpp    # Custom operator tests
    └── storage_test.cpp      # Storage operator tests
```

## Examples

- **histogram.cpp** - Pattern-based histogram cardinality estimation
- **udf_transform.cpp** - UDF to native expression transformation (30x+ speedup)
- **fast_groupby.cpp** - Custom GROUP BY using ITERATOR mode
- **storage_test.cpp** - Columnar storage with 30x+ GROUP BY speedup

---

## Build & Test

### Complete Build from Scratch

Follow these steps to build everything from a clean state:

```bash
# ============================================
# STEP 1: Clean everything
# ============================================
rm -f ~/sqlite_build/libsqlite3.so ~/sqlite_build/sqlite3
rm -f ~/sqlite/ext/lyrore_sdk/*.so ~/sqlite/ext/lyrore_sdk/build/*

# ============================================
# STEP 2: Build SQLite with Lyrore
# ============================================
cd ~/sqlite_build
make -j4 "OPTS=-DSQLITE_ENABLE_LYRORE=1 -DSQLITE_ENABLE_STMT_SCANSTATUS=1 -DSQLITE_ENABLE_PREUPDATE_HOOK=1" "LDFLAGS=-rdynamic"

# ============================================
# STEP 3: Build SDK plugins
# ============================================
cd ~/sqlite/ext/lyrore_sdk
make clean && make

# ============================================
# STEP 4: Run tests
# ============================================
cd ~/sqlite/ext/lyrore_sdk
./build/lyrore_main_test
```

### One-Liner for Full Rebuild & Test

```bash
cd ~/sqlite_build && rm -f libsqlite3.so sqlite3 && make -j4 "OPTS=-DSQLITE_ENABLE_LYRORE=1 -DSQLITE_ENABLE_STMT_SCANSTATUS=1 -DSQLITE_ENABLE_PREUPDATE_HOOK=1" "LDFLAGS=-rdynamic" && cd ~/sqlite/ext/lyrore_sdk && make clean && make && ./build/lyrore_main_test
```

### Run Main Test Suite

```bash
cd ~/sqlite/ext/lyrore_sdk
./build/lyrore_main_test
```

Expected output:
```
=== LyroreMain Test Suite ===
Test 1: Singleton Lifecycle... PASS
Test 2: Plugin Hot-Reload... PASS
Test 3: Transaction Version Binding... PASS
Test 4: Pre-Parse Dialect Transform... PASS
Test 5: Statement-Level Statistics... PASS
Test 6: Per-Scan Statistics... PASS
Test 7: Shared Resource Registry... PASS
Test 8: SDK Compatibility... PASS (storage tests verified, 58x speedup)

=== Results: 8 passed, 0 failed ===
```

### E2E Demo: Storage Operator Performance

```cpp
// See tests/storage_test.cpp for complete implementation
// Demonstrates: columnar storage with 30x+ GROUP BY speedup
// Verified by Test 8 in lyrore_main_test
```
