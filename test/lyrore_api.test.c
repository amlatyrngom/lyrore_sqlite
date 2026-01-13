/*
** 2024 January 13
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** C API Test program for Lyrore model registration API.
** Compile with: gcc -o test_lyrore_api test/lyrore_api.test.c -I. -L. -lsqlite3
** Run with: LD_LIBRARY_PATH=. ./test_lyrore_api
*/
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "sqlite3.h"

/* Forward declare Lyrore types and functions */
typedef struct LyroreFeatures LyroreFeatures;
typedef struct LyroreModelOps LyroreModelOps;

struct LyroreFeatures {
  int nTables, nJoins, nPredicates, nAggregations;
  int queryDepth, nProjections;
  double estTotalRows, estOutputRows, estCost, avgSelectivity;
  int nIndexScans, nTableScans, hasEquality, hasRange, hasLike;
  double lastExecTime, avgExecTime;
  int execCount;
  int nExtended;
  double *aExtended;
};

struct LyroreModelOps {
  const char *zName;
  void *(*xCreate)(sqlite3 *db, const char *zConfig);
  void (*xDestroy)(void *pModel);
  double (*xPredict)(void *pModel, double *aFeat, int nFeat);
  int (*xSelect)(void *pModel, double *aFeat, int nFeat, int nOptions);
  void (*xUpdate)(void *pModel, double *aFeat, int nFeat, double predicted, double actual);
  void (*xReward)(void *pModel, int selected, double reward);
  int (*xSave)(void *pModel, sqlite3 *db);
  int (*xLoad)(void *pModel, sqlite3 *db);
};

/* External functions from lyrore_model.c */
extern int lyroreRegisterModel(sqlite3 *db, const char *zPurpose, LyroreModelOps *pOps, const char *zConfig);
extern int lyroreUnregisterModel(sqlite3 *db, const char *zPurpose);
extern LyroreModelOps *lyroreGetModel(sqlite3 *db, const char *zPurpose);
extern void *lyroreGetModelState(sqlite3 *db, const char *zPurpose);
extern void lyroreInitFeatures(LyroreFeatures *pFeat);
extern void lyroreFreeFeatures(LyroreFeatures *pFeat);
extern int lyroreFeaturesToArray(LyroreFeatures *pFeat, double *aOut, int nMax);

/* Test model implementation */
typedef struct {
  double correction;
  int callCount;
} TestModelState;

static void *testModelCreate(sqlite3 *db, const char *zConfig) {
  TestModelState *p = sqlite3_malloc(sizeof(*p));
  if(p) {
    p->correction = 1.0;
    p->callCount = 0;
  }
  return p;
}

static void testModelDestroy(void *pModel) {
  sqlite3_free(pModel);
}

static double testModelPredict(void *pModel, double *aFeat, int nFeat) {
  TestModelState *p = (TestModelState*)pModel;
  p->callCount++;
  return aFeat[0] * p->correction;
}

static int testModelSelect(void *pModel, double *aFeat, int nFeat, int nOptions) {
  return 0;
}

static void testModelUpdate(void *pModel, double *aFeat, int nFeat, double predicted, double actual) {
  TestModelState *p = (TestModelState*)pModel;
  if(predicted > 0) {
    p->correction = 0.9 * p->correction + 0.1 * (actual / predicted);
  }
}

static LyroreModelOps testModelOps = {
  "TestModel",
  testModelCreate,
  testModelDestroy,
  testModelPredict,
  testModelSelect,
  testModelUpdate,
  NULL,
  NULL,
  NULL
};

