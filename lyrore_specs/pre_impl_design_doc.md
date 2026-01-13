# Lyrore SQLite: Pre-Implementation Design Document

## 1. Executive Summary

**Lyrore** generates native C code into SQLite's source tree, creating workload-optimized binaries with embedded ML-ready adaptive logic.

### Critical SQLite Constraints

| Constraint | Impact |
|------------|--------|
| **Tuple-at-a-time VDBE** | Per-tuple branching DESTROYS performance |
| **Nested loops only** | No hash/merge joins |
| **LogEst cost model** | Logarithmic estimates (base-2 × 10) |

**Core Principle**: Standard VDBE uses compile-time adaptivity only. Fused operators (Section 7) enable batch-level runtime adaptivity (~1 branch per 1024 tuples).

### ML-Ready Architecture

All adaptive decisions use **pluggable model interface** (`LyroreModelOps`). Initial implementations are simple heuristics; seamless replacement with ML models is supported.

---

## 2. SQLite Architecture Overview

### 2.1 Query Pipeline
```
SQL → Parser → AST → Planner (where.c) → VDBE Codegen → Execution (vdbe.c)
```

### 2.2 Key Structures
| Structure | Purpose |
|-----------|---------|
| `WhereLoop` | Candidate scan/index path |
| `WhereLevel` | One nested loop level |
| `FuncDef` | Function definition |
| `Expr` | Expression tree node |

### 2.3 LogEst System
```c
typedef INT16_TYPE LogEst;  /* log2(x) * 10 */
```

### 2.4 BTree API Conventions
```c
/* sqlite3BtreeNext() returns:
 *   SQLITE_OK   - success, cursor moved to next row
 *   SQLITE_DONE - no more rows (end of scan)
 *   Other       - error condition
 */
int rc = sqlite3BtreeFirst(pCur, &res);
while(rc == SQLITE_OK && res == 0){
    /* process row */
    rc = sqlite3BtreeNext(pCur, 0);
}
```

---

## 3. ML-Ready Model Framework

### 3.1 Feature Extraction
```c
typedef struct LyroreFeatures {
    /* Query structure */
    int nTables, nJoins, nPredicates, nAggregations;
    int queryDepth, nProjections;
    /* Cost estimates */
    double estTotalRows, estOutputRows, estCost, avgSelectivity;
    /* Access patterns */
    int nIndexScans, nTableScans, hasEquality, hasRange, hasLike;
    /* Historical */
    double lastExecTime, avgExecTime;
    int execCount;
    /* Extension */
    int nExtended;
    double *aExtended;
} LyroreFeatures;

void lyroreExtractPlanFeatures(WhereInfo*, LyroreFeatures*);
void lyroreExtractLoopFeatures(WhereLoop*, Table*, LyroreFeatures*);
int lyroreFeaturesToArray(LyroreFeatures*, double *aOut, int nMax);
```

### 3.2 Pluggable Model Interface
```c
typedef struct LyroreModelOps {
    const char *zName;
    double (*xPredict)(void *pModel, double *aFeat, int nFeat);
    int (*xSelect)(void *pModel, double *aFeat, int nFeat, int nOptions);
    void (*xUpdate)(void *pModel, double *aFeat, int nFeat, double predicted, double actual);
    void (*xReward)(void *pModel, int selected, double reward);
    void *(*xCreate)(sqlite3 *db, const char *zConfig);
    void (*xDestroy)(void *pModel);
    int (*xSave)(void *pModel, sqlite3 *db);
    int (*xLoad)(void *pModel, sqlite3 *db);
} LyroreModelOps;

int lyroreRegisterModel(sqlite3 *db, const char *zPurpose, LyroreModelOps *pOps, const char *zConfig);
LyroreModelOps *lyroreGetModel(sqlite3 *db, const char *zPurpose);
void *lyroreGetModelState(sqlite3 *db, const char *zPurpose);
```

### 3.3 Model Purposes
| Purpose | Decision | Initial Implementation |
|---------|----------|------------------------|
| `"cost"` | Cost estimation | Linear correction factor |
| `"plan"` | Join order selection | Thompson Sampling |
| `"flavor"` | Expression variant | Statistics-based |

---

## 4. Initial Model Implementations

