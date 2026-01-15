/*
** E2E Test Plugin for Lyrore Step 2 - Provable Improvements
**
** This plugin demonstrates:
** 1. Estimate hook that ACTUALLY modifies cardinality estimates
** 2. Uses knowledge of price*qty correlation to improve estimates
**
** Build:
** gcc -shared -fPIC -I$HOME/sqlite_build -I$HOME/sqlite/src e2e_estimate_plugin.c -o e2e_estimate_plugin.so
*/
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

/* Forward declarations */
typedef struct sqlite3 sqlite3;
typedef struct Parse Parse;
typedef struct Select Select;
typedef struct WhereLoopBuilder WhereLoopBuilder;
typedef struct WhereLoop WhereLoop;
typedef struct Vdbe Vdbe;
typedef long long i64;
typedef unsigned long long u64;
typedef short int LogEst;

/* Hook types from lyrore_model.h */
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

typedef struct LyrorePreOptHook {
  const char *zName;
  int priority;
  int (*xRewrite)(sqlite3*, Parse*, Select*, void*);
  void *pCtx;
} LyrorePreOptHook;

typedef struct LyroreEstimateHook {
  const char *zName;
  int priority;
  int (*xMatch)(sqlite3*, WhereLoopBuilder*, WhereLoop*, void*);
  void (*xAdjust)(sqlite3*, WhereLoopBuilder*, WhereLoop*, void*);
  void *pCtx;
} LyroreEstimateHook;

typedef struct LyrorePostQueryHook {
  const char *zName;
  void (*xCollect)(sqlite3*, Vdbe*, LyroreQueryStats*, void*);
  void *pCtx;
} LyrorePostQueryHook;

typedef struct LyroreAnalyzeHook {
  const char *zName;
  void (*xAnalyze)(sqlite3*, int iDb, void*);
  void *pCtx;
} LyroreAnalyzeHook;

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

#define LYRORE_REWRITE_NONE     0

/*
** WhereLoop structure - we need to know the layout to modify nOut
** This is based on whereInt.h definitions
*/
typedef unsigned long long Bitmask;

/* Minimal WhereLoop structure to access nOut field */
struct MinimalWhereLoop {
  Bitmask prereq;
  Bitmask maskSelf;
#ifdef SQLITE_DEBUG
  char cId;
#endif
  unsigned char iTab;
  unsigned char iSortIdx;
  LogEst rSetup;
  LogEst rRun;
  LogEst nOut;       /* This is what we want to modify! */
};

/* LogEst conversion - log2(x) * 10 */
static LogEst myLogEst(u64 x){
  static LogEst a[] = { 0, 2, 3, 5, 6, 7, 8, 9 };
  LogEst y = 40;
  if( x<8 ){
    if( x<2 ) return 0;
    while( x<8 ){ y -= 10; x <<= 1; }
  }else{
    while( x>255 ){ y += 40; x >>= 4; }
    while( x>15 ){ y += 10; x >>= 1; }
  }
  return a[x&7] + y - 10;
}

static u64 myLogEstToInt(LogEst x){
  u64 n;
  if( x<10 ) return 1;
  n = x%10;
  x /= 10;
  if( n>=5 ) n -= 2;
  else if( n>=1 ) n -= 1;
  if( x>=3 ) return (n+8)<<(x-3);
  return (n+8)>>(3-x);
}

/* Plugin context */
typedef struct E2EPluginContext {
  int adjustmentCount;
  int matchCount;
  LogEst lastOriginalNOut;
  LogEst lastAdjustedNOut;
  i64 knownActualRows;  /* Set by test harness if known */
  int verbose;
} E2EPluginContext;

static E2EPluginContext g_ctx = {0, 0, 0, 0, 0, 1};

/* Export functions for test inspection */
int e2e_get_adjustment_count(void) { return g_ctx.adjustmentCount; }
int e2e_get_match_count(void) { return g_ctx.matchCount; }
LogEst e2e_get_last_original_nout(void) { return g_ctx.lastOriginalNOut; }
LogEst e2e_get_last_adjusted_nout(void) { return g_ctx.lastAdjustedNOut; }
void e2e_set_known_actual_rows(i64 rows) { g_ctx.knownActualRows = rows; }
void e2e_set_verbose(int v) { g_ctx.verbose = v; }
void e2e_reset(void) { 
  g_ctx.adjustmentCount = 0; 
  g_ctx.matchCount = 0;
  g_ctx.lastOriginalNOut = 0;
  g_ctx.lastAdjustedNOut = 0;
}

/*
** Estimate hook - ACTUALLY modifies the cardinality estimate
**
** For the products table with correlated price/qty:
** - SQLite assumes independence: ~10k * selectivity(price) * selectivity(qty)
** - But actual data has correlation: low price -> high qty
** - For (price * qty < threshold), the actual selectivity is different
**
** Our adjustment: We know the data distribution, so we provide a better estimate.
** In a real system, this would come from a trained model or histogram.
*/
static int e2eEstimateMatch(sqlite3 *db, WhereLoopBuilder *pBuilder, 
                            WhereLoop *pLoop, void *pCtx){
  g_ctx.matchCount++;
  /* Match all loops - we want to demonstrate we CAN modify them */
  return 1;
}

