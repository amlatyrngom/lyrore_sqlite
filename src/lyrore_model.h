/*
** 2024-01-13
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** This file contains definitions for the Lyrore ML-ready optimization
** framework. It defines the core data structures for model registration,
** feature extraction, and the pluggable model interface.
*/
#ifndef SQLITE_LYRORE_MODEL_H
#define SQLITE_LYRORE_MODEL_H

#ifdef SQLITE_ENABLE_LYRORE

/* Forward declarations */
typedef struct LyroreContext LyroreContext;
typedef struct LyroreModelOps LyroreModelOps;
typedef struct LyroreModelEntry LyroreModelEntry;
typedef struct LyroreFeatures LyroreFeatures;
typedef struct LyrorePatternRegistry LyrorePatternRegistry;

/*
** Feature extraction flags for lyroreFeaturesToArrayMasked().
** Each bit corresponds to a specific feature field.
*/
#define LYRORE_FEAT_NTABLES       (1<<0)
#define LYRORE_FEAT_NJOINS        (1<<1)
#define LYRORE_FEAT_NPREDICATES   (1<<2)
#define LYRORE_FEAT_NAGGREGATIONS (1<<3)
#define LYRORE_FEAT_QUERYDEPTH    (1<<4)
#define LYRORE_FEAT_NPROJECTIONS  (1<<5)
#define LYRORE_FEAT_ESTTOTALROWS  (1<<6)
#define LYRORE_FEAT_ESTOUTPUTROWS (1<<7)
#define LYRORE_FEAT_ESTCOST       (1<<8)
#define LYRORE_FEAT_AVGSELECTIVITY (1<<9)
#define LYRORE_FEAT_NINDEXSCANS   (1<<10)
#define LYRORE_FEAT_NTABLESCANS   (1<<11)
#define LYRORE_FEAT_HASEQUALITY   (1<<12)
#define LYRORE_FEAT_HASRANGE      (1<<13)
#define LYRORE_FEAT_HASLIKE       (1<<14)
#define LYRORE_FEAT_LASTEXECTIME  (1<<15)
#define LYRORE_FEAT_AVGEXECTIME   (1<<16)
#define LYRORE_FEAT_EXECCOUNT     (1<<17)
#define LYRORE_FEAT_ALL           0xFFFFFFFF
#define LYRORE_DEFAULT_FEATURES   0x0003FFFF  /* All 18 static features */

/*
** Number of static feature fields in LyroreFeatures.
*/
#define LYRORE_NUM_STATIC_FEATURES 18

/*
** LyroreFeatures - Input features for models.
**
** This structure contains all extractable features from query plans.
** Models decide which features to use via their own configuration.
** Features are purely INPUT to models (query/plan characteristics).
** Model weights are INTERNAL to each model's state (via xSave/xLoad).
*/
struct LyroreFeatures {
  /* Query structure */
  int nTables;           /* Number of tables in FROM clause */
  int nJoins;            /* Number of joins (nTables - 1 if > 1) */
  int nPredicates;       /* Number of WHERE clause terms */
  int nAggregations;     /* Number of aggregate functions */
  int queryDepth;        /* Subquery nesting level */
  int nProjections;      /* Number of result columns */

  /* Cost estimates */
  double estTotalRows;   /* Estimated total rows in table */
  double estOutputRows;  /* Estimated output rows */
  double estCost;        /* Estimated execution cost */
  double avgSelectivity; /* Average predicate selectivity */

  /* Access patterns */
  int nIndexScans;       /* Number of index scan loops */
  int nTableScans;       /* Number of full table scan loops */
  int hasEquality;       /* Has equality predicates */
  int hasRange;          /* Has range predicates */
  int hasLike;           /* Has LIKE predicates */

  /* Historical (from past executions) */
  double lastExecTime;   /* Most recent execution time (us) */
  double avgExecTime;    /* Average execution time (us) */
  int execCount;         /* Number of times pattern executed */

  /* Dynamic extension vector (for additional features, NOT weights) */
  int nExtended;         /* Count of extended feature values */
  double *aExtended;     /* Dynamically allocated array */
};

