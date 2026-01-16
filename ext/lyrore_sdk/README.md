# Lyrore C++ Plugin SDK

A C++ SDK for SQLite query optimization plugins. Build plugins that transform queries, adjust cost estimates, collect statistics, and replace query subtrees with custom computation.

## Quick Start

```bash
# Build SQLite with Lyrore (one-time)
mkdir -p ~/sqlite_build && cd ~/sqlite_build
~/sqlite/configure
make -j4 "OPTS=-DSQLITE_ENABLE_LYRORE=1" "LDFLAGS=-rdynamic"

# Build plugins
cd ~/sqlite/ext/lyrore_sdk && make
```

## Loading Plugins

```sql
PRAGMA lyrore_enabled = ON;
PRAGMA lyrore_plugins = ON;
SELECT lyrore_register('/path/to/plugin.so');
```

## Plugin Hooks

| Hook | Purpose |
|------|---------|
| `onInit` | Initialize patterns, register custom operators |
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
│   ├── lyrore_plugin.hpp     # Plugin base, contexts, LyValue
│   ├── lyrore_pattern.hpp    # Pattern matching API
│   └── lyrore_custom_op.hpp  # Custom operators API
├── src/                      # SDK implementation
├── examples/
│   ├── histogram.cpp         # Cardinality estimation
│   ├── udf_transform.cpp     # AST transformation
│   └── fast_groupby.cpp      # Custom GROUP BY operator
└── tests/                    # Test suite
```

## Examples

- **histogram.cpp** - Pattern-based histogram cardinality estimation
- **udf_transform.cpp** - UDF to native expression transformation (30x+ speedup)
- **fast_groupby.cpp** - Custom GROUP BY using ITERATOR mode

---

## Build & Test

### Complete Build from Scratch

Follow these steps to build everything from a clean state:

```bash
# ============================================
# STEP 1: Clean everything
# ============================================
rm -f ~/sqlite_build/libsqlite3.so ~/sqlite_build/sqlite3
rm -f ~/sqlite/ext/lyrore_sdk/*.so

# ============================================
# STEP 2: Build SQLite with Lyrore
# ============================================
cd ~/sqlite_build
make -j4 "OPTS=-DSQLITE_ENABLE_LYRORE=1" "LDFLAGS=-rdynamic"

# ============================================
# STEP 3: Build SDK plugins
# ============================================
cd ~/sqlite/ext/lyrore_sdk
make clean && make

# ============================================
# STEP 4: Run tests
# ============================================
cd ~/sqlite/ext/lyrore_sdk
~/sqlite_build/sqlite3 :memory: <<'EOF'
PRAGMA lyrore_enabled=ON;
PRAGMA lyrore_plugins=ON;
SELECT lyrore_register('./custom_op_test.so');
SELECT run_custom_op_tests();
EOF
```

### One-Liner for Full Rebuild & Test

```bash
cd ~/sqlite_build && rm -f libsqlite3.so sqlite3 && make -j4 "OPTS=-DSQLITE_ENABLE_LYRORE=1" "LDFLAGS=-rdynamic" && cd ~/sqlite/ext/lyrore_sdk && make clean && make && ~/sqlite_build/sqlite3 :memory: "PRAGMA lyrore_enabled=ON; PRAGMA lyrore_plugins=ON; SELECT lyrore_register('./custom_op_test.so'); SELECT run_custom_op_tests();"
```

### Run Test Suite

Run the comprehensive test suite for custom operators:

```bash
cd ~/sqlite/ext/lyrore_sdk
~/sqlite_build/sqlite3 :memory: <<'EOF'
PRAGMA lyrore_enabled=ON;
PRAGMA lyrore_plugins=ON;
SELECT lyrore_register('./custom_op_test.so');
SELECT run_custom_op_tests();
EOF
```

Expected output:
```
=== Custom Operator Test Suite ===
PASS: TableCursor basic operations work correctly
PASS: TableCursor handles empty tables correctly
...
ALL TESTS PASSED
```

### E2E Demo: Fast GROUP BY

Complete self-contained demo showing the SDK in action:

```bash
cd ~/sqlite/ext/lyrore_sdk
~/sqlite_build/sqlite3 :memory: <<'EOF'
-- Enable Lyrore
PRAGMA lyrore_enabled=ON;
PRAGMA lyrore_plugins=ON;

-- Create test table with 10K rows
CREATE TABLE orders(id INTEGER PRIMARY KEY, category_id INT, qty INT);
WITH RECURSIVE cnt(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM cnt WHERE x<10000)
INSERT INTO orders SELECT x, x%5, x*10 FROM cnt;

-- Load the fast GROUP BY plugin
SELECT lyrore_register('./fast_groupby.so');

-- Run GROUP BY query (uses custom operator)
SELECT category_id, SUM(qty) FROM orders GROUP BY category_id;
EOF
```

Expected output:
```
0|100050000
1|99970000
2|99990000
3|100010000
4|100030000
```

