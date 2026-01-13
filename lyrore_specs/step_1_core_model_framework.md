# Step 1: Core Model Framework & State Infrastructure

## 1. Goal

Establish the foundational infrastructure that ALL subsequent Lyrore optimizations depend on:
- **LyroreContext**: Central state container attached to each `sqlite3` connection
- **LyroreModelOps**: Pluggable model interface for ML-ready adaptive decisions
- **LyroreFeatures**: Structured feature extraction from query plans
- **State Persistence**: Dedicated connection for persisting model state
- **PRAGMA Configuration**: Enable/disable Lyrore features at runtime

This step produces NO performance improvements—it builds infrastructure for Steps 2-7.

---

## 2. Architecture

```
┌────────────────────────────────────────────────────────────────┐
│                          sqlite3 struct                         │
│  ┌──────────────────────────────────────────────────────────┐  │
│  │                     LyroreContext                         │  │
│  │   - Model registry (purpose → ops + state)                │  │
│  │   - pStateDb: dedicated connection for state persistence  │  │
│  │   - dirty counter for periodic persistence                │  │
│  └──────────────────────────────────────────────────────────┘  │
└────────────────────────────────────────────────────────────────┘
```

### Key Design Decisions

| Decision | Rationale |
|----------|-----------|
| **Dedicated state connection** | Prevents transaction conflicts; Lyrore persists independently of user transactions |
| **Function pointer interface** | Matches SQLite's C style; allows model swapping without recompilation |
| **Features struct → array conversion** | Decouples extraction from models; models are feature-agnostic |
| **Flags in db->flags** | Reuses existing infrastructure; high bits still available in u64 |

---

## 3. Core Types

### LyroreFeatures

**Key Clarification**: 
- LyroreFeatures is purely INPUT to models (query/plan characteristics)
- Model weights are INTERNAL to each model (stored in model state via xSave/xLoad), NOT in LyroreFeatures
- `aExtended` contains additional FEATURES, not model parameters

**Explicit Struct Definition:**

```c
typedef struct LyroreFeatures {
    /* Pre-defined static fields (well-known, interpretable features) */

    /* Query structure */
    int nTables, nJoins, nPredicates, nAggregations;
    int queryDepth, nProjections;

    /* Cost estimates */
    double estTotalRows, estOutputRows, estCost, avgSelectivity;

    /* Access patterns */
    int nIndexScans, nTableScans, hasEquality, hasRange, hasLike;

    /* Historical (from past executions) */
    double lastExecTime, avgExecTime;
    int execCount;

    /* Dynamic extension vector (for additional features, NOT weights) */
    int nExtended;      /* count of extended feature values */
    double *aExtended;  /* dynamically allocated array for additional features */
                        /* Examples: embeddings, table stats, workload features */
} LyroreFeatures;
```

**Design Rationale:**
1. **Static fields** = well-known, stable, interpretable features (usable by simple heuristics)
2. **aExtended** = additional FEATURES (NOT model weights) for ML extensibility
3. **Model weights** are internal to each model's state (via xSave/xLoad)
4. **lyroreFeaturesToArray()** helper combines both parts into single `double[]` for model input

### LyroreModelOps
Pluggable model interface with:
- `xCreate/xDestroy`: Lifecycle
- `xPredict`: Continuous prediction (cost estimation)
- `xSelect`: Discrete selection (join order, flavor)
- `xUpdate/xReward`: Learning callbacks
- `xSave/xLoad`: State persistence

### LyroreContext
Per-connection context holding model registry and dedicated state connection.

---

## 4. API Summary

### Model Management (lyrore_model.c/h)
- `lyroreRegisterModel(db, purpose, ops, config)` - Register model for purpose
- `lyroreGetModel(db, purpose)` - Get model ops
- `lyroreGetModelState(db, purpose)` - Get model state

### Feature Extraction (lyrore_features.c/h)
- `lyroreExtractPlanFeatures(WhereInfo*, LyroreFeatures*)` - From completed plan
- `lyroreExtractLoopFeatures(WhereLoop*, Table*, LyroreFeatures*)` - From single loop
- `lyroreFeaturesToArray(LyroreFeatures*, double*, int)` - Convert to flat array

### State Persistence (lyrore_stats.c/h)
- `lyroreInit(db)` - Initialize context, open state connection, create tables
- `lyroreShutdown(db)` - Persist state, cleanup
- `lyroreMaybePersist(db)` - Periodic persistence (every N queries)
- `lyrorePersistNow(db)` - Force persistence
- `lyroreReset(db)` - Clear all state