/*
** LyroreModelOps - Pluggable model interface.
**
** This structure defines the callbacks for a model implementation.
** Models can implement prediction, selection, learning, and persistence.
*/
struct LyroreModelOps {
  const char *zName;     /* Model name for identification */

  /* Lifecycle */
  void *(*xCreate)(sqlite3 *db, const char *zConfig);  /* Create model state */
  void (*xDestroy)(void *pModel);                      /* Destroy model state */

  /* Inference */
  double (*xPredict)(void *pModel, double *aFeat, int nFeat);  /* Continuous prediction */
  int (*xSelect)(void *pModel, double *aFeat, int nFeat, int nOptions);  /* Discrete selection */

  /* Learning */
  void (*xUpdate)(void *pModel, double *aFeat, int nFeat,
                  double predicted, double actual);    /* Regression update */
  void (*xReward)(void *pModel, int selected, double reward);  /* Bandit reward */

  /* Persistence */
  int (*xSave)(void *pModel, sqlite3 *db);             /* Serialize to DB */
  int (*xLoad)(void *pModel, sqlite3 *db);             /* Deserialize from DB */
};

/*
** LyroreModelEntry - Registry entry for a model.
**
** Used internally to maintain a linked list of registered models.
*/
struct LyroreModelEntry {
  char *zPurpose;           /* Model purpose (e.g., "cost", "plan", "flavor") */
  LyroreModelOps *pOps;     /* Model operations */
  void *pState;             /* Model state */
  LyroreModelEntry *pNext;  /* Next entry in linked list */
};


/*
** Pattern Matching Framework Types (Step 1.5)
*/

/* Forward declarations for pattern framework */
struct WhereLoop;
struct WhereInfo;
struct Parse;
struct Expr;
struct Table;

/*
** Pattern ID enum - workload-specific patterns added here.
*/
typedef enum {
  LYRORE_PAT_DEFAULT = 0,
  LYRORE_PAT_COST_BASE = 100,
  LYRORE_PAT_PLAN_BASE = 200,
  LYRORE_PAT_EXPR_BASE = 300,
  LYRORE_PAT_WORKLOAD_BASE = 1000,
  LYRORE_PAT_COUNT = 2000
} LyrorePatternId;

/*
** Pattern category enum - determines which registry to use.
*/
typedef enum {
  LYRORE_CAT_COST = 1,    /* Cost/cardinality estimation (where.c) */
  LYRORE_CAT_PLAN = 2,    /* Plan selection (wherePathSolver) */
  LYRORE_CAT_EXPR = 3     /* Expression codegen (expr.c) */
} LyrorePatternCategory;

/*
** Typed matcher functions - return 1 if pattern matches, 0 otherwise.
*/
typedef int (*LyroreCostMatcher)(struct WhereLoop*, struct Table*, void*);
typedef int (*LyrorePlanMatcher)(struct WhereInfo*, void*);
typedef int (*LyroreExprMatcher)(struct Parse*, struct Expr*, void*);

/*
** Feature extractors - populate LyroreFeatures with pattern-specific features.
*/
typedef void (*LyroreCostFeatureExtractor)(struct WhereLoop*, struct Table*, LyroreFeatures*);
typedef void (*LyrorePlanFeatureExtractor)(struct WhereInfo*, LyroreFeatures*);
typedef void (*LyroreExprFeatureExtractor)(struct Parse*, struct Expr*, LyroreFeatures*);

/*
** Pattern Definition Structure
*/
struct LyrorePatternDef {
  LyrorePatternId id;           /* Numeric ID for fast dispatch */
  const char *zName;            /* String name for debugging/persistence */
  LyrorePatternCategory category;
  int priority;                 /* Higher priority checked first (0-1000) */

  /* Matcher function - union discriminated by category */
  union {
    LyroreCostMatcher xCostMatch;
    LyrorePlanMatcher xPlanMatch;
    LyroreExprMatcher xExprMatch;
    void *xGenericMatch;
  } matcher;

  /* Feature extraction - union discriminated by category */
  union {
    LyroreCostFeatureExtractor xCostFeatures;
    LyrorePlanFeatureExtractor xPlanFeatures;
    LyroreExprFeatureExtractor xExprFeatures;
    void *xGenericFeatures;
  } features;
  int nExtendedFeatures;        /* Number of extended features */