### 4.1 Linear Cost Correction
```c
typedef struct LinearCostState {
    double correction;  /* Multiplicative factor, starts 1.0 */
    int nSamples;
} LinearCostState;

static double linearPredict(void *p, double *aFeat, int n){
    return aFeat[12] * ((LinearCostState*)p)->correction;
}

static void linearUpdate(void *p, double *aFeat, int n, double predicted, double actual){
    LinearCostState *s = (LinearCostState*)p;
    s->correction = 0.9 * s->correction + 0.1 * (actual / (predicted + 1e-6));
    s->nSamples++;
}
```

### 4.2 Thompson Sampling Plan Selector
```c
typedef struct ThompsonState {
    int nArms;
    double *aAlpha, *aBeta;
} ThompsonState;

static int thompsonSelect(void *p, double *aFeat, int n, int nOpts){
    ThompsonState *s = (ThompsonState*)p;
    int best = 0; double bestSample = 0;
    for(int i = 0; i < nOpts; i++){
        double sample = sampleBeta(s->aAlpha[i], s->aBeta[i]);
        if(sample > bestSample){ bestSample = sample; best = i; }
    }
    return best;
}

static void thompsonReward(void *p, int sel, double reward){
    ThompsonState *s = (ThompsonState*)p;
    if(reward > 0.5) s->aAlpha[sel] += 1.0;
    else s->aBeta[sel] += 1.0;
}
```

### 4.3 Statistics-Based Flavor Selector
```c
static int staticFlavorSelect(void *p, double *aFeat, int n, int nOpts){
    double sel = aFeat[13];  /* avgSelectivity */
    return (sel < 0.05 || sel > 0.95) ? 0 : 1;  /* BRANCHING vs BRANCHLESS */
}
```

---

## 5. Plan-Level Optimizations

### 5.1 Join Order Selection

**Hook Point**: Inside `wherePathSolver()` DURING iteration, save top N candidates before final selection:

```c
static int wherePathSolver(WhereInfo *pWInfo, LogEst nRowEst){
    /* ... standard path computation ... */

    /* LYRORE: Save top candidates DURING solver, not at end */
    if(db->flags & SQLITE_LyrorePlanRL && nTo > 1){
        int nSave = MIN(nTo, 4);
        memcpy(pWInfo->aLyroreCandidates, aTo, nSave * sizeof(WherePath));
        pWInfo->nLyroreCandidates = nSave;
    }

    /* ... continue with standard selection ... */
}

/* After solver completes, apply RL selection */
void lyroreSelectPlan(WhereInfo *pWInfo){
    if(pWInfo->nLyroreCandidates < 2) return;

    LyroreFeatures feat;
    lyroreExtractPlanFeatures(pWInfo, &feat);
    double aFeat[64];
    int nFeat = lyroreFeaturesToArray(&feat, aFeat, 64);

    LyroreModelOps *ops = lyroreGetModel(db, "plan");
    int sel = ops->xSelect(lyroreGetModelState(db, "plan"), aFeat, nFeat, 
                           pWInfo->nLyroreCandidates);

    if(sel != 0){
        WherePath tmp = pWInfo->aLyroreCandidates[0];
        pWInfo->aLyroreCandidates[0] = pWInfo->aLyroreCandidates[sel];
        pWInfo->aLyroreCandidates[sel] = tmp;
    }
    pWInfo->iLyrorePlan = sel;
}
```

### 5.2 Cost Estimation Hook
```c
/* In whereLoopAddBtree() */
if(db->flags & SQLITE_LyroreCost){
    LyroreFeatures feat;
    lyroreExtractLoopFeatures(pNew, pSrc->pTab, &feat);
    double aFeat[64];
    int nFeat = lyroreFeaturesToArray(&feat, aFeat, 64);

    double cost = lyroreGetModel(db, "cost")->xPredict(
        lyroreGetModelState(db, "cost"), aFeat, nFeat);
    pNew->rRun = sqlite3LogEst((u64)cost);
}
```

### 5.3 Reward Collection
```c
void lyroreRecordOutcome(WhereInfo *pWInfo, i64 execTime){
    sqlite3 *db = pWInfo->pParse->db;

    if(db->flags & SQLITE_LyrorePlanRL){
        double reward = 1.0 / (1.0 + execTime / 1000000.0);
        lyroreGetModel(db, "plan")->xReward(
            lyroreGetModelState(db, "plan"), pWInfo->iLyrorePlan, reward);
    }

    if(db->flags & SQLITE_LyroreCost){
        /* Update cost model with actual vs predicted */
        LyroreFeatures feat;
        lyroreExtractLoopFeatures(pWInfo->a[0].pWLoop, NULL, &feat);
        double aFeat[64];
        int nFeat = lyroreFeaturesToArray(&feat, aFeat, 64);
        lyroreGetModel(db, "cost")->xUpdate(
            lyroreGetModelState(db, "cost"), aFeat, nFeat, 
            feat.estCost, (double)execTime);
    }
}
```