---

## 5. SQLite Integration Points

### sqliteInt.h
- Add `struct LyroreContext *pLyrore` to `sqlite3` struct
- Add flags: `SQLITE_LyroreEnabled`, `SQLITE_LyroreCost`, `SQLITE_LyrorePlanRL`, `SQLITE_LyroreFlavor`, `SQLITE_LyroreFusion`

### main.c
- Call `lyroreInit()` in `openDatabase()`
- Call `lyroreShutdown()` in `sqlite3Close()`

### pragma.c
- Add `PRAGMA lyrore_enabled = ON|OFF`
- Add `PRAGMA lyrore_persist`
- Add `PRAGMA lyrore_reset`

### Makefile.in
- Add lyrore_*.o to LIBOBJ
- Add `-DSQLITE_ENABLE_LYRORE=1` to TCC

---

## 6. State Tables

```sql
-- Model state persistence
CREATE TABLE lyrore_model_state(
    purpose TEXT, context_key TEXT, state_blob BLOB,
    sample_count INT, last_updated INT,
    PRIMARY KEY(purpose, context_key)
);

-- Execution history
CREATE TABLE lyrore_exec_history(
    pattern_sig TEXT PRIMARY KEY, exec_count INT,
    total_time_us INT, last_exec_time_us INT
);

-- Column statistics (for selectivity)
CREATE TABLE lyrore_column_stats(
    tbl TEXT, col TEXT, distinct_count INT, total_count INT,
    PRIMARY KEY(tbl, col)
);
```

---

## 7. Testing Strategy

| Test | Verification |
|------|--------------|
| PRAGMA tests | `lyrore_enabled` toggles flag correctly |
| Model registration | Register, get, unregister models |
| Feature extraction | Extract from dummy WhereInfo/WhereLoop |
| State persistence | Values survive connection close/reopen |
| Curriculum passthrough | All queries run correctly (no optimization yet) |

---

## 8. Success Criteria

1. `PRAGMA lyrore_enabled = ON/OFF` works
2. Model registration API functional
3. Feature extraction produces valid features from plans
4. State tables created and persisted across connections
5. Dedicated state connection doesn't conflict with user transactions
6. Curriculum workload queries run unchanged (passthrough mode)

---

## 9. Risks & Mitigations

| Risk | Mitigation |
|------|------------|
| State connection conflicts | Open as separate connection, not attached DB |
| In-memory DB has no persistence | Gracefully handle NULL pStateDb |
| Flag bits exhausted | Use HI() macro for high bits of u64 |
| Pragma registration complexity | Follow existing pragma patterns exactly |

---

## 10. API Stability Guidelines

APIs defined here are foundational. If changes needed in later steps:

1. **Prefer extension over modification**: Add new fields/functions rather than change existing
2. **LyroreFeatures**: Add new feature fields; existing models unaffected (they select their features)
3. **LyroreModelOps**: All callbacks except xCreate are optional; add new optional callbacks
4. **State tables**: Add new tables rather than alter existing; use schema versioning if needed

---

## 11. Dependencies

- **Depends on**: Nothing (this is the foundation)
- **Depended on by**: All subsequent steps (2-7)

---

## 12. Estimated Effort

- New files: ~1500 LOC (lyrore_model.c/h, lyrore_features.c/h, lyrore_stats.c/h)
- Modified files: ~200 LOC (sqliteInt.h, main.c, pragma.c, Makefile.in)
- Tests: ~500 LOC
- **Total: ~2200 LOC**

---

## 13. Detailed Component Specifications

### 13.1 LyroreContext Lifecycle

**Creation** (in `lyroreInit`):
1. Allocate `LyroreContext` with `sqlite3MallocZero`
2. Get database path via `sqlite3_db_filename(db, "main")`
3. Open dedicated state connection with `sqlite3_open_v2` (READWRITE|CREATE)
4. Execute CREATE TABLE IF NOT EXISTS for state tables
5. Set `db->pLyrore = ctx` and enable `SQLITE_LyroreEnabled` flag

**Destruction** (in `lyroreShutdown`):
1. Call `lyrorePersistNow` if dirty counter > 0
2. For each registered model, call `xDestroy` if defined
3. Close state connection with `sqlite3_close`
4. Free context memory

**Special Case - In-Memory DBs**: When `zPath` is NULL or empty (in-memory database), set `pStateDb = NULL`. All persistence operations become no-ops. Models still work but don't persist across connections.