  /* Pattern-specific models (NULL = use global model) */
  LyroreModelOps *pCostModel;
  void *pCostState;
  LyroreModelOps *pSelectModel;
  void *pSelectState;

  void *pMatchCtx;              /* Context for matcher */
};
typedef struct LyrorePatternDef LyrorePatternDef;

/*
** Pattern Registry Structure
*/
struct LyrorePatternRegistry {
  int nPatterns;                /* Number of registered patterns */
  int nAlloc;                   /* Allocated slots */
  LyrorePatternDef **aPatterns; /* Sorted by priority (descending) */
  LyrorePatternDef defaultPat;  /* Default fallback pattern */
};


/*
** Forward declarations for hook parameters (Step 2)
*/
typedef struct Vdbe Vdbe;
typedef struct Select Select;

/*
** Hook type definitions (Step 2)
*/

/* Return values for pre-opt hooks */
#define LYRORE_REWRITE_NONE     0
#define LYRORE_REWRITE_MODIFIED 1
#define LYRORE_REWRITE_ERROR   -1

/* Pre-Optimization Hook - transforms expressions before WHERE optimization */
typedef struct LyrorePreOptHook {
  const char *zName;           /* Hook identifier */
  int priority;                /* Higher = run first */
  int (*xRewrite)(sqlite3*, Parse*, Select*, void*);
  void *pCtx;                  /* Hook context */
} LyrorePreOptHook;

/* Estimate Hook - overrides cost/cardinality estimates */
typedef struct LyroreEstimateHook {
  const char *zName;           /* Hook identifier */
  int priority;                /* Higher = run first */
  int (*xMatch)(sqlite3*, WhereLoopBuilder*, WhereLoop*, void*);
  void (*xAdjust)(sqlite3*, WhereLoopBuilder*, WhereLoop*, void*);
  void *pCtx;                  /* Hook context */
} LyroreEstimateHook;

/* Post-Query Statistics */
typedef struct LyroreLoopStats {
  const char *zExplain;        /* EXPLAIN output for this loop */
  i64 estRows;                 /* Estimated rows */
  i64 actualRows;              /* Actual rows (-1 if unknown) */
  double qError;               /* Q-error = max(est/actual, actual/est) */
} LyroreLoopStats;

typedef struct LyroreQueryStats {
  const char *zSql;            /* SQL text */
  u64 queryHash;               /* Query hash */
  i64 execTimeUs;              /* Execution time in microseconds */
  i64 nVmStep;                 /* VDBE steps executed */
  int nLoops;                  /* Number of loop stats */
  LyroreLoopStats *aLoops;     /* Per-loop statistics (dynamically allocated) */
  int nCustom;                 /* Number of custom metrics */
  double *aCustom;             /* Plugin-provided custom metrics */
} LyroreQueryStats;

/* Post-Query Hook - collects execution statistics */
typedef struct LyrorePostQueryHook {
  const char *zName;           /* Hook identifier */
  void (*xCollect)(sqlite3*, Vdbe*, LyroreQueryStats*, void*);
  void *pCtx;                  /* Hook context */
} LyrorePostQueryHook;

/* Analyze Hook - trains models during ANALYZE */
typedef struct LyroreAnalyzeHook {
  const char *zName;           /* Hook identifier */
  void (*xAnalyze)(sqlite3*, int iDb, void*);
  void *pCtx;                  /* Hook context */
} LyroreAnalyzeHook;

/*
** Plugin types (Step 2)
*/
#define LYRORE_PLUGIN_VERSION 1
#define LYRORE_PLUGIN_ENTRY "lyrore_plugin_info"

/* Plugin entry structure provided by plugins */
typedef struct LyrorePluginInfo {
  int version;                             /* API version (1) */
  const char *name;                        /* Plugin identifier */

  /* Hook Arrays (NULL = not provided) */
  LyroreAnalyzeHook *aAnalyzeHooks;        int nAnalyzeHooks;
  LyrorePreOptHook *aPreOptHooks;          int nPreOptHooks;
  LyroreEstimateHook *aEstimateHooks;      int nEstimateHooks;
  LyrorePostQueryHook *aPostQueryHooks;    int nPostQueryHooks;

  /* Lifecycle callbacks */
  int (*xInit)(sqlite3 *db, void **ppCtx);
  void (*xShutdown)(void *pCtx);
  void (*xSchemaChange)(sqlite3 *db, void *pCtx);
} LyrorePluginInfo;