---

## 6. Expression-Level Optimizations

### 6.1 Why Per-Tuple Branching is Harmful
```c
/* HARMFUL - ~15 cycle misprediction per tuple */
for each row:
    if(flavor == 0) result = f0(row);
    else result = f1(row);
```

**Solution**: Select flavor ONCE at compile time. For runtime adaptivity, use fused operators with batch processing (Section 7).

### 6.2 Compile-Time Flavor Selection
```c
void sqlite3ExprCode(Parse *pParse, Expr *pExpr, int target){
    sqlite3 *db = pParse->db;
    if(lyroreHasVariants(pExpr) && (db->flags & SQLITE_LyroreFlavor)){
        LyroreFeatures feat;
        lyroreExtractExprFeatures(pParse, pExpr, &feat);
        double aFeat[64];
        int nFeat = lyroreFeaturesToArray(&feat, aFeat, 64);

        int flavor = lyroreGetModel(db, "flavor")->xSelect(
            lyroreGetModelState(db, "flavor"), aFeat, nFeat, 2);
        lyroreCodeExprFlavor(pParse, pExpr, target, flavor);
        return;
    }
    sqlite3ExprCodeInternal(pParse, pExpr, target);
}
```

### 6.3 Expression Fusion
```c
/* Before: 4 opcodes. After: 1 fused opcode */
case OP_LyroreFusedAddMul: {
    double a = sqlite3VdbeRealValue(&aMem[pOp->p1]);
    double b = sqlite3VdbeRealValue(&aMem[pOp->p2]);
    double c = sqlite3VdbeRealValue(&aMem[pOp->p3]);
    pOut->u.r = (a + b) * c;
    pOut->flags = MEM_Real;
    break;
}
```
---

## 7. Fused Operators

> **Note**: The examples below illustrate specific optimization patterns (e.g., array join for small tables, filter-aggregate fusion). These patterns are meant to be **generalized by the implementation agent** to cover similar cases.

### 7.1 Schema Invalidation and Operator Classification

Fused operators cache schema info. SQLite tracks schema changes at **database granularity**, not per-table. Any schema change (CREATE/DROP/ALTER on any table) increments the database's `schema_cookie` and triggers `iGeneration` increment on schema reload.

**Operator Classification by Schema Dependency**:

| Flag | Description | Schema Check | Examples |
|------|-------------|--------------|----------|
| `LYRORE_OP_SCHEMA_SAFE` | General optimizations, no schema assumptions | Bypass check | Expression fusion, arithmetic opts |
| `LYRORE_OP_SCHEMA_DEPENDENT` | Caches column indices, types, or table structure | **Require dual-check** | Array joins, column extractors |

```c
typedef struct LyroreFusedPlan {
    u32 schemaVersion;      /* schema_cookie when plan created */
    int schemaGeneration;   /* iGeneration when plan created */
    int iDb;                /* Database index */
    u8 flags;               /* LYRORE_OP_SCHEMA_SAFE or LYRORE_OP_SCHEMA_DEPENDENT */
    /* ... operator-specific fields ... */
} LyroreFusedPlan;

#define LYRORE_OP_SCHEMA_SAFE       0x01
#define LYRORE_OP_SCHEMA_DEPENDENT  0x02

static int lyrorePlanValid(sqlite3 *db, LyroreFusedPlan *plan){
    /* Schema-safe operators bypass check entirely */
    if(plan->flags & LYRORE_OP_SCHEMA_SAFE) return 1;

    /* Schema-dependent operators require dual-check */
    Db *pDb = &db->aDb[plan->iDb];
    return (plan->schemaVersion == pDb->pSchema->schema_cookie &&
            plan->schemaGeneration == pDb->pSchema->iGeneration);
}
```

**Why Database-Level Granularity is Acceptable**: Schema-safe operators are unaffected by any schema changes. Schema-dependent operators are invalidated on ANY schema change (even unrelated tables), which is conservative but safe. Production workloads rarely have frequent DDL, so re-preparation cost is acceptable.

### 7.2 Error Handling Pattern