### 13.2 LyroreModelOps Detailed Semantics

| Callback | When Called | Return Value | Notes |
|----------|-------------|--------------|-------|
| `xCreate` | Registration | Model state ptr or NULL | REQUIRED; NULL = SQLITE_NOMEM |
| `xDestroy` | Unregister/Shutdown | void | Optional; cleanup resources |
| `xPredict` | Cost estimation | Predicted cost (double) | For continuous outputs |
| `xSelect` | Plan/flavor selection | Index 0..nOptions-1 | For discrete choices |
| `xUpdate` | After execution | void | Regression learning (predicted vs actual) |
| `xReward` | After execution | void | Bandit learning (selected + reward) |
| `xSave` | Periodic/explicit persist | SQLITE_OK or error | Serialize to lyrore_model_state |
| `xLoad` | Registration | SQLITE_OK or error | Deserialize from lyrore_model_state |

### 13.3 LyroreFeatures Field Reference

**Query Structure Features** (from WhereInfo):
- `nTables`: `pWInfo->pTabList->nSrc`
- `nJoins`: `nTables - 1` if nTables > 1
- `nPredicates`: `pWInfo->sWC.nTerm`
- `nAggregations`: Count of aggregate functions in SELECT
- `queryDepth`: Subquery nesting level
- `nProjections`: Number of result columns

**Cost Estimate Features** (from WhereLoop):
- `estTotalRows`: Table row estimate (`pTab->nRowLogEst` converted)
- `estOutputRows`: Loop output estimate (`pLoop->nOut` converted)
- `estCost`: Loop run cost (`pLoop->rRun` converted)
- `avgSelectivity`: Predicate selectivity from analysis

**Access Pattern Features** (from WhereLoop flags):
- `nIndexScans`: Count of WHERE_INDEXED loops
- `nTableScans`: Count of WHERE_IPK (full table) loops
- `hasEquality`: `pLoop->u.btree.nEq > 0`
- `hasRange`: `wsFlags & (WHERE_BTM_LIMIT|WHERE_TOP_LIMIT)`
- `hasLike`: LIKE predicate present

**Historical Features** (from lyrore_exec_history):
- `lastExecTime`: Most recent execution time
- `avgExecTime`: `total_time_us / exec_count`
- `execCount`: Number of times pattern executed

**Feature Selection**: Model-driven; each model specifies which features it uses via config. See Section 13.5.

### 13.4 Flag Bit Allocation

Using HI() macro for upper 32 bits of db->flags (u64):

```
Bit Position    Flag Name               Purpose
HI(0x0001000)   SQLITE_LyroreEnabled    Master enable
HI(0x0002000)   SQLITE_LyroreCost       Cost correction (Step 2)
HI(0x0004000)   SQLITE_LyrorePlanRL     Plan selection (Step 3)
HI(0x0008000)   SQLITE_LyroreFlavor     Expression flavor (Step 4)
HI(0x0010000)   SQLITE_LyroreFusion     Fused operators (Step 5)
HI(0x0020000)   SQLITE_LyroreColstore   Columnar storage (Step 7)
HI(0x0040000)   Reserved                Future use
HI(0x0080000)   Reserved                Future use
```

---

## 14. SQLite Integration Details

### 14.1 Adding Pragmas

SQLite pragmas are defined in `tool/mkpragmatab.tcl` which generates `pragma.h`. Add entries:

```tcl
{lyrore_enabled  PragTyp_LYRORE_ENABLED  PragFlg_NoColumns}
{lyrore_persist  PragTyp_LYRORE_PERSIST  PragFlg_NoColumns}
{lyrore_reset    PragTyp_LYRORE_RESET    PragFlg_NoColumns}
```

Then add case handlers in `sqlite3Pragma()` switch statement following pattern of `PragTyp_FLAG` handlers.

### 14.2 Include Dependencies

New Lyrore files need access to:
- `sqliteInt.h` - Core types (sqlite3, Parse, etc.)
- `whereInt.h` - WhereInfo, WhereLoop, WhereTerm, etc.
- Standard headers for memset, NULL, etc.

Header guard pattern:
```c
#ifndef SQLITE_LYRORE_MODEL_H
#define SQLITE_LYRORE_MODEL_H
#ifdef SQLITE_ENABLE_LYRORE
// ... declarations ...
#endif
#endif
```

### 14.3 Build Configuration

Two build modes:
1. **Enabled by default**: Add `-DSQLITE_ENABLE_LYRORE=1` to TCC in Makefile
2. **Optional**: Define only when explicitly requested