/* Loaded plugin entry */
typedef struct LyrorePluginEntry {
  char *zName;                 /* Plugin name */
  char *zPath;                 /* Path to .so file */
  void *pHandle;               /* dlopen handle */
  LyrorePluginInfo *pInfo;     /* Plugin info from entry point */
  void *pCtx;                  /* Plugin context from xInit */
  struct LyrorePluginEntry *pNext;  /* Linked list */
} LyrorePluginEntry;

/* Hook array structure for dynamic allocation */
typedef struct LyroreHookArray {
  void *a;                     /* Array of hooks (cast to specific type) */
  int n;                       /* Number of registered hooks */
  int nAlloc;                  /* Allocated slots */
} LyroreHookArray;

/*
** LyroreContext - Per-connection context.
**
** This is the central state container attached to each sqlite3 connection.
** It manages the model registry, pattern registries, hooks, and plugins.
*/
struct LyroreContext {
  sqlite3 *pStateDb;         /* Dedicated connection for state persistence */
  LyroreModelEntry *pModels; /* Linked list of registered models */
  int nModels;               /* Number of registered models */
  int dirty;                 /* Queries since last persist */
  int persistThreshold;      /* Persist after this many queries (default 100) */

  /* Pattern registries (Step 1.5) */
  LyrorePatternRegistry costPatterns;  /* Cost/cardinality patterns */
  LyrorePatternRegistry planPatterns;  /* Plan selection patterns */
  LyrorePatternRegistry exprPatterns;  /* Expression flavor patterns */

  /* Plugin management (Step 2) */
  LyrorePluginEntry *pPlugins;         /* Linked list of loaded plugins */
  int nPlugins;                        /* Number of loaded plugins */

  /* Hook arrays (Step 2) - sorted by priority (descending) */
  struct {
    LyrorePreOptHook *a;
    int n;
    int nAlloc;
  } preOptHooks;

  struct {
    LyroreEstimateHook *a;
    int n;
    int nAlloc;
  } estimateHooks;

  struct {
    LyrorePostQueryHook *a;
    int n;
    int nAlloc;
  } postQueryHooks;

  struct {
    LyroreAnalyzeHook *a;
    int n;
    int nAlloc;
  } analyzeHooks;
};

/*
** Model Management API
*/
int lyroreRegisterModel(sqlite3 *db, const char *zPurpose,
                        LyroreModelOps *pOps, const char *zConfig);
int lyroreUnregisterModel(sqlite3 *db, const char *zPurpose);
LyroreModelOps *lyroreGetModel(sqlite3 *db, const char *zPurpose);
void *lyroreGetModelState(sqlite3 *db, const char *zPurpose);

/*
** Feature utilities
*/
void lyroreInitFeatures(LyroreFeatures *pFeat);
void lyroreFreeFeatures(LyroreFeatures *pFeat);
int lyroreFeaturesToArray(LyroreFeatures *pFeat, double *aOut, int nMax);
int lyroreFeaturesToArrayMasked(LyroreFeatures *pFeat, double *aOut,
                                 int nMax, u32 featureMask);

/*
** Hook Invocation Forward Declarations (Step 2)
** These are declared here to ensure they are visible before the call sites
** in select.c, where.c, vdbeaux.c, and vdbe.c.
*/
int lyroreInvokePreOptHooks(sqlite3 *db, Parse *pParse, Select *p);
void lyroreInvokeEstimateHooks(sqlite3 *db, WhereLoopBuilder *pBuilder, WhereLoop *pLoop);
void lyroreInvokePostQueryHooks(sqlite3 *db, Vdbe *p);
void lyroreInvokeAnalyzeHooks(sqlite3 *db, int iDb);

#endif /* SQLITE_ENABLE_LYRORE */
#endif /* SQLITE_LYRORE_MODEL_H */
