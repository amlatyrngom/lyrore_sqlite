/* histogram_plugin.c - Multi-column histogram for cardinality estimation
 *
 * DEMONSTRATES: Using ANALYZE hook to build histogram, then estimate hook to 
 * provide better cardinality estimates for correlated columns.
 * 
 * Uses environment variables for test harness:
 *   HIST_THRESHOLD - threshold for price*qty < threshold predicate
 *   HIST_ACTUAL - actual row count (for Q-error calculation)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <dlfcn.h>

typedef short int LogEst;
typedef long long int i64;
typedef unsigned long long int u64;
typedef struct sqlite3 sqlite3;
typedef struct sqlite3_stmt sqlite3_stmt;
typedef struct WhereLoopBuilder WhereLoopBuilder;
typedef struct WhereLoop WhereLoop;

#define SQLITE_OK 0
#define SQLITE_ROW 100

/* SQLite API function pointers */
static int (*p_prepare)(sqlite3*, const char*, int, sqlite3_stmt**, const char**) = NULL;
static int (*p_step)(sqlite3_stmt*) = NULL;
static int (*p_finalize)(sqlite3_stmt*) = NULL;
static double (*p_col_double)(sqlite3_stmt*, int) = NULL;

static int resolveAPI(void) {
    static int resolved = 0;
    if (resolved) return 1;
    
    p_prepare = dlsym(RTLD_DEFAULT, "sqlite3_prepare_v2");
    p_step = dlsym(RTLD_DEFAULT, "sqlite3_step");
    p_finalize = dlsym(RTLD_DEFAULT, "sqlite3_finalize");
    p_col_double = dlsym(RTLD_DEFAULT, "sqlite3_column_double");
    
    if (!p_prepare || !p_step || !p_finalize || !p_col_double) {
        fprintf(stderr, "[Hist] ERROR: SQLite API not available via dlsym\n");
        return 0;
    }
    resolved = 1;
    return 1;
}

/* Lyrore hook types (must match lyrore_hooks.h) */
typedef struct LyroreAnalyzeHook {
    const char *zName;
    void (*xAnalyze)(sqlite3*, int, void*);
    void *pCtx;
} LyroreAnalyzeHook;

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
    LyroreAnalyzeHook *aAnalyzeHooks;    int nAnalyzeHooks;
    void *aPreOptHooks;                   int nPreOptHooks;
    LyroreEstimateHook *aEstimateHooks;  int nEstimateHooks;
    LyrorePostQueryHook *aPostQueryHooks; int nPostQueryHooks;
    int (*xInit)(sqlite3 *db, void **ppCtx);
    void (*xShutdown)(void *pCtx);
    void (*xSchemaChange)(sqlite3 *db, void *pCtx);
} LyrorePluginInfo;

/* Histogram structure */
#define NUM_BUCKETS 100
#define MAX_PRODUCT 120000.0

static struct {
    int initialized;
    i64 totalRows;
    double bucketWidth;
    i64 bucketCounts[NUM_BUCKETS];
    i64 cumulativeCounts[NUM_BUCKETS];
} g_hist = {0};

/* Test context - read from environment */
static struct {
    double threshold;   /* from HIST_THRESHOLD */
    i64 actualRows;     /* from HIST_ACTUAL */
    int firstAdjustment;
    double qErrorBefore;
    double qErrorAfter;
} g_ctx = {0};

/* LogEst conversions (matching SQLite's implementation) */
static u64 logEstToInt(LogEst x) {
    u64 n;
    if (x < 10) return (x >= 0) ? 1 : 0;
    n = x % 10; x /= 10;
    if (n >= 5) n -= 2;
    else if (n >= 1) n -= 1;
    if (x >= 3) return (n + 8) << (x - 3);
    if (x >= 1) return (n + 8) >> (3 - x);
    return 1;
}

static LogEst intToLogEst(u64 x) {
    LogEst y = 0;
    if (x <= 1) return 0;
    while (x >= 8) { y += 10; x >>= 1; }
    static const LogEst a[] = {0, 10, 16, 20, 23, 25, 27, 28};
    return y + a[x];
}

static double calcQError(i64 est, i64 actual) {
    if (actual <= 0 || est <= 0) return 1.0;
    double e = (double)est, a = (double)actual;
    return (e > a) ? e/a : a/e;
}

#define NOUT_OFFSET 22

/* Estimate rows for a given threshold using histogram */
static i64 estimateRows(double threshold) {
    if (!g_hist.initialized || threshold <= 0) return 0;
    if (threshold >= MAX_PRODUCT) return g_hist.totalRows;
    
    int bucket = (int)(threshold / g_hist.bucketWidth);
    if (bucket >= NUM_BUCKETS) bucket = NUM_BUCKETS - 1;
    
    /* Cumulative up to previous bucket + interpolation within current bucket */
    i64 est = (bucket > 0) ? g_hist.cumulativeCounts[bucket - 1] : 0;
    double frac = (threshold - bucket * g_hist.bucketWidth) / g_hist.bucketWidth;
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    est += (i64)(frac * g_hist.bucketCounts[bucket]);
    
    return (est > 0) ? est : 1;
}

