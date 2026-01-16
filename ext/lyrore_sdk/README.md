# Lyrore C++ Plugin SDK

A C++ SDK for SQLite query optimization plugins. Build plugins that transform queries, adjust cost estimates, and collect statistics.

## Quick Start

```bash
# Build SQLite with Lyrore (one-time)
mkdir -p ~/sqlite_build && cd ~/sqlite_build
~/sqlite/configure
make -j4 "OPTS=-DSQLITE_ENABLE_LYRORE=1" "LDFLAGS=-rdynamic"

# Build plugins
cd ~/sqlite/ext/lyrore_sdk && make
```

## Pattern API (Recommended)

Match SQL queries using SQLite's own parser. No manual AST traversal needed.

### Example: Match and Extract Parameters

```cpp
#include "lyrore_plugin.hpp"
#include "lyrore_pattern.hpp"

class MyPlugin : public lyrore::Plugin {
    lyrore::pattern::PatternPtr pattern_;

public:
    std::string name() const override { return "my_plugin"; }

    void onInit(sqlite3* db) override {
        // Define pattern using SQL - '?' marks parameters to extract
        pattern_ = lyrore::pattern::Pattern::from_query(db,
            "SELECT * FROM orders WHERE customer_id = ? AND amount > ?");
    }

    void onPreOpt(lyrore::PreOptContext& ctx) override {
        if (auto m = pattern_->match(ctx.select_raw())) {
            // Pattern matched! Extract parameters:
            int64_t cust_id = m.get<int64_t>("$1");
            double min_amt = m.get<double>("$2");
            // Use for optimization decisions...
        }
    }
};

LYRORE_REGISTER_PLUGIN(MyPlugin);
```

### Pattern Creation Methods

```cpp
// Match full SELECT queries
auto p1 = Pattern::from_query(db, "SELECT SUM(x) FROM t WHERE a = ?");

// Match WHERE expressions only
auto p2 = Pattern::from_where_expr(db, "SELECT 1 FROM t WHERE (price * qty) < ?");
```

### Match Result API

```cpp
if (auto m = pattern->match(select)) {
    // By position: $1, $2, ...
    int64_t val1 = m.get<int64_t>("$1");
    double val2 = m.get<double>("$2");
    std::string val3 = m.get<std::string>("$3");
}
```

### Cross-Hook State

```cpp
void onPreOpt(PreOptContext& ctx) {
    if (auto m = pattern_->match(ctx.select_raw())) {
        set_template_match(ctx.select_raw(), TEMPLATE_ID, m);
    }
}

void onEstimate(EstimateContext& ctx) {
    if (auto tid = get_template_id(ctx.select_raw())) {
        auto* params = get_template_params(ctx.select_raw());
        // Use cached match from PreOpt...
    }
}
```

## Plugin Hooks

| Hook | Purpose |
|------|---------|
| `onPreOpt` | Transform AST before optimization |
| `onEstimate` | Override cardinality estimates |
| `onAnalyze` | Build models during ANALYZE |
| `onPostQuery` | Collect execution stats |

## Loading Plugins

```sql
PRAGMA lyrore_enabled = ON;
PRAGMA lyrore_plugins = ON;
SELECT lyrore_register('/path/to/plugin.so');
```

## Examples

See `examples/` directory:
- `histogram.cpp` - Pattern-based cardinality estimation
- `udf_transform.cpp` - AST transformation using raw SQLite C API
- `pattern_e2e_test.cpp` - Comprehensive pattern matching tests

## Low-Level AST Manipulation

For direct AST manipulation (advanced use cases), use the raw SQLite C API via `lyrore_cabi.h` wrappers:

```cpp
#include "lyrore_cabi.h"

void onPreOpt(PreOptContext& ctx) {
    Expr* pWhere = ctx.where_raw();
    sqlite3* db = ctx.db();

    // Duplicate expressions to preserve Table* bindings
    Expr* pDup = lyrore_sqlite3ExprDup(db, pWhere->pLeft, 0);

    // Create new expressions
    Expr* pMul = lyrore_sqlite3Expr(db, TK_STAR, nullptr);
    pMul->pLeft = lyrore_sqlite3ExprDup(db, price_col, 0);
    pMul->pRight = lyrore_sqlite3ExprDup(db, qty_col, 0);

    // Create comparison
    Expr* pLt = lyrore_sqlite3Expr(db, TK_LT, nullptr);
    pLt->pLeft = pMul;
    pLt->pRight = lyrore_sqlite3ExprDup(db, threshold, 0);

    // In-place substitution
    lyrore_sqlite3ExprDelete(db, pWhere->pLeft);
    lyrore_sqlite3ExprDelete(db, pWhere->pRight);
    pWhere->op = pLt->op;
    pWhere->pLeft = pLt->pLeft;
    pWhere->pRight = pLt->pRight;
    pLt->pLeft = pLt->pRight = nullptr;
    lyrore_sqlite3DbFree(db, pLt);

    ctx.set_modified();
}
```

### Available C API Wrappers (lyrore_cabi.h)

| Function | Purpose |
|----------|---------|
| `lyrore_sqlite3Expr(db, op, token)` | Create new expression |
| `lyrore_sqlite3ExprDup(db, expr, flags)` | Duplicate expression (preserves bindings) |
| `lyrore_sqlite3ExprDelete(db, expr)` | Free expression tree |
| `lyrore_sqlite3StrICmp(s1, s2)` | Case-insensitive string compare |
| `lyrore_sqlite3DbFree(db, ptr)` | Free memory |

## Files

```
ext/lyrore_sdk/
├── include/
│   ├── lyrore_plugin.hpp    # Plugin base, contexts, LyValue
│   └── lyrore_pattern.hpp   # Pattern API
├── src/                     # SDK implementation
├── examples/                # Example plugins
└── tests/                   # Test suite
```
