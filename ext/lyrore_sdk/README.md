# Lyrore C++ Plugin SDK

A C++ SDK for building SQLite query optimization plugins. Lyrore enables runtime-loadable plugins that can transform SQL queries, adjust cost estimates, collect statistics, and more.

## Quick Start

### Prerequisites

- SQLite built with Lyrore support
- GCC/G++ with C++17 support
- Linux (tested on Ubuntu)

### 1. Build SQLite with Lyrore

```bash
# Clone/navigate to SQLite source
cd ~/sqlite

# Create out-of-tree build directory
mkdir -p ~/sqlite_build && cd ~/sqlite_build

# Configure (only needed once)
~/sqlite/configure

# Build with Lyrore enabled
make -j4 "OPTS=-DSQLITE_ENABLE_LYRORE=1 -DSQLITE_ENABLE_STMT_SCANSTATUS=1" "LDFLAGS=-rdynamic"
```

**Important:** The `-rdynamic` flag is required so plugins can call SQLite APIs.

### 2. Build Example Plugins

```bash
cd ~/sqlite/ext/lyrore_sdk
make
```

This builds:
- `udf_transform.so` - Transforms UDF calls to native expressions (~60x speedup)
- `histogram.so` - Better cardinality estimation via histograms
- `passthrough_test.so` - Correctness test for AST manipulation
- `product_filter_udf.so` - Test UDF for benchmarking

### 3. Test a Plugin

```bash
cd ~/sqlite_build

./sqlite3 :memory: << 'EOF'
-- Enable Lyrore
PRAGMA lyrore_enabled = ON;
PRAGMA lyrore_plugins = ON;

-- Load a plugin
SELECT lyrore_register('/home/ubuntu/sqlite/ext/lyrore_sdk/passthrough_test.so');

-- Run queries (plugin hooks are invoked automatically)
CREATE TABLE test(a INT, b INT);
INSERT INTO test VALUES (1, 10), (2, 20), (3, 30);
SELECT * FROM test WHERE a > 1;
EOF
```

Expected output:
```
Plugin loaded successfully
2|20
3|30
```

---

## Architecture

```
+---------------------------------------------+
|          C++ Plugin (.so)                   |
|  class MyPlugin : public lyrore::Plugin     |
+---------------------------------------------+
                    |
                    v
+---------------------------------------------+
|        C++ SDK (lyrore_plugin.hpp)          |
|  - LyExpr class (AST manipulation)          |
|  - Plugin base class                        |
|  - Context classes for each hook            |
+---------------------------------------------+
                    |
                    v
+---------------------------------------------+
|         SQLite with Lyrore Support          |
|  - Hook entry points in query pipeline      |
|  - Plugin loading via lyrore_register()     |
+---------------------------------------------+
```

---

## Plugin API

### Plugin Base Class

```cpp
#include "lyrore_plugin.hpp"

class MyPlugin : public lyrore::Plugin {
public:
    std::string name() const override { return "my_plugin"; }

    // Lifecycle
    void onInit(sqlite3* db) override { /* initialization */ }
    void onShutdown() override { /* cleanup */ }

    // Hooks - override what you need
    void onPreOpt(lyrore::PreOptContext& ctx) override { /* transform AST */ }
    void onEstimate(lyrore::EstimateContext& ctx) override { /* adjust estimates */ }
    void onPostQuery(lyrore::PostQueryContext& ctx) override { /* collect stats */ }
    void onAnalyze(lyrore::AnalyzeContext& ctx) override { /* train models */ }
};

// Required macro at end of file
LYRORE_REGISTER_PLUGIN(MyPlugin);
```

### Hook Types

| Hook | When Called | Use Case |
|------|-------------|----------|
| `onPreOpt` | Before WHERE optimization | Transform expressions, rewrite UDFs |
| `onEstimate` | During plan costing | Adjust cardinality estimates |
| `onPostQuery` | After query execution | Collect execution statistics |
| `onAnalyze` | During ANALYZE command | Build statistical models |

---

## LyExpr Class - AST Manipulation

The `LyExpr` class provides safe manipulation of SQLite expression trees.

### Converting Expressions

```cpp
// SQLite Expr* -> LyExpr
auto expr = lyrore::LyExpr::from_sqlite(ctx.where_raw());

// LyExpr -> SQLite Expr* (in-place replacement)
new_expr->to_sqlite(ctx.parse(), ctx.where_raw());
ctx.set_modified();
```

### Building Expressions

```cpp
// Literals
auto num = lyrore::LyExpr::integer(42);
auto pi = lyrore::LyExpr::floating(3.14);
auto str = lyrore::LyExpr::string("hello");

// Arithmetic: (a * b)
auto mul = lyrore::LyExpr::multiply(expr_a, expr_b);

// Comparison: (x < 100)
auto cmp = lyrore::LyExpr::less_than(expr_x, lyrore::LyExpr::integer(100));

// Logical: (a > 0 AND b > 0)
auto both = lyrore::LyExpr::logical_and(cond_a, cond_b);
```

