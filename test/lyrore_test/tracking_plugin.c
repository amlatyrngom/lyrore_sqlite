/*
** Test tracking plugin - verifies hooks are invoked correctly
** Counts hook invocations and logs stats
*/
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Type declarations matching SQLite internals */
typedef struct sqlite3 sqlite3;
typedef struct Parse Parse;
typedef struct Select Select;
typedef struct WhereLoopBuilder WhereLoopBuilder;
typedef struct WhereLoop WhereLoop;
typedef struct Vdbe Vdbe;
typedef long long i64;
typedef unsigned long long u64;
typedef short int LogEst;

/* Stats structures */
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

/* Hook types */
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
#define LYRORE_REWRITE_MODIFIED 1
#define LYRORE_REWRITE_ERROR   -1

/* Tracking context - exported for inspection */
typedef struct TrackingContext {
  int preOptInvocations;
  int estimateMatchCalls;
  int estimateAdjustCalls;
  int postQueryCalls;
  int analyzeCalls;
  i64 totalLoopsObserved;
  double totalQError;
  char lastSql[256];
} TrackingContext;

/* Global context for inspection from tests */
static TrackingContext g_track = {0};

/* Export functions to read tracking data */
int tracking_get_preopt_count(void) { return g_track.preOptInvocations; }
int tracking_get_estimate_match_count(void) { return g_track.estimateMatchCalls; }
int tracking_get_estimate_adjust_count(void) { return g_track.estimateAdjustCalls; }
int tracking_get_postquery_count(void) { return g_track.postQueryCalls; }
int tracking_get_analyze_count(void) { return g_track.analyzeCalls; }
i64 tracking_get_total_loops(void) { return g_track.totalLoopsObserved; }
double tracking_get_total_qerror(void) { return g_track.totalQError; }
const char* tracking_get_last_sql(void) { return g_track.lastSql; }
void tracking_reset(void) { memset(&g_track, 0, sizeof(g_track)); }

/* Pre-opt hook implementation */
static int trackPreOptRewrite(sqlite3 *db, Parse *pParse, Select *p, void *pCtx){
  g_track.preOptInvocations++;
  return LYRORE_REWRITE_NONE;
}

/* Estimate hook - match */
static int trackEstimateMatch(sqlite3 *db, WhereLoopBuilder *pBuilder, 
                              WhereLoop *pLoop, void *pCtx){
  g_track.estimateMatchCalls++;
  return 1;  /* Always match */
}

/* Estimate hook - adjust */
static void trackEstimateAdjust(sqlite3 *db, WhereLoopBuilder *pBuilder,
                                WhereLoop *pLoop, void *pCtx){
  g_track.estimateAdjustCalls++;
  /* Note: Real implementation would modify pLoop->nOut and pLoop->rRun */
}

/* Post-query hook */
static void trackPostQueryCollect(sqlite3 *db, Vdbe *p, 
                                  LyroreQueryStats *stats, void *pCtx){
  g_track.postQueryCalls++;
  if( stats ){
    if( stats->zSql && strlen(stats->zSql) < 255 ){
      strncpy(g_track.lastSql, stats->zSql, 255);
    }
    g_track.totalLoopsObserved += stats->nLoops;
    if( stats->aLoops ){
      int i;
      for(i = 0; i < stats->nLoops; i++){
        g_track.totalQError += stats->aLoops[i].qError;
      }
    }
  }
}

/* Analyze hook */
static void trackAnalyze(sqlite3 *db, int iDb, void *pCtx){
  g_track.analyzeCalls++;
}

/* Plugin lifecycle */
static int trackPluginInit(sqlite3 *db, void **ppCtx){
  *ppCtx = &g_track;
  tracking_reset();
  return 0;
}

static void trackPluginShutdown(void *pCtx){
  /* Nothing to clean up */
}

/* Hook arrays */
static LyrorePreOptHook preOptHooks[] = {
  { "track_preopt", 100, trackPreOptRewrite, &g_track }
};

static LyroreEstimateHook estimateHooks[] = {
  { "track_estimate", 100, trackEstimateMatch, trackEstimateAdjust, &g_track }
};

static LyrorePostQueryHook postQueryHooks[] = {
  { "track_postquery", trackPostQueryCollect, &g_track }
};

static LyroreAnalyzeHook analyzeHooks[] = {
  { "track_analyze", trackAnalyze, &g_track }
};

/* Plugin info */
static LyrorePluginInfo pluginInfo = {
  1,                            /* version */
  "tracking_plugin",            /* name */
  analyzeHooks, 1,
  preOptHooks, 1,
  estimateHooks, 1,
  postQueryHooks, 1,
  trackPluginInit,
  trackPluginShutdown,
  0
};

/* Entry point */
LyrorePluginInfo* lyrore_plugin_info(void){
  return &pluginInfo;
}
