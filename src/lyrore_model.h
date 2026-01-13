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
** LyroreContext - Per-connection context.
**
** This is the central state container attached to each sqlite3 connection.
** It manages the model registry and dedicated state connection.
*/
struct LyroreContext {
  sqlite3 *pStateDb;         /* Dedicated connection for state persistence */
  LyroreModelEntry *pModels; /* Linked list of registered models */
  int nModels;               /* Number of registered models */
  int dirty;                 /* Queries since last persist */
  int persistThreshold;      /* Persist after this many queries (default 100) */
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

#endif /* SQLITE_ENABLE_LYRORE */
#endif /* SQLITE_LYRORE_MODEL_H */
