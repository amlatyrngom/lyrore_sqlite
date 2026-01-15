/* correlation_estimate_plugin.c - Estimate hook with correlation awareness
 * 
 * This plugin demonstrates the estimate hook capability by adjusting
 * cardinality estimates for queries on correlated columns.
 * 
 * WhereLoop offsets (non-debug build):
 *   prereq:   0  (8 bytes)
 *   maskSelf: 8  (8 bytes)
 *   iTab:     16 (1 byte)
 *   iSortIdx: 17 (1 byte)
 *   rSetup:   18 (2 bytes, LogEst)
 *   rRun:     20 (2 bytes, LogEst)
 *   nOut:     22 (2 bytes, LogEst)  <-- We modify this
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Minimal type definitions matching SQLite */
typedef short int LogEst;
typedef long long int i64;
typedef unsigned long long int u64;

/* Forward declare Lyrore types */
typedef struct sqlite3 sqlite3;
typedef struct WhereLoopBuilder WhereLoopBuilder;
typedef struct WhereLoop WhereLoop;

/* Hook types from Lyrore */
typedef struct LyroreEstimateHook {
    const char *zName;
    int priority;
    int (*xMatch)(sqlite3*, WhereLoopBuilder*, WhereLoop*, void*);
    void (*xAdjust)(sqlite3*, WhereLoopBuilder*, WhereLoop*, void*);
    void *pCtx;
} LyroreEstimateHook;

typedef struct LyrorePreOptHook {
    const char *zName;
    int priority;
    int (*xRewrite)(sqlite3*, void*, void*, void*);
    void *pCtx;
} LyrorePreOptHook;

typedef struct LyrorePostQueryHook {
    const char *zName;
    void (*xCollect)(sqlite3*, void*, void*, void*);
    void *pCtx;
} LyrorePostQueryHook;

typedef struct LyrorePluginInfo {
    int version;
    const char *name;
    void *aAnalyzeHooks;          int nAnalyzeHooks;
    LyrorePreOptHook *aPreOptHooks;    int nPreOptHooks;
    LyroreEstimateHook *aEstimateHooks;   int nEstimateHooks;
    LyrorePostQueryHook *aPostQueryHooks; int nPostQueryHooks;
    int (*xInit)(sqlite3 *db, void **ppCtx);
    void (*xShutdown)(void *pCtx);
    void (*xSchemaChange)(sqlite3 *db, void *pCtx);
} LyrorePluginInfo;

/* LogEst conversion (same as SQLite's) */
static u64 myLogEstToInt(LogEst x) {
    u64 n;
    if (x < 10) return (x >= 0) ? 1 : 0;
    n = x % 10;
    x /= 10;
    if (n >= 5) n -= 2;
    else if (n >= 1) n -= 1;
    if (x >= 3) return (n + 8) << (x - 3);
    if (x >= 1) return (n + 8) >> (3 - x);
    return 1;
}

static LogEst myLogEst(u64 x) {
    /* Approximate log2(x) * 10 */
    LogEst y = 0;
    if (x <= 1) return 0;
    while (x >= 8) { y += 10; x >>= 1; }
    /* Remaining bits */
    static const LogEst a[] = {0, 10, 16, 20, 23, 25, 27, 28};
    return y + a[x];
}

/* Context for tracking adjustments */
static struct {
    int nAdjustments;
    i64 lastOriginal;
    i64 lastAdjusted;
    /* For Q-error tracking */
    i64 targetEstimate;  /* Externally provided better estimate */
    int useTarget;
} g_ctx = {0, 0, 0, 0, 0};

/* Access nOut at correct offset */
#define NOUT_OFFSET 22

static LogEst* getNOut(WhereLoop *pLoop) {
    return (LogEst*)((char*)pLoop + NOUT_OFFSET);
}

/* Match all scan operations */
static int matchAllScans(sqlite3 *db, WhereLoopBuilder *pBuilder, WhereLoop *pLoop, void *pCtx) {
    return 1;  /* Match all */
}

/* Adjust estimates based on correlation knowledge
 *
 * For our test data with price*qty correlation:
 * - SQLite estimates independently: assumes uniform distribution of product
 * - Reality: products cluster due to inverse correlation
 *
 * Heuristic: reduce estimates by 40% to account for clustering
 */
static void adjustWithCorrelation(sqlite3 *db, WhereLoopBuilder *pBuilder, 
                                   WhereLoop *pLoop, void *pCtx) {
    LogEst *pNOut = getNOut(pLoop);
    LogEst origLogEst = *pNOut;
    i64 original = myLogEstToInt(origLogEst);
    i64 adjusted;
    
    g_ctx.nAdjustments++;
    g_ctx.lastOriginal = original;
    
    /* Apply correlation-aware adjustment:
     * For correlated columns, the selectivity of product predicates
     * is higher than SQLite's independent assumption.
     * We reduce the estimate by 40% (multiply by 0.6) */
    if (original > 100) {
        adjusted = (original * 60) / 100;
    } else {
        adjusted = original;  /* Don't adjust small estimates */
    }
    
    /* Apply the adjusted estimate */
    LogEst newLogEst = myLogEst(adjusted);
    *pNOut = newLogEst;
    
    g_ctx.lastAdjusted = adjusted;
    
    fprintf(stderr, "[CorrPlugin] Adj #%d: %lld -> %lld (LogEst %d -> %d)\n", 
            g_ctx.nAdjustments, (long long)original, (long long)adjusted,
            (int)origLogEst, (int)newLogEst);
}

/* Post-query hook to log stats */
static void logStats(sqlite3 *db, void *pVdbe, void *pStats, void *pCtx) {
    fprintf(stderr, "[CorrPlugin] Query complete. Total adjustments: %d\n", 
            g_ctx.nAdjustments);
}

/* Plugin hooks */
static LyroreEstimateHook estimateHooks[] = {
    {"correlation_estimate", 100, matchAllScans, adjustWithCorrelation, NULL}
};

static LyrorePostQueryHook postQueryHooks[] = {
    {"correlation_postquery", logStats, NULL}
};

/* Plugin entry point */
__attribute__((visibility("default")))
LyrorePluginInfo* lyrore_plugin_info(void) {
    static LyrorePluginInfo info = {
        1,                    /* version */
        "correlation_plugin", /* name */
        NULL, 0,              /* analyze hooks */
        NULL, 0,              /* pre-opt hooks */
        estimateHooks, 1,     /* estimate hooks */
        postQueryHooks, 1,    /* post-query hooks */
        NULL,                 /* xInit */
        NULL,                 /* xShutdown */
        NULL                  /* xSchemaChange */
    };
    return &info;
}