For curriculum workload testing, use enabled-by-default.

---

## 15. Curriculum Workload Integration

### 15.1 Passthrough Testing

In this step, the curriculum workload should run unchanged:
- All Q1-Q22 TPC-H queries execute correctly
- All Q_NEW_01-Q_NEW_20 UDF queries execute correctly
- Results match baseline SQLite

### 15.2 Test Script Additions

Add to curriculum test framework:
```python
def test_lyrore_pragmas(conn):
    # Verify pragma exists and works
    conn.execute("PRAGMA lyrore_enabled = ON")
    assert conn.execute("PRAGMA lyrore_enabled").fetchone()[0] == 1
    conn.execute("PRAGMA lyrore_enabled = OFF")
    assert conn.execute("PRAGMA lyrore_enabled").fetchone()[0] == 0

def test_state_persistence(db_path):
    # Verify state tables created
    conn = sqlite3.connect(db_path)
    tables = conn.execute(
        "SELECT name FROM sqlite_master WHERE name LIKE 'lyrore_%'"
    ).fetchall()
    assert len(tables) >= 3  # model_state, exec_history, column_stats
```

### 15.3 Performance Baseline

Record baseline query times before any optimization:
- Run each curriculum query 3x
- Record median execution time
- Store in results.json for comparison with later steps

---

## 16. File Summary

| File | LOC Est. | Purpose |
|------|----------|---------|
| `src/lyrore_model.h` | 150 | Type definitions, macros, declarations |
| `src/lyrore_model.c` | 300 | Model registration, lookup, lifecycle |
| `src/lyrore_features.h` | 50 | Feature extraction declarations |
| `src/lyrore_features.c` | 400 | Feature extraction from WhereInfo/WhereLoop |
| `src/lyrore_stats.h` | 50 | State persistence declarations |
| `src/lyrore_stats.c` | 500 | Init, shutdown, persist, reset, record |
| `src/sqliteInt.h` (mod) | 20 | LyroreContext ptr, flags |
| `src/main.c` (mod) | 20 | Init/shutdown calls |
| `src/pragma.c` (mod) | 50 | PRAGMA handlers |
| `Makefile.in` (mod) | 10 | Build rules |
| Tests | 500 | Unit + integration tests |
| **Total** | **~2050** | |

---

## 17. Verification Checklist

Before marking Step 1 complete:

- [ ] `make` compiles without errors with `-DSQLITE_ENABLE_LYRORE=1`
- [ ] `PRAGMA lyrore_enabled` returns 0/1 correctly
- [ ] `PRAGMA lyrore_persist` and `lyrore_reset` execute without error
- [ ] State tables exist in database after first connection
- [ ] Model registration API works (test with dummy model)
- [ ] Feature extraction runs on simple SELECT (inspect output)
- [ ] State persists: set value, close connection, reopen, verify value
- [ ] Curriculum workload: all queries pass correctness check
- [ ] No memory leaks (valgrind check on curriculum queries)
- [ ] No performance regression > 5% on curriculum queries

### 13.5 Feature Design Philosophy

**Key Principle**: Features are model-driven, not framework-fixed.

The `LyroreFeatures` struct contains ALL extractable features. Each model decides which features to use via its own configuration:

1. **LyroreFeatures**: Dense struct with all possible feature fields (query structure, costs, access patterns, historical). No fixed-size extension array.

2. **Model-specific feature selection**: Each model's `xCreate` receives a config string specifying which features it uses. Model maintains its own feature mask/indices.

3. **lyroreFeaturesToArray**: Takes an optional feature mask parameter. If NULL, outputs canonical order. If provided, outputs only requested features.

```c
/* Feature selection options */
#define LYRORE_FEAT_NTABLES      (1<<0)
#define LYRORE_FEAT_NJOINS       (1<<1)
#define LYRORE_FEAT_NPREDICATES  (1<<2)
/* ... etc ... */
#define LYRORE_FEAT_ALL          0xFFFFFFFF

/* Model can specify its feature set */
int lyroreFeaturesToArrayMasked(LyroreFeatures *pFeat, double *aOut, 
                                 int nMax, u32 featureMask);
```

4. **Default feature set**: Define `LYRORE_DEFAULT_FEATURES` as a compile-time flag with reasonable defaults. Models can override.

This design allows:
- Simple models to use few features
- Complex models to use many features  
- Future features added without breaking existing models
- Per-model feature normalization/scaling