/* ANALYZE hook - scans table and builds histogram */
static void buildHistogram(sqlite3 *db, int iDb, void *pCtx) {
    fprintf(stderr, "[Hist] ANALYZE: Building histogram...\n");
    
    if (!resolveAPI()) return;
    
    memset(&g_hist, 0, sizeof(g_hist));
    g_hist.bucketWidth = MAX_PRODUCT / NUM_BUCKETS;
    
    sqlite3_stmt *stmt = NULL;
    int rc = p_prepare(db, "SELECT price * qty FROM products", -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[Hist] prepare failed: %d\n", rc);
        return;
    }
    
    while ((rc = p_step(stmt)) == SQLITE_ROW) {
        double val = p_col_double(stmt, 0);
        g_hist.totalRows++;
        
        int bucket = (int)(val / g_hist.bucketWidth);
        if (bucket < 0) bucket = 0;
        if (bucket >= NUM_BUCKETS) bucket = NUM_BUCKETS - 1;
        g_hist.bucketCounts[bucket]++;
    }
    p_finalize(stmt);
    
    /* Build cumulative counts */
    i64 cum = 0;
    for (int i = 0; i < NUM_BUCKETS; i++) {
        cum += g_hist.bucketCounts[i];
        g_hist.cumulativeCounts[i] = cum;
    }
    
    g_hist.initialized = 1;
    fprintf(stderr, "[Hist] Built histogram: %lld rows, %d buckets (width=%.0f)\n", 
            (long long)g_hist.totalRows, NUM_BUCKETS, g_hist.bucketWidth);
}

/* Match function - always match if histogram is ready and threshold is set */
static int matchScan(sqlite3 *db, WhereLoopBuilder *b, WhereLoop *p, void *c) {
    return g_hist.initialized && g_ctx.threshold > 0;
}

/* Adjust estimate using histogram */
static void adjustEstimate(sqlite3 *db, WhereLoopBuilder *b, WhereLoop *pLoop, void *c) {
    LogEst *pNOut = (LogEst*)((char*)pLoop + NOUT_OFFSET);
    i64 sqliteEst = logEstToInt(*pNOut);
    i64 histEst = estimateRows(g_ctx.threshold);
    
    /* Calculate Q-errors */
    double qBefore = calcQError(sqliteEst, g_ctx.actualRows);
    double qAfter = calcQError(histEst, g_ctx.actualRows);
    
    /* Only report once per query (first WhereLoop) */
    if (!g_ctx.firstAdjustment) {
        g_ctx.firstAdjustment = 1;
        g_ctx.qErrorBefore = qBefore;
        g_ctx.qErrorAfter = qAfter;
        
        fprintf(stderr, "[Hist] ESTIMATE: SQLite=%lld, Histogram=%lld, Actual=%lld\n",
                (long long)sqliteEst, (long long)histEst, (long long)g_ctx.actualRows);
        fprintf(stderr, "[Hist] Q-ERROR: Before=%.2f, After=%.2f (%.1f%% improvement)\n",
                qBefore, qAfter, 100.0 * (1.0 - qAfter/qBefore));
    }
    
    /* Apply histogram estimate */
    *pNOut = intToLogEst(histEst);
}

/* Post-query hook - reset state */
static void postQuery(sqlite3 *db, void *v, void *s, void *c) {
    g_ctx.firstAdjustment = 0;
}

/* Plugin init - read environment variables */
static int pluginInit(sqlite3 *db, void **ppCtx) {
    const char *thr = getenv("HIST_THRESHOLD");
    const char *act = getenv("HIST_ACTUAL");
    
    if (thr) g_ctx.threshold = atof(thr);
    if (act) g_ctx.actualRows = atoll(act);
    
    fprintf(stderr, "[Hist] Plugin init: threshold=%.0f, actual=%lld\n",
            g_ctx.threshold, (long long)g_ctx.actualRows);
    return 0;
}

/* Hook arrays */
static LyroreAnalyzeHook aHooks[] = { {"hist_analyze", buildHistogram, NULL} };
static LyroreEstimateHook eHooks[] = { {"hist_estimate", 100, matchScan, adjustEstimate, NULL} };
static LyrorePostQueryHook pHooks[] = { {"hist_post", postQuery, NULL} };

/* Plugin entry point */
__attribute__((visibility("default")))
LyrorePluginInfo* lyrore_plugin_info(void) {
    static LyrorePluginInfo info = {
        1, "histogram_plugin",
        aHooks, 1,  /* analyze hooks */
        NULL, 0,    /* pre-opt hooks */
        eHooks, 1,  /* estimate hooks */
        pHooks, 1,  /* post-query hooks */
        pluginInit, NULL, NULL
    };
    return &info;
}