All Lyrore opcodes must follow SQLite's error handling:
```c
case OP_LyroreFusedXxx: {
    LyroreFusedPlan *plan = pOp->p4.pFusedPlan;

    /* Schema check (skipped for SCHEMA_SAFE ops) */
    if(!lyrorePlanValid(db, plan)){
        rc = SQLITE_SCHEMA;
        goto abort_due_to_error;
    }

    /* ... operation ... */

    if(rc != SQLITE_OK && rc != SQLITE_DONE){
        goto abort_due_to_error;
    }
    break;
}
```

### 7.3 Small-Table Array Join (SCHEMA_DEPENDENT)
```c
case OP_LyroreArrayBuild: {
    LyroreFusedPlan *plan = pOp->p4.pFusedPlan;  /* flags = SCHEMA_DEPENDENT */
    if(!lyrorePlanValid(db, plan)){ rc = SQLITE_SCHEMA; goto abort_due_to_error; }

    i64 *lookup = sqlite3DbMallocZero(db, pOp->p4.i * sizeof(i64));
    if(!lookup){ rc = SQLITE_NOMEM; goto abort_due_to_error; }

    int rc = sqlite3BtreeFirst(pCur, &res);
    while(rc == SQLITE_OK && res == 0){
        int key = sqlite3VdbeIntValue(pCur, pOp->p3);
        lookup[key] = sqlite3BtreeRowid(pCur) + 1;
        rc = sqlite3BtreeNext(pCur, 0);
    }
    if(rc != SQLITE_OK && rc != SQLITE_DONE){ goto abort_due_to_error; }

    aMem[pOp->p2].z = (char*)lookup;
    break;
}
```

### 7.4 Batch-Level Micro-Adaptivity (SCHEMA_SAFE)

**vw-greedy Selector** - parameters are tunable starting points:

```c
typedef struct LyroreMicroAdaptState {
    int nFlavors, bestFlavor, callCount;
    i64 *aCycles, *aTuples;
    int explorePeriod;    /* default: 8 batches */
    int exploreBatches;   /* default: 2 per flavor */
} LyroreMicroAdaptState;

int lyroreMicroAdaptSelect(LyroreMicroAdaptState *p){
    int pos = p->callCount % p->explorePeriod;
    int exploreLen = p->exploreBatches * p->nFlavors;
    if(pos < exploreLen) return (pos / p->exploreBatches) % p->nFlavors;
    return p->bestFlavor;
}

void lyroreMicroAdaptRecord(LyroreMicroAdaptState *p, int flavor, i64 cycles, i64 tuples){
    p->aCycles[flavor] += cycles;
    p->aTuples[flavor] += tuples;
    if(++p->callCount % p->explorePeriod == p->exploreBatches * p->nFlavors){
        double bestCPT = 1e30;
        for(int f = 0; f < p->nFlavors; f++){
            if(p->aTuples[f] > 0){
                double cpt = (double)p->aCycles[f] / p->aTuples[f];
                if(cpt < bestCPT){ bestCPT = cpt; p->bestFlavor = f; }
            }
            p->aCycles[f] = p->aTuples[f] = 0;
        }
    }
}
```

### 7.5 Adaptive Fused Operator Example (SCHEMA_DEPENDENT)
```c
typedef i64 (*SumWhereFn)(int n, i64 *f, i64 *v, i64 t);
static SumWhereFn sumFlavors[] = { sumBranch, sumNoBranch };

case OP_LyroreFusedSumWhereAdaptive: {
    LyroreFusedPlan *plan = pOp->p4.pFusedPlan;  /* flags = SCHEMA_DEPENDENT */
    if(!lyrorePlanValid(db, plan)){ rc = SQLITE_SCHEMA; goto abort_due_to_error; }

    LyroreMicroAdaptState *adapt = plan->pMicroAdapt;
    i64 sum = 0, filterBuf[1024], valBuf[1024];

    int rc = sqlite3BtreeFirst(pCur, &res);
    while(rc == SQLITE_OK && res == 0){
        int n = 0;
        while(rc == SQLITE_OK && res == 0 && n < 1024){
            filterBuf[n] = sqlite3VdbeGetInt(pCur, plan->filterCol);
            valBuf[n++] = sqlite3VdbeGetInt(pCur, plan->sumCol);
            rc = sqlite3BtreeNext(pCur, &res);
        }
        int flavor = lyroreMicroAdaptSelect(adapt);
        i64 t0 = sqlite3Hwtime();
        sum += sumFlavors[flavor](n, filterBuf, valBuf, plan->filterVal);
        lyroreMicroAdaptRecord(adapt, flavor, sqlite3Hwtime() - t0, n);
    }
    if(rc != SQLITE_OK && rc != SQLITE_DONE){ goto abort_due_to_error; }

    aMem[pOp->p2].u.i = sum;
    break;
}

/* Flavor implementations */
i64 sumBranch(int n, i64 *f, i64 *v, i64 t){
    i64 s=0; for(int i=0;i<n;i++) if(f[i]>t) s+=v[i]; return s;
}
i64 sumNoBranch(int n, i64 *f, i64 *v, i64 t){
    i64 s=0; for(int i=0;i<n;i++) s+=v[i]*(f[i]>t); return s;
}
```