int main(int argc, char **argv) {
  sqlite3 *db;
  int rc;
  int tests_passed = 0;
  int tests_failed = 0;

  printf("=== Lyrore API Test Suite ===\n\n");

  rc = sqlite3_open(":memory:", &db);
  if(rc != SQLITE_OK) {
    printf("FAIL: Could not open database: %s\n", sqlite3_errmsg(db));
    return 1;
  }
  printf("PASS: Database opened\n");
  tests_passed++;

  /* Test 1: PRAGMA lyrore_enabled should be ON by default */
  {
    sqlite3_stmt *stmt;
    rc = sqlite3_prepare_v2(db, "PRAGMA lyrore_enabled", -1, &stmt, NULL);
    if(rc == SQLITE_OK) {
      rc = sqlite3_step(stmt);
      if(rc == SQLITE_ROW && sqlite3_column_int(stmt, 0) == 1) {
        printf("PASS: PRAGMA lyrore_enabled = 1 by default\n");
        tests_passed++;
      } else {
        printf("FAIL: PRAGMA lyrore_enabled expected 1\n");
        tests_failed++;
      }
      sqlite3_finalize(stmt);
    } else {
      printf("FAIL: Could not prepare PRAGMA lyrore_enabled\n");
      tests_failed++;
    }
  }

  /* Test 2: Model registration */
  rc = lyroreRegisterModel(db, "test_cost", &testModelOps, NULL);
  if(rc == SQLITE_OK) {
    printf("PASS: Model registration succeeded\n");
    tests_passed++;
  } else {
    printf("FAIL: Model registration failed with rc=%d\n", rc);
    tests_failed++;
  }

  /* Test 3: Get registered model */
  {
    LyroreModelOps *ops = lyroreGetModel(db, "test_cost");
    if(ops != NULL && ops == &testModelOps) {
      printf("PASS: lyroreGetModel returned correct ops\n");
      tests_passed++;
    } else {
      printf("FAIL: lyroreGetModel returned wrong ops\n");
      tests_failed++;
    }
  }

  /* Test 4: Get model state */
  {
    TestModelState *state = (TestModelState*)lyroreGetModelState(db, "test_cost");
    if(state != NULL) {
      printf("PASS: lyroreGetModelState returned non-NULL\n");
      tests_passed++;
      if(state->correction == 1.0 && state->callCount == 0) {
        printf("PASS: Model state correctly initialized\n");
        tests_passed++;
      } else {
        printf("FAIL: Model state not correctly initialized\n");
        tests_failed++;
      }
    } else {
      printf("FAIL: lyroreGetModelState returned NULL\n");
      tests_failed++;
    }
  }

  /* Test 5: Model prediction */
  {
    TestModelState *state = (TestModelState*)lyroreGetModelState(db, "test_cost");
    LyroreModelOps *ops = lyroreGetModel(db, "test_cost");
    if(state && ops && ops->xPredict) {
      double features[3] = {100.0, 50.0, 25.0};
      double result = ops->xPredict(state, features, 3);
      if(result == 100.0) {
        printf("PASS: Model prediction correct (%.2f)\n", result);
        tests_passed++;
      } else {
        printf("FAIL: Model prediction incorrect (%.2f vs 100.0)\n", result);
        tests_failed++;
      }
    }
  }

  /* Test 6: Feature struct initialization */
  {
    LyroreFeatures feat;
    lyroreInitFeatures(&feat);
    if(feat.nTables == 0 && feat.estCost == 0.0 && feat.aExtended == NULL) {
      printf("PASS: LyroreFeatures initialized to zero\n");
      tests_passed++;
    } else {
      printf("FAIL: LyroreFeatures not properly zeroed\n");
      tests_failed++;
    }
    lyroreFreeFeatures(&feat);
  }

  /* Test 7: Features to array conversion */
  {
    LyroreFeatures feat;
    lyroreInitFeatures(&feat);
    feat.nTables = 3;
    feat.nJoins = 2;
    feat.estCost = 1000.5;
    
    double arr[20];
    int n = lyroreFeaturesToArray(&feat, arr, 20);
    if(n >= 3 && arr[0] == 3.0 && arr[1] == 2.0) {
      printf("PASS: lyroreFeaturesToArray converted %d features\n", n);
      tests_passed++;
    } else {
      printf("FAIL: lyroreFeaturesToArray incorrect\n");
      tests_failed++;
    }
    lyroreFreeFeatures(&feat);
  }

  /* Test 8: Duplicate registration should fail */
  rc = lyroreRegisterModel(db, "test_cost", &testModelOps, NULL);
  if(rc == SQLITE_ERROR) {
    printf("PASS: Duplicate registration correctly rejected\n");
    tests_passed++;
  } else {
    printf("FAIL: Duplicate registration should fail\n");
    tests_failed++;
  }

  /* Test 9: Unregister model */
  rc = lyroreUnregisterModel(db, "test_cost");
  if(rc == SQLITE_OK) {
    printf("PASS: Model unregistration succeeded\n");
    tests_passed++;
  } else {
    printf("FAIL: Model unregistration failed\n");
    tests_failed++;
  }

  /* Test 10: Get unregistered model should return NULL */
  {
    LyroreModelOps *ops = lyroreGetModel(db, "test_cost");
    if(ops == NULL) {
      printf("PASS: lyroreGetModel returns NULL after unregister\n");
      tests_passed++;
    } else {
      printf("FAIL: lyroreGetModel should return NULL after unregister\n");
      tests_failed++;
    }
  }

  sqlite3_close(db);

  printf("\n=== Test Summary ===\n");
  printf("Passed: %d\n", tests_passed);
  printf("Failed: %d\n", tests_failed);
  
  return tests_failed > 0 ? 1 : 0;
}
