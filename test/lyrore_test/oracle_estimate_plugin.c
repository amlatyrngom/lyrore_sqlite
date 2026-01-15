/* oracle_estimate_plugin.c - Oracle plugin for E2E testing
 *
 * This plugin demonstrates that estimate hooks CAN improve estimation
 * by using an "oracle" approach - we tell it the correct answer and
 * it applies that as the estimate.
 *
 * In production, this would be replaced by learned models that predict
 * better estimates from historical data.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef short int LogEst;
typedef long long int i64;
typedef unsigned long long int u64;

typedef struct sqlite3 sqlite3;
typedef struct WhereLoopBuilder WhereLoopBuilder;
typedef struct WhereLoop WhereLoop;

typedef struct LyroreEstimateHook {
    const char *zName;
    int priority;
    int (*xMatch)(sqlite3*, WhereLoopBuilder*, WhereLoop*, void*);
    void (*xAdjust)(sqlite3*, WhereLoopBuilder*, WhereLoop*, void*);
    void *pCtx;
} LyroreEstimateHook;

typedef struct LyrorePostQueryHook {
    const char *zName;
    void (*xCollect)(sqlite3*, void*, void*, void*);
    void *pCtx;
} LyrorePostQueryHook;

typedef struct LyrorePluginInfo {
    int version;
    const char *name;
    void *aAnalyzeHooks;                 int nAnalyzeHooks;
    void *aPreOptHooks;                  int nPreOptHooks;
    LyroreEstimateHook *aEstimateHooks;  int nEstimateHooks;
    LyrorePostQueryHook *aPostQueryHooks; int nPostQueryHooks;
    int (*xInit)(sqlite3 *db, void **ppCtx);
    void (*xShutdown)(void *pCtx);
    void (*xSchemaChange)(sqlite3 *db, void *pCtx);
} LyrorePluginInfo;

/* LogEst conversions */
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
    LogEst y = 0;
    if (x <= 1) return 0;
    while (x >= 8) { y += 10; x >>= 1; }
    static const LogEst a[] = {0, 10, 16, 20, 23, 25, 27, 28};
    return y + a[x];
}

#define NOUT_OFFSET 22
static LogEst* getNOut(WhereLoop *pLoop) {
    return (LogEst*)((char*)pLoop + NOUT_OFFSET);
}

/* Oracle context - set the target estimate externally */
static struct {
    i64 targetEstimate;     /* What we want to set */
    int useTarget;          /* Whether to apply target */
    int nAdjustments;
    i64 lastOriginal;
    i64 lastAdjusted;
    double qErrorBefore;    /* Q-error with original */
    double qErrorAfter;     /* Q-error with adjusted */
    i64 actualRows;         /* Ground truth for Q-error calc */
} g_oracle = {0, 0, 0, 0, 0, 0.0, 0.0, 0};

/* Calculate Q-error */
static double calcQError(i64 est, i64 actual) {
    if (est <= 0 || actual <= 0) return 0.0;
    double ratio = (double)est / (double)actual;
    return ratio > 1.0 ? ratio : 1.0 / ratio;
}

/* Environment variable reader for oracle values */
__attribute__((constructor))
static void initOracle(void) {
    const char *target = getenv("LYRORE_ORACLE_ESTIMATE");
    const char *actual = getenv("LYRORE_ORACLE_ACTUAL");
    
    if (target) {
        g_oracle.targetEstimate = atoll(target);
        g_oracle.useTarget = 1;
        fprintf(stderr, "[Oracle] Target estimate set to: %lld\n", 
                (long long)g_oracle.targetEstimate);
    }
    if (actual) {
        g_oracle.actualRows = atoll(actual);
        fprintf(stderr, "[Oracle] Actual rows for Q-error: %lld\n", 
                (long long)g_oracle.actualRows);
    }
}

static int matchAll(sqlite3 *db, WhereLoopBuilder *pBuilder, WhereLoop *pLoop, void *pCtx) {
    return 1;
}

static void adjustWithOracle(sqlite3 *db, WhereLoopBuilder *pBuilder, 
                             WhereLoop *pLoop, void *pCtx) {
    LogEst *pNOut = getNOut(pLoop);
    LogEst origLogEst = *pNOut;
    i64 original = myLogEstToInt(origLogEst);
    i64 adjusted;
    
    g_oracle.nAdjustments++;
    g_oracle.lastOriginal = original;
    
    if (g_oracle.useTarget && g_oracle.targetEstimate > 0) {
        /* Apply the oracle estimate */
        adjusted = g_oracle.targetEstimate;
        *pNOut = myLogEst(adjusted);
    } else {
        adjusted = original;
    }
    
    g_oracle.lastAdjusted = adjusted;
    
    /* Calculate Q-errors if actual is known */
    if (g_oracle.actualRows > 0) {
        g_oracle.qErrorBefore = calcQError(original, g_oracle.actualRows);
        g_oracle.qErrorAfter = calcQError(adjusted, g_oracle.actualRows);
        
        fprintf(stderr, "[Oracle] Adj #%d: %lld -> %lld | Q-error: %.3f -> %.3f\n",
                g_oracle.nAdjustments, (long long)original, (long long)adjusted,
                g_oracle.qErrorBefore, g_oracle.qErrorAfter);
    } else {
        fprintf(stderr, "[Oracle] Adj #%d: %lld -> %lld (LogEst %d -> %d)\n",
                g_oracle.nAdjustments, (long long)original, (long long)adjusted,
                (int)origLogEst, (int)myLogEst(adjusted));
    }
}

static void logFinalStats(sqlite3 *db, void *pVdbe, void *pStats, void *pCtx) {
    fprintf(stderr, "[Oracle] Query complete. Adjustments: %d\n", g_oracle.nAdjustments);
    if (g_oracle.actualRows > 0 && g_oracle.nAdjustments > 0) {
        fprintf(stderr, "[Oracle] Final Q-error improvement: %.3f -> %.3f (%.1f%% better)\n",
                g_oracle.qErrorBefore, g_oracle.qErrorAfter,
                (g_oracle.qErrorBefore > g_oracle.qErrorAfter) ?
                    ((g_oracle.qErrorBefore - g_oracle.qErrorAfter) / g_oracle.qErrorBefore * 100.0) :
                    (-(g_oracle.qErrorAfter - g_oracle.qErrorBefore) / g_oracle.qErrorBefore * 100.0));
    }
}

static LyroreEstimateHook estimateHooks[] = {
    {"oracle_estimate", 100, matchAll, adjustWithOracle, NULL}
};

static LyrorePostQueryHook postQueryHooks[] = {
    {"oracle_postquery", logFinalStats, NULL}
};

__attribute__((visibility("default")))
LyrorePluginInfo* lyrore_plugin_info(void) {
    static LyrorePluginInfo info = {
        1, "oracle_plugin",
        NULL, 0,  /* analyze */
        NULL, 0,  /* pre-opt */
        estimateHooks, 1,
        postQueryHooks, 1,
        NULL, NULL, NULL
    };
    return &info;
}