### 7.6 Fixed-Format Pipeline Records (SCHEMA_SAFE)

For ephemeral tables (joins, subqueries), encode all numerics as 8 bytes for O(1) access. These are inherently schema-safe since ephemeral tables have no persistent schema.

```c
/* VdbeCursor extension */
Bool lyroreFixedFormat:1;
u8 lyroreNumCols;

/* O(1) column access for fixed-format records */
if(pC->lyroreFixedFormat){
    u32 offset = (pC->lyroreNumCols + 1) + (p2 * 8);
    const u8 *zData = pC->aRow + offset;
    u8 serial_type = pC->aRow[1 + p2];

    if(serial_type == 6){
        pDest->u.i = LYRORE_EIGHT_BYTE_INT(zData);
        pDest->flags = MEM_Int;
    }else{
        memcpy(&pDest->u.r, zData, 8);
        pDest->flags = MEM_Real;
    }
    goto op_column_out;
}
/* Fallback: standard OP_Column handling */
goto op_column_standard;
```

### 7.7 Schema-Aware Column Extraction (SCHEMA_DEPENDENT)

Standard `OP_Column` has per-tuple overhead. For fused operators with known schemas, we use **parse-once extraction**.

**Critical Constraint**: SQLite INTEGER columns use variable-length encoding. Fixed offsets are NOT possible for INTEGER columns. Only REAL NOT NULL columns have fixed 8-byte storage.

```c
/* Schema: t(id INTEGER, score REAL NOT NULL, flags INTEGER) */
static inline void lyroreExtractRow(const u8 *rec, i64 *id, double *score, i64 *flags){
    int hdrSz = rec[0];
    int off = hdrSz;
    /* Parse header once, extract with type-specific handlers */
}
```

**Schema Safety**: Column indices are captured at prepare time. Schema changes invalidate via the dual-check, triggering re-preparation which re-resolves column names to current indices.
---

## 8. UDF Handling

### 8.1 Scalar UDF Transpilation
```c
static void lyrore_extract_year(sqlite3_context *ctx, int argc, sqlite3_value **argv){
    if(sqlite3_value_type(argv[0]) == SQLITE_NULL){
        sqlite3_result_null(ctx);
        return;
    }
    const char *s = (const char*)sqlite3_value_text(argv[0]);
    int year = 0;
    while(*s && *s != '-') year = year * 10 + (*s++ - '0');
    sqlite3_result_int(ctx, year);
}
```

### 8.2 Aggregate UDF
```c
typedef struct MedianCtx { double *values; int count, cap; } MedianCtx;

static void median_step(sqlite3_context *ctx, int argc, sqlite3_value **argv){
    MedianCtx *p = sqlite3_aggregate_context(ctx, sizeof(*p));
    if(sqlite3_value_type(argv[0]) == SQLITE_NULL) return;
    if(p->count >= p->cap){
        p->cap = p->cap ? p->cap*2 : 16;
        p->values = sqlite3_realloc(p->values, p->cap*sizeof(double));
    }
    p->values[p->count++] = sqlite3_value_double(argv[0]);
}

static void median_final(sqlite3_context *ctx){
    MedianCtx *p = sqlite3_aggregate_context(ctx, 0);
    if(!p || !p->count){ sqlite3_result_null(ctx); return; }
    qsort(p->values, p->count, sizeof(double), cmp);
    int n = p->count;
    double r = (n%2) ? p->values[n/2] : (p->values[n/2-1]+p->values[n/2])/2;
    sqlite3_result_double(ctx, r);
    sqlite3_free(p->values);
}
```