### Querying Expressions

```cpp
if (expr->is_function("product_filter")) {
    // It's a call to product_filter()
    size_t n = expr->nargs();  // Number of arguments
    auto arg0 = expr->arg(0);  // First argument
}

if (expr->is_column()) {
    std::string table = expr->table_name;
    std::string column = expr->column_name;
}

if (expr->is_integer()) {
    auto val = expr->as_int();  // std::optional<int64_t>
}
```

---

## Context Classes

### PreOptContext

```cpp
void onPreOpt(lyrore::PreOptContext& ctx) {
    auto where = ctx.where();           // Get WHERE as LyExpr
    Expr* raw = ctx.where_raw();        // Get raw Expr*
    Parse* parse = ctx.parse();         // For to_sqlite()

    // After modification:
    ctx.set_modified();
}
```

### EstimateContext

```cpp
void onEstimate(lyrore::EstimateContext& ctx) {
    std::string table = ctx.table_name();
    int64_t est = ctx.cardinality();

    // Set new estimate
    ctx.set_cardinality(1000);

    // Get WHERE terms for pattern matching
    auto terms = ctx.get_where_terms();
}
```

### AnalyzeContext

```cpp
void onAnalyze(lyrore::AnalyzeContext& ctx) {
    // Query the database
    auto rs = ctx.query("SELECT col FROM table");
    while (rs && rs->next()) {
        int64_t val = rs->get_int(0);
        double d = rs->get_double(0);
    }
}
```

---

## Example: UDF Transform Plugin

This example transforms a slow UDF call into native arithmetic:

`product_filter(price, qty, threshold) = 1` → `(price * qty) < threshold`

### Plugin Code (~45 lines)

```cpp
#include "lyrore_plugin.hpp"

class UdfTransformPlugin : public lyrore::Plugin {
public:
    std::string name() const override { return "udf_transform"; }

    void onPreOpt(lyrore::PreOptContext& ctx) override {
        auto where = ctx.where();
        if (!where || where->op != lyrore::LyOp::EQ) return;

        lyrore::LyExpr* func = where->left.get();
        lyrore::LyExpr* val = where->right.get();

        // Handle reversed order: 1 = product_filter(...)
        if (val && val->is_function()) std::swap(func, val);

        // Match: product_filter(a, b, c) = 1
        if (!func || !func->is_function("product_filter")) return;
        if (func->nargs() != 3) return;
        if (!val || !val->is_integer()) return;
        auto int_val = val->as_int();
        if (!int_val || *int_val != 1) return;

        // Build: (arg0 * arg1) < arg2
        auto mul = lyrore::LyExpr::multiply(func->arg(0), func->arg(1));
        auto cmp = lyrore::LyExpr::less_than(std::move(mul), func->arg(2));

        // Substitute back
        cmp->to_sqlite(ctx.parse(), ctx.where_raw());
        ctx.set_modified();
    }
};

LYRORE_REGISTER_PLUGIN(UdfTransformPlugin);
```

### Performance Results (Verified)

| Metric | Without Plugin | With Plugin |
|--------|----------------|-------------|
| Time | ~62ms | ~1ms |
| Opcode | `Function product_filter(3)` | `Multiply` |
| Result | 7900 rows | 7900 rows (identical) |

**Speedup: ~60x**

---

## End-to-End Demo (Verified Working)

Here's a complete, copy-paste-ready demo:

### Step 1: Create Test Database

```bash
cd ~/sqlite_build

./sqlite3 /tmp/demo.db << 'EOF'
DROP TABLE IF EXISTS products;
CREATE TABLE products(id INTEGER PRIMARY KEY, price REAL, qty INT);

WITH RECURSIVE cnt(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM cnt WHERE x < 10000)
INSERT INTO products(id, price, qty) SELECT x, (x % 100) + 1.0, (x % 50) + 1 FROM cnt;

SELECT COUNT(*) || ' rows created' FROM products;
EOF
```

Output: `10000 rows created`

### Step 2: Benchmark WITHOUT Plugin

```bash
cd ~/sqlite_build

./sqlite3 /tmp/demo.db << 'EOF'
.load /home/ubuntu/sqlite/ext/lyrore_sdk/product_filter_udf
.timer on
SELECT COUNT(*) FROM products WHERE product_filter(price, qty, 2500) = 1;
.timer off
EXPLAIN SELECT * FROM products WHERE product_filter(price, qty, 2500) = 1;
EOF
```

Expected:
- Time: ~60ms
- EXPLAIN shows `Function product_filter(3)` at line 6

### Step 3: Benchmark WITH Plugin

