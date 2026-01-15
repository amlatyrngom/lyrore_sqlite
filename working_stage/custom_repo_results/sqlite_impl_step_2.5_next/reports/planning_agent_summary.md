# Step 2.5 Implementation Plan: Plugin Refactor to C++ SDK

## Summary
Complete refactor from C plugin system to C++ SDK:
- DELETE: 12 lyrore_*.c/h files (~2,913 lines)
- CREATE: C++ SDK in ext/lyrore_sdk/ + ~100 LOC C glue

## 1. Files to DELETE (from src/)

| File | Lines | Purpose |
|------|-------|---------|
| lyrore_model.c | 289 | Model registration, context lifecycle |
| lyrore_model.h | 413 | Core types, LyroreContext, patterns |
| lyrore_pattern.c | 574 | Pattern matching dispatch |
| lyrore_pattern.h | 55 | Pattern types |
| lyrore_features.c | 145 | Feature extraction |
| lyrore_features.h | 26 | Feature types |
| lyrore_hooks.c | 540 | Hook registration/invocation |
| lyrore_hooks.h | 85 | Hook types |
| lyrore_plugin.c | 318 | Plugin loading |
| lyrore_plugin.h | 45 | Plugin types |
| lyrore_stats.c | 376 | State persistence |
| lyrore_stats.h | 47 | Stats types |
| **Total** | ~2,913 | |

## 2. Files to CREATE

### 2.1 C Glue (src/lyrore_cabi.h + src/lyrore_cabi.c) ~100 LOC

```c
// lyrore_cabi.h
typedef struct LyroreCppContext LyroreCppContext;
LyroreCppContext* lyrore_cpp_create(sqlite3* db);
void lyrore_cpp_destroy(LyroreCppContext* ctx);
int lyrore_cpp_load_plugin(LyroreCppContext* ctx, const char* path);
int lyrore_cpp_invoke_preopt(LyroreCppContext* ctx, void* parse, void* select);
void lyrore_cpp_invoke_estimate(LyroreCppContext* ctx, void* builder, void* loop);
void lyrore_cpp_invoke_analyze(LyroreCppContext* ctx, int iDb);
void lyrore_cpp_invoke_postquery(LyroreCppContext* ctx, void* vdbe);
```

### 2.2 C++ SDK Structure (ext/lyrore_sdk/)

```
ext/lyrore_sdk/
├── include/
│   └── lyrore_plugin.hpp    # ~500 LOC - Plugin base, LyExpr, Contexts
├── src/
│   ├── ly_expr.cpp          # ~400 LOC - LyExpr implementation
│   └── cpp_context.cpp      # ~200 LOC - C++ plugin manager
├── examples/
│   ├── udf_transform.cpp    # ~50 LOC
│   ├── histogram.cpp        # ~80 LOC
│   └── passthrough_test.cpp # ~30 LOC
├── Makefile
└── README.md
```

### 2.3 Key Classes in lyrore_plugin.hpp

```cpp
namespace lyrore {

// LyExpr - AST manipulation
class LyExpr {
    static LyExprPtr from_sqlite(Expr* pExpr);
    void to_sqlite(Parse* pParse, Expr* pTarget) const;
    Expr* to_sqlite_new(Parse* pParse) const;

    // Builders
    static LyExprPtr multiply(LyExprPtr l, LyExprPtr r);
    static LyExprPtr less_than(LyExprPtr l, LyExprPtr r);
    static LyExprPtr integer(int64_t val);
    // ... all ops

    LyOp op;
    LyExprPtr left, right;
    std::vector<LyExprPtr> args;
    Expr* sqlite_expr;  // For column binding
};

// Plugin base class
class Plugin {
    virtual std::string name() const = 0;
    virtual void onInit(sqlite3* db) {}
    virtual void onShutdown() {}
    virtual void onPreOpt(PreOptContext& ctx) {}
    virtual void onEstimate(EstimateContext& ctx) {}
    virtual void onPostQuery(PostQueryContext& ctx) {}
    virtual void onAnalyze(AnalyzeContext& ctx) {}
};

// Context classes
class PreOptContext {
    LyExprPtr where();
    Expr* where_raw();
    Parse* parse();
    void set_modified();
};

class EstimateContext {
    std::string table_name();
    int64_t cardinality();
    void set_cardinality(int64_t rows);
};

class AnalyzeContext {
    sqlite3* db();
    std::unique_ptr<ResultSet> query(const std::string& sql);
};

}

#define LYRORE_REGISTER_PLUGIN(PluginClass) \
    extern "C" { \
        lyrore::Plugin* lyrore_create_plugin() { return new PluginClass(); } \
        void lyrore_destroy_plugin(lyrore::Plugin* p) { delete p; } \
    }
```