### 8.3 UDF Strategy
| UDF Type | Strategy |
|----------|----------|
| Pure computation | OUTLINE (transpile to C) |
| Pure SQL (SELECTs) | INLINE (embed in plan) |
| Mixed | HYBRID |

---

## 9. State Persistence

### 9.1 Dedicated State Connection (Critical)

**Must use separate connection** to avoid transaction conflicts with user queries:

```c
typedef struct LyroreContext {
    sqlite3 *pStateDb;      /* Dedicated connection for state persistence */
    int dirty;              /* Queries since last persist */
    LyroreModelEntry *aModels;
    int nModels;
} LyroreContext;

int lyroreInit(sqlite3 *db){
    LyroreContext *ctx = sqlite3MallocZero(sizeof(*ctx));

    /* Open dedicated state connection to same database */
    const char *zPath = sqlite3_db_filename(db, "main");
    int rc = sqlite3_open_v2(zPath, &ctx->pStateDb, 
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
    if(rc != SQLITE_OK) return rc;

    db->pLyrore = ctx;
    return lyroreLoadState(db);
}

void lyroreMaybePersist(sqlite3 *db){
    LyroreContext *ctx = db->pLyrore;
    if(ctx->dirty < 100) return;

    /* Use dedicated connection - safe regardless of user transaction state */
    sqlite3 *stateDb = ctx->pStateDb;
    sqlite3_exec(stateDb, "BEGIN IMMEDIATE", 0, 0, 0);
    for(int i = 0; i < ctx->nModels; i++){
        if(ctx->aModels[i].pOps->xSave)
            ctx->aModels[i].pOps->xSave(ctx->aModels[i].pState, stateDb);
    }
    sqlite3_exec(stateDb, "COMMIT", 0, 0, 0);
    ctx->dirty = 0;
}
```

### 9.2 State Tables
```sql
CREATE TABLE lyrore_column_stats(
    tbl TEXT, col TEXT,
    distinct_count INT, null_count INT, total_count INT,
    min_value BLOB, max_value BLOB, histogram_bounds BLOB,
    PRIMARY KEY(tbl, col)
);

CREATE TABLE lyrore_model_state(
    purpose TEXT, context_key TEXT,
    state_blob BLOB, sample_count INT, last_updated INT,
    PRIMARY KEY(purpose, context_key)
);

CREATE TABLE lyrore_exec_history(
    pattern_sig TEXT PRIMARY KEY,
    exec_count INT, total_time_us INT, avg_rows REAL
);
```

---

## 10. Configuration

### 10.1 PRAGMAs
```sql
PRAGMA lyrore_enabled = ON|OFF;
PRAGMA lyrore_plan_rl = ON|OFF;
PRAGMA lyrore_cost = ON|OFF;
PRAGMA lyrore_flavor = ON|OFF;
PRAGMA lyrore_fusion = ON|OFF;
PRAGMA lyrore_persist;
PRAGMA lyrore_reset;
```

### 10.2 Compile Flags
```makefile
LYRORE_FLAGS = -DLYRORE_ENABLED=1 -DSQLITE_ENABLE_STAT4=1
```

---

## 11. Implementation Files

| File | Purpose |
|------|---------|
| `lyrore_model.c/h` | Model interface, registration |
| `lyrore_features.c/h` | Feature extraction |
| `lyrore_stats.c/h` | Statistics, state persistence |
| `lyrore_plan.c` | Plan selection integration |
| `lyrore_cost.c` | Cost estimation integration |
| `lyrore_expr.c` | Flavor selection, fusion |
| `lyrore_udf.c/h` | Transpiled UDFs |
| `lyrore_opcodes.c` | Fused opcodes |
| `lyrore_colstore.c/h` | Columnar storage (Section 13) |

**Modified files**: `vdbe.c`, `where.c`, `wherecode.c`, `expr.c`, `func.c`, `main.c`, `sqliteInt.h`, `Makefile.in`
---

## 13. On-Demand Vectorized Table Scans

### 13.1 Architecture Overview

Columnar storage in same DB file via shadow tables. AFTER triggers for ACID sync. SQL functions for API.

### 13.2 Shadow Table Schema