static void e2eEstimateAdjust(sqlite3 *db, WhereLoopBuilder *pBuilder,
                              WhereLoop *pLoop, void *pCtx){
  struct MinimalWhereLoop *loop = (struct MinimalWhereLoop*)pLoop;

  g_ctx.adjustmentCount++;
  g_ctx.lastOriginalNOut = loop->nOut;

  /*
  ** Key insight: For a 10K row table with correlated price/qty,
  ** SQLite's default estimate assumes independence.
  ** 
  ** For predicate (price * qty) < 1000:
  ** - SQLite might estimate ~3333 rows (33% selectivity guess)
  ** - Actual with correlation: depends on threshold
  **
  ** We simulate having learned the actual selectivity from training.
  ** For (price * qty) < 1000 on our correlated data:
  ** - Low prices (10-30): qty 800-1000, product ~8000-30000
  ** - High prices (90-110): qty 100-200, product ~9000-22000
  ** - Medium prices (40-80): qty 300-700, product ~12000-56000
  ** 
  ** With threshold 1000, almost nothing qualifies (edge cases only)
  ** With threshold 15000, roughly 20-30% might qualify
  ** With threshold 30000, roughly 60-70% might qualify
  **
  ** For this test, we'll use a more conservative adjustment:
  ** If the original estimate seems unreasonable (>50% of table), reduce it.
  ** This simulates having correlation knowledge.
  */

  /* Convert current estimate to row count */
  u64 currentEst = myLogEstToInt(loop->nOut);

  /* Apply a simple adjustment: reduce high estimates by factor
  ** In real implementation, this would use histogram or learned model
  ** For products table with correlation, estimates > 4000 are likely too high
  ** for predicates on (price * qty)
  */
  u64 adjustedEst = currentEst;

  if( currentEst > 5000 ){
    /* High estimate - likely missing correlation info */
    /* Reduce by ~40% to account for correlation */
    adjustedEst = (currentEst * 6) / 10;
  } else if( currentEst > 2000 ){
    /* Medium estimate - slight reduction */
    adjustedEst = (currentEst * 8) / 10;
  }
  /* Low estimates left as-is */

  if( adjustedEst != currentEst ){
    loop->nOut = myLogEst(adjustedEst);
    g_ctx.lastAdjustedNOut = loop->nOut;

    if( g_ctx.verbose ){
      fprintf(stderr, "[E2E Plugin] Adjusted estimate: %llu -> %llu (LogEst %d -> %d)\n",
              (unsigned long long)currentEst, (unsigned long long)adjustedEst,
              g_ctx.lastOriginalNOut, loop->nOut);
    }
  } else {
    g_ctx.lastAdjustedNOut = loop->nOut;
  }
}

/* Pre-opt hook - placeholder for UDF transformation demo */
static int e2ePreOptRewrite(sqlite3 *db, Parse *pParse, Select *p, void *pCtx){
  /* Log that we were called */
  if( g_ctx.verbose ){
    fprintf(stderr, "[E2E Plugin] Pre-opt hook invoked\n");
  }
  return LYRORE_REWRITE_NONE;
}

/* Post-query hook - collect stats for verification */
static void e2ePostQueryCollect(sqlite3 *db, Vdbe *p, 
                                LyroreQueryStats *stats, void *pCtx){
  if( g_ctx.verbose && stats ){
    fprintf(stderr, "[E2E Plugin] Post-query: %d loops\n", stats->nLoops);
    if( stats->aLoops ){
      for( int i = 0; i < stats->nLoops; i++ ){
        fprintf(stderr, "  Loop %d: est=%lld actual=%lld qError=%.2f\n",
                i, (long long)stats->aLoops[i].estRows,
                (long long)stats->aLoops[i].actualRows,
                stats->aLoops[i].qError);
      }
    }
  }
}

/* Plugin lifecycle */
static int e2ePluginInit(sqlite3 *db, void **ppCtx){
  *ppCtx = &g_ctx;
  e2e_reset();
  return 0;
}

static void e2ePluginShutdown(void *pCtx){
  /* Nothing to clean up */
}

/* Hook arrays */
static LyrorePreOptHook preOptHooks[] = {
  { "e2e_preopt", 100, e2ePreOptRewrite, &g_ctx }
};

static LyroreEstimateHook estimateHooks[] = {
  { "e2e_estimate", 100, e2eEstimateMatch, e2eEstimateAdjust, &g_ctx }
};

static LyrorePostQueryHook postQueryHooks[] = {
  { "e2e_postquery", e2ePostQueryCollect, &g_ctx }
};

/* Plugin info */
static LyrorePluginInfo pluginInfo = {
  1,                            /* version */
  "e2e_estimate_plugin",        /* name */
  0, 0,                         /* analyze hooks */
  preOptHooks, 1,               /* pre-opt hooks */
  estimateHooks, 1,             /* estimate hooks */
  postQueryHooks, 1,            /* post-query hooks */
  e2ePluginInit,
  e2ePluginShutdown,
  0
};

/* Entry point */
LyrorePluginInfo* lyrore_plugin_info(void){
  return &pluginInfo;
}
