/*
** Test plugin for Lyrore Step 2: Plugin, Stats, and Pre-Optimization Framework
**
** This plugin demonstrates:
** 1. Pre-opt hook: Transform product_filter(a,b,c)=1 -> (a*b)<c
** 2. Estimate hook: Simple cardinality adjustment
** 3. Post-query hook: Stats collection and logging
** 4. Analyze hook: Called during ANALYZE
**
** Compile with:
** gcc -shared -fPIC -I/path/to/sqlite/src test_plugin.c -o test_plugin.so
*/
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Forward declarations - these types are defined in sqliteInt.h */
typedef struct sqlite3 sqlite3;
typedef struct Parse Parse;
typedef struct Select Select;
typedef struct WhereLoopBuilder WhereLoopBuilder;
typedef struct WhereLoop WhereLoop;
typedef struct Vdbe Vdbe;
typedef struct Expr Expr;

/* Type definitions from lyrore_model.h */
typedef long long i64;
typedef unsigned long long u64;

typedef struct LyroreLoopStats {
  const char *zExplain;
  i64 estRows;
  i64 actualRows;
  double qError;
} LyroreLoopStats;

typedef struct LyroreQueryStats {
  const char *zSql;
  u64 queryHash;
  i64 execTimeUs;
  i64 nVmStep;
  int nLoops;
  LyroreLoopStats *aLoops;
  int nCustom;
  double *aCustom;
} LyroreQueryStats;

/* Pre-Optimization Hook */
typedef struct LyrorePreOptHook {
  const char *zName;
  int priority;
  int (*xRewrite)(sqlite3*, Parse*, Select*, void*);
  void *pCtx;
} LyrorePreOptHook;

/* Estimate Hook */
typedef struct LyroreEstimateHook {
  const char *zName;
  int priority;
  int (*xMatch)(sqlite3*, WhereLoopBuilder*, WhereLoop*, void*);
  void (*xAdjust)(sqlite3*, WhereLoopBuilder*, WhereLoop*, void*);
  void *pCtx;
} LyroreEstimateHook;

/* Post-Query Hook */
typedef struct LyrorePostQueryHook {
  const char *zName;
  void (*xCollect)(sqlite3*, Vdbe*, LyroreQueryStats*, void*);
  void *pCtx;
} LyrorePostQueryHook;

/* Analyze Hook */
typedef struct LyroreAnalyzeHook {
  const char *zName;
  void (*xAnalyze)(sqlite3*, int iDb, void*);
  void *pCtx;
} LyroreAnalyzeHook;

/* Plugin Info Structure */
typedef struct LyrorePluginInfo {
  int version;
  const char *name;
  LyroreAnalyzeHook *aAnalyzeHooks;     int nAnalyzeHooks;
  LyrorePreOptHook *aPreOptHooks;       int nPreOptHooks;
  LyroreEstimateHook *aEstimateHooks;   int nEstimateHooks;
  LyrorePostQueryHook *aPostQueryHooks; int nPostQueryHooks;
  int (*xInit)(sqlite3 *db, void **ppCtx);
  void (*xShutdown)(void *pCtx);
  void (*xSchemaChange)(sqlite3 *db, void *pCtx);
} LyrorePluginInfo;

/* Return values for pre-opt hooks */
#define LYRORE_REWRITE_NONE     0
#define LYRORE_REWRITE_MODIFIED 1
#define LYRORE_REWRITE_ERROR   -1

/* Plugin context for tracking stats */
typedef struct TestPluginContext {
  int nQueriesProcessed;
  int nRewrites;
  int nEstimateAdjustments;
  i64 totalExecTimeUs;
} TestPluginContext;

static TestPluginContext g_ctx = {0, 0, 0, 0};

/*
** Pre-opt hook: This is a placeholder that logs when it's called.
** Real UDF transformation would require access to SQLite internal
** expression tree APIs which aren't available to external plugins.
*/
static int testPreOptRewrite(sqlite3 *db, Parse *pParse, Select *p, void *pCtx){
  TestPluginContext *ctx = (TestPluginContext*)pCtx;
  if( ctx ){
    /* Just log that we were called - real implementation would
    ** walk the expression tree and transform product_filter calls */
    ctx->nQueriesProcessed++;
  }
  return LYRORE_REWRITE_NONE;  /* No modification in this test */
}

/*
** Estimate hook match: Always match for testing purposes
*/
static int testEstimateMatch(sqlite3 *db, WhereLoopBuilder *pBuilder, 
                             WhereLoop *pLoop, void *pCtx){
  return 1;  /* Match all loops */
}

/*
** Estimate hook adjust: Log that we were called
** In real implementation, this would adjust pLoop->nOut and pLoop->rRun
*/
static void testEstimateAdjust(sqlite3 *db, WhereLoopBuilder *pBuilder,
                               WhereLoop *pLoop, void *pCtx){
  TestPluginContext *ctx = (TestPluginContext*)pCtx;
  if( ctx ){
    ctx->nEstimateAdjustments++;
  }
  /* Real implementation would:
  ** pLoop->nOut = sqlite3LogEst(adjustedRows);
  ** pLoop->rRun = sqlite3LogEst(adjustedCost);
  */
}

/*
** Post-query hook: Collect execution statistics
*/
static void testPostQueryCollect(sqlite3 *db, Vdbe *p, 
                                 LyroreQueryStats *stats, void *pCtx){
  TestPluginContext *ctx = (TestPluginContext*)pCtx;
  if( ctx && stats ){
    ctx->totalExecTimeUs += stats->execTimeUs;
  }
}

/*
** Analyze hook: Called during ANALYZE
*/
static void testAnalyze(sqlite3 *db, int iDb, void *pCtx){
  /* In real implementation, this would train models
  ** based on the table statistics */
}

/*
** Plugin lifecycle: Initialize
*/
static int testPluginInit(sqlite3 *db, void **ppCtx){
  *ppCtx = &g_ctx;
  g_ctx.nQueriesProcessed = 0;
  g_ctx.nRewrites = 0;
  g_ctx.nEstimateAdjustments = 0;
  g_ctx.totalExecTimeUs = 0;
  return 0;  /* SQLITE_OK */
}

/*
** Plugin lifecycle: Shutdown
*/
static void testPluginShutdown(void *pCtx){
  /* Nothing to clean up in this simple test */
}

/* Hook instances */
static LyrorePreOptHook preOptHooks[] = {
  { "test_preopt", 100, testPreOptRewrite, &g_ctx }
};

static LyroreEstimateHook estimateHooks[] = {
  { "test_estimate", 100, testEstimateMatch, testEstimateAdjust, &g_ctx }
};

static LyrorePostQueryHook postQueryHooks[] = {
  { "test_postquery", testPostQueryCollect, &g_ctx }
};

static LyroreAnalyzeHook analyzeHooks[] = {
  { "test_analyze", testAnalyze, &g_ctx }
};

/* Plugin entry point */
static LyrorePluginInfo pluginInfo = {
  1,                            /* version */
  "test_plugin",                /* name */
  analyzeHooks, 1,              /* analyze hooks */
  preOptHooks, 1,               /* pre-opt hooks */
  estimateHooks, 1,             /* estimate hooks */
  postQueryHooks, 1,            /* post-query hooks */
  testPluginInit,               /* xInit */
  testPluginShutdown,           /* xShutdown */
  0                             /* xSchemaChange */
};

/* The entry point symbol that lyrore_register() looks for */
LyrorePluginInfo* lyrore_plugin_info(void){
  return &pluginInfo;
}