```sql
CREATE TABLE lyrore_colstore_config (
    table_id INTEGER PRIMARY KEY,
    table_name TEXT UNIQUE NOT NULL,
    chunk_size INTEGER DEFAULT 1024,
    schema_version INTEGER NOT NULL,   -- schema_cookie at vectorization time
    column_hash TEXT NOT NULL,         -- hash of vectorized column names+types
    created_at TEXT DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE lyrore_colstore_columns (
    table_id INTEGER NOT NULL, column_name TEXT NOT NULL,
    column_idx INTEGER NOT NULL, data_type INTEGER NOT NULL,
    PRIMARY KEY (table_id, column_name)) WITHOUT ROWID;

CREATE TABLE lyrore_colstore_data (
    table_id INTEGER NOT NULL, column_idx INTEGER NOT NULL, chunk_id INTEGER NOT NULL,
    lo_rowid INTEGER NOT NULL, hi_rowid INTEGER NOT NULL, row_count INTEGER NOT NULL,
    null_bitmap BLOB, delete_bitmap BLOB, data BLOB NOT NULL,
    PRIMARY KEY (table_id, column_idx, chunk_id)) WITHOUT ROWID;
```

### 13.3 API Functions

```sql
SELECT lyrore_vectorize('table', 'col1,col2');  -- '*' for all
SELECT lyrore_devectorize('table', 'col1');     -- '*' for all
SELECT lyrore_status('table');                  -- JSON status
```

### 13.4 AFTER Trigger Sync

AFTER triggers execute within the SAME transaction as the triggering statement, ensuring ACID compliance. On ROLLBACK, all shadow table modifications are automatically rolled back.

```sql
CREATE TRIGGER lyrore_<table>_ai AFTER INSERT ON <table>
WHEN (SELECT 1 FROM lyrore_colstore_config WHERE table_name='<table>')
BEGIN
    SELECT lyrore_colstore_sync_insert('<table>', NEW.rowid, NEW.col1, ...);
END;

CREATE TRIGGER lyrore_<table>_ad AFTER DELETE ON <table>
WHEN (SELECT 1 FROM lyrore_colstore_config WHERE table_name='<table>')
BEGIN
    SELECT lyrore_colstore_sync_delete('<table>', OLD.rowid);
END;

-- UPDATE: OLD.rowid for delete, NEW.rowid for insert (handles rowid changes)
CREATE TRIGGER lyrore_<table>_au AFTER UPDATE ON <table>
WHEN (SELECT 1 FROM lyrore_colstore_config WHERE table_name='<table>')
BEGIN
    SELECT lyrore_colstore_sync_delete('<table>', OLD.rowid);
    SELECT lyrore_colstore_sync_insert('<table>', NEW.rowid, NEW.col1, ...);
END;
```

**Note**: `OLD.rowid == NEW.rowid` for most UPDATEs. The delete+insert pattern handles both cases correctly.

### 13.5 Recursive Trigger Protection

Shadow tables must NOT trigger Lyrore sync. The sync functions include a guard:

```c
static void lyrore_colstore_sync_insert(sqlite3_context *ctx, int argc, sqlite3_value **argv){
    const char *table = (const char*)sqlite3_value_text(argv[0]);
    if(strncmp(table, "lyrore_", 7) == 0) return;  /* Prevent recursion */
    /* ... sync logic ... */
}
```

### 13.6 Schema Change Interception

Schema changes to vectorized tables require **validation-on-access** with **automatic cleanup**.

#### 13.6.1 Schema Validation on Access

```c
static int lyrore_colstore_validate_schema(sqlite3 *db, const char *table){
    int stored_version = lyrore_get_stored_schema_version(db, table);
    int current_version = db->aDb[0].pSchema->schema_cookie;

    if(stored_version != current_version){
        if(!lyrore_verify_columns_exist(db, table)){
            lyrore_disable_colstore(db, table);  /* Columns dropped/altered */
            return SQLITE_SCHEMA;
        }
        lyrore_update_stored_schema_version(db, table, current_version);
    }
    return SQLITE_OK;
}
```

#### 13.6.2 DROP TABLE Handling

Triggers are automatically removed. Orphaned shadow data cleaned lazily:

```c
static void lyrore_cleanup_orphaned_data(sqlite3 *db){
    sqlite3_exec(db,
        "DELETE FROM lyrore_colstore_data WHERE table_id IN "
        "(SELECT table_id FROM lyrore_colstore_config c "
        " WHERE NOT EXISTS (SELECT 1 FROM sqlite_master "
        "   WHERE type='table' AND name=c.table_name));"
        /* Similar for lyrore_colstore_columns and lyrore_colstore_config */,
        0, 0, 0);
}
```