## 3. SQLite Files to MODIFY

### 3.1 src/sqliteInt.h
- Replace LyroreContext* with void* pCppCtx
- Keep pragma flags (HI bits)

### 3.2 src/main.c  
- In openDatabase(): Call lyrore_cpp_create()
- In closeZombie(): Call lyrore_cpp_destroy()

### 3.3 src/select.c (line ~7740)
- Replace lyroreInvokePreOptHooks() with:
  ```c
  #ifdef SQLITE_ENABLE_LYRORE
  lyrore_cpp_invoke_preopt(db->pCppCtx, pParse, p);
  #endif
  ```

### 3.4 src/where.c (line ~2838)
- Replace lyroreInvokeEstimateHooks() with lyrore_cpp_invoke_estimate()

### 3.5 src/vdbe.c (line ~7189)
- Replace lyroreInvokeAnalyzeHooks() with lyrore_cpp_invoke_analyze()

### 3.6 src/vdbeaux.c (line ~3338)
- Replace lyroreInvokePostQueryHooks() with lyrore_cpp_invoke_postquery()

### 3.7 src/pragma.c
- Keep custom handlers for persist/reset (may simplify)

## 4. Build System Changes

### 4.1 main.mk
- Remove: LYRORE_OBJ entries for old .c files
- Add: lyrore_cabi.o
- Add rule to compile lyrore_cabi.c with C++ linking

### 4.2 tool/mksqlite3c.tcl
- Remove: all lyrore_*.c/h from amalgamation
- Add: lyrore_cabi.c/h only

## 5. Test Plan

### 5.1 Pass-through Test (CRITICAL for correctness)
Tests that LyExpr from_sqlite->clone->to_sqlite produces identical results:
```sql
SELECT * FROM t WHERE a + b > 10;
SELECT * FROM t WHERE x = 5 AND y > 0;
SELECT * FROM t WHERE (a + b) * c > (d - e) / f;
-- etc.
```

### 5.2 UDF Transform E2E Test
- Create products table with 10k rows
- Query: `SELECT COUNT(*) FROM products WHERE product_filter(price, qty, 50000) = 1`
- Without plugin: ~171ms
- With plugin: <6ms (30x+ speedup)
- Verify: EXPLAIN shows Multiply opcode, not Function

### 5.3 Histogram Estimation E2E Test  
- Build histogram during ANALYZE
- Compare SQLite default Q-error vs histogram plugin
- Target: Q-error < 2x (vs 14.6x baseline)

## 6. Implementation Order

1. **Phase 1: Create C++ SDK skeleton**
   - ext/lyrore_sdk/include/lyrore_plugin.hpp
   - Basic types (LyOp, LyValue, Plugin class)

2. **Phase 2: Implement LyExpr class**
   - from_sqlite() with deep copy
   - to_sqlite() with in-place substitution
   - All builder functions

3. **Phase 3: Implement contexts and plugin manager**
   - PreOptContext, EstimateContext, AnalyzeContext
   - cpp_context.cpp with plugin loading

4. **Phase 4: Create C glue**
   - lyrore_cabi.h/c
   - Connect to SQLite integration points

5. **Phase 5: Update SQLite files**
   - Modify hook call sites
   - Update build system

6. **Phase 6: Delete old C code**
   - Remove all lyrore_*.c/h from src/

7. **Phase 7: Create example plugins and tests**
   - passthrough_test.cpp
   - udf_transform.cpp
   - histogram.cpp
   - E2E benchmark scripts

## 7. Success Criteria

| Metric | Target |
|--------|--------|
| Pass-through correctness | 100% identical results |
| UDF speedup | ≥30x (171ms → <6ms) |
| Histogram Q-error | <2x (vs 14.6x baseline) |
| Plugin LOC (UDF) | <60 lines |
| C glue LOC | <100 lines |