```bash
cd ~/sqlite_build

./sqlite3 /tmp/demo.db << 'EOF'
PRAGMA lyrore_enabled = ON;
PRAGMA lyrore_plugins = ON;
.load /home/ubuntu/sqlite/ext/lyrore_sdk/product_filter_udf
SELECT lyrore_register('/home/ubuntu/sqlite/ext/lyrore_sdk/udf_transform.so');
.timer on
SELECT COUNT(*) FROM products WHERE product_filter(price, qty, 2500) = 1;
.timer off
EXPLAIN SELECT * FROM products WHERE product_filter(price, qty, 2500) = 1;
EOF
```

Expected:
- Output: `Plugin loaded successfully`
- Time: ~1ms
- EXPLAIN shows `Multiply` at line 6 instead of `Function`

---

## Building Your Own Plugin

### 1. Create Plugin Source

```cpp
// my_plugin.cpp
#include "lyrore_plugin.hpp"

class MyPlugin : public lyrore::Plugin {
public:
    std::string name() const override { return "my_plugin"; }

    void onPreOpt(lyrore::PreOptContext& ctx) override {
        // Your transformation logic here
    }
};

LYRORE_REGISTER_PLUGIN(MyPlugin);
```

### 2. Build

Using the Makefile:
```bash
cd ~/sqlite/ext/lyrore_sdk
make my_plugin.so
```

Or manually:
```bash
g++ -std=c++17 -fPIC -O2 \
    -I~/sqlite/ext/lyrore_sdk/include \
    -I~/sqlite/src \
    -I~/sqlite_build \
    -shared -ldl \
    -o my_plugin.so my_plugin.cpp
```

### 3. Load and Test

```sql
PRAGMA lyrore_enabled = ON;
PRAGMA lyrore_plugins = ON;
SELECT lyrore_register('/path/to/my_plugin.so');
-- Run queries, plugin hooks are invoked automatically
```

---

## Pragmas Reference

| Pragma | Purpose |
|--------|---------|
| `PRAGMA lyrore_enabled = ON/OFF` | Master switch for Lyrore |
| `PRAGMA lyrore_plugins = ON/OFF` | Allow plugin loading (security) |
| `PRAGMA lyrore_cost = ON/OFF` | Enable estimate hooks |

---

## Files Reference

```
ext/lyrore_sdk/
├── include/
│   └── lyrore_plugin.hpp    # Main SDK header
├── src/
│   ├── cpp_context.cpp      # C++ plugin manager
│   └── ly_expr.cpp          # LyExpr implementation
├── examples/
│   ├── udf_transform.cpp    # UDF transformation example
│   ├── histogram.cpp        # Histogram estimation example
│   └── passthrough_test.cpp # AST round-trip test
├── product_filter_udf.c     # Test UDF source
├── Makefile                 # Build rules
└── README.md                # This file
```

---

## Troubleshooting

### Plugin won't load

1. Check that `PRAGMA lyrore_plugins = ON` is set
2. Verify the .so file path is correct and absolute
3. Ensure SQLite was built with `-rdynamic`

### Plugin hooks not called

1. Ensure `PRAGMA lyrore_enabled = ON`
2. For estimate hooks, also enable `PRAGMA lyrore_cost = ON`
3. Check plugin is loaded: `SELECT lyrore_register(...)` should print "Plugin loaded successfully"

### Build errors

1. Verify SQLite build exists at `~/sqlite_build`
2. Check include paths point to correct locations
3. Ensure C++17 support: `g++ --version` (need 7.0+)

### UDF extension won't load

1. Use `.load path/to/extension` without the `.so` extension
2. The sqlite3 shell auto-appends `.so` to the path

---

## API Quick Reference

### LyOp (Expression Types)

| Comparisons | Arithmetic | Logical | Leaf Types |
|-------------|------------|---------|------------|
| EQ (=) | ADD (+) | AND | COLUMN |
| NE (!=) | SUB (-) | OR | INTEGER |
| LT (<) | MUL (*) | NOT | FLOAT |
| LE (<=) | DIV (/) | | STRING |
| GT (>) | MOD (%) | | NULL_VAL |
| GE (>=) | | | FUNCTION |

### LyExpr Static Builders

```cpp
// Arithmetic
LyExpr::add(l, r)       LyExpr::subtract(l, r)
LyExpr::multiply(l, r)  LyExpr::divide(l, r)
LyExpr::mod(l, r)

// Comparison
LyExpr::equals(l, r)        LyExpr::not_equals(l, r)
LyExpr::less_than(l, r)     LyExpr::less_equal(l, r)
LyExpr::greater_than(l, r)  LyExpr::greater_equal(l, r)

// Logical
LyExpr::logical_and(l, r)   LyExpr::logical_or(l, r)
LyExpr::logical_not(expr)

// Literals
LyExpr::integer(val)    LyExpr::floating(val)
LyExpr::string(val)     LyExpr::null()

// Copy column binding
LyExpr::column(source_expr)
```

---

## License

Part of the Lyrore SQLite project. See main repository for license details.