#### 13.6.3 ADD/DROP/RENAME COLUMN Handling

```c
static int lyrore_check_column_changes(sqlite3 *db, const char *table){
    /* Compare current vs stored column list */
    /* If columns added: mark stale, triggers miss new columns */
    /* If columns dropped: devectorize missing, recreate triggers */
    /* If columns renamed: treated as drop+add */

    if(/* column mismatch detected */){
        lyrore_devectorize_missing_columns(db, table);
        lyrore_recreate_triggers(db, table);
    }
    return rc;
}
```

### 13.7 Query Routing

#### 13.7.1 When to Use Columnar Scan

- Full/wide range scans (not point lookups)
- Query accesses only vectorized columns
- Analytical aggregations on large tables

#### 13.7.2 Optimizer Integration

```c
/* Add columnar scan as WhereLoop option */
if(lyrore_table_has_colstore(pTab) && lyrore_query_suits_colstore(pBuilder)){
    WhereLoop *pNew = pBuilder->pNew;
    pNew->wsFlags |= WHERE_LYRORE_COLSTORE;
    pNew->rRun = sqlite3LogEst(nChunks * CHUNK_IO_COST);
    whereLoopInsert(pBuilder, pNew);
}
```

#### 13.7.3 Mixed-Column Queries

For queries needing both vectorized and non-vectorized columns, fall back to row storage (future: hybrid approach).

#### 13.7.4 Columnar Scan Execution

```c
case OP_LyroreColstoreScan: {
    LyroreColstorePlan *plan = pOp->p4.pColstorePlan;
    if(lyrore_colstore_validate_schema(db, plan->table) != SQLITE_OK){
        rc = SQLITE_SCHEMA; goto abort_due_to_error;
    }
    /* Iterate chunks, apply delete_bitmap, process in batch */
    break;
}
```

### 13.8 Implementation Files

**New**: `src/lyrore_colstore.c/h` | **Modify**: `src/main.c`, `src/where.c`, `Makefile.in`
---

## 14. Critical Correctness Guarantees

### 14.1 Format Preservation
- **All on-disk formats unchanged** - Lyrore uses standard SQLite format
- Fixed-format records are ephemeral only (DELETEONCLOSE tables)
- State tables use standard SQLite format
- Shadow tables for columnar storage use standard SQLite tables

### 14.2 Schema Safety

**Dual-Check Mechanism**: Check BOTH `schema_cookie` AND `iGeneration` before any schema-dependent fused operation:
```c
if(plan->schemaVersion != pDb->pSchema->schema_cookie ||
   plan->schemaGeneration != pDb->pSchema->iGeneration){
    rc = SQLITE_SCHEMA;
    goto abort_due_to_error;
}
```

**Invalidation Granularity**: Schema changes to ANY table in a database invalidate ALL schema-dependent Lyrore optimizations for that database. This mirrors SQLite's own behavior. Schema-safe operators (flagged `LYRORE_OP_SCHEMA_SAFE`) are unaffected.

**Column Index Lifecycle**: Column indices (e.g., `plan->filterCol`) are resolved from column names at prepare time. Schema changes trigger `SQLITE_SCHEMA`, causing re-preparation with fresh index resolution.

### 14.3 Transaction Safety  
- Dedicated state connection prevents conflicts with user transactions
- State persistence is independent of user COMMIT/ROLLBACK
- No nested transaction assumptions
- AFTER triggers for columnar sync execute in same transaction (automatic rollback on failure)

### 14.4 Error Handling
- All Lyrore opcodes use `goto abort_due_to_error` on failure
- Proper cleanup on `SQLITE_NOMEM` and other errors
- Fallback to standard execution on any Lyrore-specific error
- Schema validation returns `SQLITE_SCHEMA` to trigger re-prepare

### 14.5 Columnar Storage Safety
- Shadow table modifications are atomic with main table (AFTER triggers)
- Recursive trigger protection prevents infinite loops
- Schema change detection prevents stale columnar data access
- Orphaned shadow data cleaned lazily (no corruption, just wasted space until cleanup)

### 14.6 Future ML Integration
- Data skew features (planned)
- Temporal pattern detection (planned)
- Feature interface is extensible via `aExtended` array

---

*All adaptive decisions use the pluggable `LyroreModelOps` interface. Initial implementations are simple heuristics; seamless replacement with ML models is supported.*
