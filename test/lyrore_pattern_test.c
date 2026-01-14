/*
** Test file for Lyrore Pattern Matching Framework (Step 1.5)
**
** Compile with (from ~/sqlite_build):
**   gcc -g -I. -I../sqlite/src -I./tsrc -DSQLITE_ENABLE_LYRORE=1 \
**       ../sqlite/test/lyrore_pattern_test.c -o lyrore_pattern_test \
**       libsqlite3.a -lm -lpthread -ldl
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* Include SQLite internals to access Lyrore structures */
#include "sqliteInt.h"
#include "lyrore_model.h"
#include "lyrore_pattern.h"

#define TEST_ASSERT(cond, msg) do { \
  if(!(cond)) { \
    fprintf(stderr, "FAIL: %s (line %d): %s\n", msg, __LINE__, #cond); \
    nFail++; \
  } else { \
    printf("PASS: %s\n", msg); \
    nPass++; \
  } \
} while(0)

static int nPass = 0;
static int nFail = 0;

/* ============================================================================
** Test matcher functions and globals for pattern dispatch testing
** ============================================================================ */
static int gMatcherCallCount = 0;
static int gMatcherReturnValue = 0;

/* Cost matcher that tracks calls and returns configurable value */
static int test_cost_matcher(struct WhereLoop *pLoop, struct Table *pTab, void *ctx){
  (void)pLoop;
  (void)pTab;
  gMatcherCallCount++;
  if(ctx != 0){
    return *((int*)ctx);
  }
  return gMatcherReturnValue;
}

/* Second cost matcher for priority testing */
static int test_cost_matcher_high_priority(struct WhereLoop *pLoop, struct Table *pTab, void *ctx){
  (void)pLoop; (void)pTab; (void)ctx;
  gMatcherCallCount += 100;
  return 1;
}

/* Plan matcher for category isolation test */
static int test_plan_matcher(struct WhereInfo *pWInfo, void *ctx){
  (void)pWInfo; (void)ctx;
  gMatcherCallCount += 1000;
  return 1;
}

/* Expr matcher for category isolation test */
static int test_expr_matcher(struct Parse *pParse, struct Expr *pExpr, void *ctx){
  (void)pParse; (void)pExpr; (void)ctx;
  gMatcherCallCount += 10000;
  return 1;
}

/* ============================================================================
** INFRASTRUCTURE TESTS
** ============================================================================ */

static void test_lyrore_enabled(void){
  sqlite3 *db;
  sqlite3_stmt *pStmt;
  int rc, enabled = 0;

  rc = sqlite3_open(":memory:", &db);
  TEST_ASSERT(rc == SQLITE_OK, "Open in-memory database");

  rc = sqlite3_prepare_v2(db, "PRAGMA lyrore_enabled", -1, &pStmt, 0);
  TEST_ASSERT(rc == SQLITE_OK, "Prepare PRAGMA lyrore_enabled");

  if(sqlite3_step(pStmt) == SQLITE_ROW){
    enabled = sqlite3_column_int(pStmt, 0);
  }
  sqlite3_finalize(pStmt);
  TEST_ASSERT(enabled == 1, "Lyrore is enabled by default");
  sqlite3_close(db);
}

static void test_pattern_state_table(void){
  sqlite3 *db;
  sqlite3_stmt *pStmt;
  int rc, found = 0;
  char *zPath = "/tmp/lyrore_pattern_test.db";

  remove(zPath);
  rc = sqlite3_open(zPath, &db);
  TEST_ASSERT(rc == SQLITE_OK, "Open file database for pattern state test");

  rc = sqlite3_prepare_v2(db, 
    "SELECT name FROM sqlite_master WHERE type='table' AND name='lyrore_pattern_state'",
    -1, &pStmt, 0);
  TEST_ASSERT(rc == SQLITE_OK, "Prepare table check query");

  if(sqlite3_step(pStmt) == SQLITE_ROW) found = 1;
  sqlite3_finalize(pStmt);
  TEST_ASSERT(found == 1, "lyrore_pattern_state table exists");

  sqlite3_close(db);
  remove(zPath);
}

static void test_query_passthrough(void){
  sqlite3 *db;
  sqlite3_stmt *pStmt;
  int rc, sum = 0;

  rc = sqlite3_open(":memory:", &db);
  TEST_ASSERT(rc == SQLITE_OK, "Open in-memory for passthrough test");

  rc = sqlite3_exec(db,
    "CREATE TABLE t1(a INTEGER, b TEXT);"
    "INSERT INTO t1 VALUES(1,'one'),(2,'two'),(3,'three'),(4,'four'),(5,'five');",
    0, 0, 0);
  TEST_ASSERT(rc == SQLITE_OK, "Create and populate test table");

  rc = sqlite3_prepare_v2(db, "SELECT sum(a) FROM t1", -1, &pStmt, 0);
  TEST_ASSERT(rc == SQLITE_OK, "Prepare simple aggregation");
  if(sqlite3_step(pStmt) == SQLITE_ROW) sum = sqlite3_column_int(pStmt, 0);
  sqlite3_finalize(pStmt);
  TEST_ASSERT(sum == 15, "Simple aggregation returns correct result");
  sqlite3_close(db);
}

/* ============================================================================
** PATTERN FRAMEWORK API TESTS
** ============================================================================ */

/* Test 4: Basic pattern registration and retrieval */
static void test_pattern_registration(void){
  sqlite3 *db;
  LyrorePatternDef pat;
  LyrorePatternDef *pRetrieved;
  int rc;

  rc = sqlite3_open(":memory:", &db);
  TEST_ASSERT(rc == SQLITE_OK, "Open DB for pattern registration test");
  TEST_ASSERT(db->pLyrore != 0, "LyroreContext initialized");

  memset(&pat, 0, sizeof(pat));
  pat.id = (LyrorePatternId)(LYRORE_PAT_COST_BASE + 1);
  pat.zName = "test_cost_pattern_1";
  pat.category = LYRORE_CAT_COST;
  pat.priority = 100;
  pat.matcher.xCostMatch = test_cost_matcher;

  rc = lyroreRegisterPattern(db, &pat);
  TEST_ASSERT(rc == SQLITE_OK, "Pattern registration succeeds");

  pRetrieved = lyroreGetPattern(db, (LyrorePatternId)(LYRORE_PAT_COST_BASE + 1));
  TEST_ASSERT(pRetrieved != 0, "Pattern retrieved by ID");
  TEST_ASSERT(strcmp(pRetrieved->zName, "test_cost_pattern_1") == 0, 
              "Retrieved pattern has correct name");
  TEST_ASSERT(pRetrieved->priority == 100, "Retrieved pattern has correct priority");
  TEST_ASSERT(pRetrieved->category == LYRORE_CAT_COST, "Retrieved pattern has correct category");

  sqlite3_close(db);
}

/* Test 5: Pattern unregistration */
static void test_pattern_unregistration(void){
  sqlite3 *db;
  LyrorePatternDef pat;
  LyrorePatternDef *pRetrieved;
  int rc;

  rc = sqlite3_open(":memory:", &db);
  TEST_ASSERT(rc == SQLITE_OK, "Open DB for unregistration test");

  memset(&pat, 0, sizeof(pat));
  pat.id = (LyrorePatternId)(LYRORE_PAT_COST_BASE + 2);
  pat.zName = "test_cost_pattern_2";
  pat.category = LYRORE_CAT_COST;
  pat.priority = 50;

  rc = lyroreRegisterPattern(db, &pat);
  TEST_ASSERT(rc == SQLITE_OK, "Pattern registered for unregistration test");

  pRetrieved = lyroreGetPattern(db, (LyrorePatternId)(LYRORE_PAT_COST_BASE + 2));
  TEST_ASSERT(pRetrieved != 0, "Pattern exists before unregistration");

  rc = lyroreUnregisterPattern(db, (LyrorePatternId)(LYRORE_PAT_COST_BASE + 2));
  TEST_ASSERT(rc == SQLITE_OK, "Unregistration succeeds");

  pRetrieved = lyroreGetPattern(db, (LyrorePatternId)(LYRORE_PAT_COST_BASE + 2));
  TEST_ASSERT(pRetrieved == 0, "Pattern is NULL after unregistration");

  rc = lyroreUnregisterPattern(db, (LyrorePatternId)(LYRORE_PAT_COST_BASE + 999));
  TEST_ASSERT(rc == SQLITE_NOTFOUND, "Unregistering non-existent pattern returns NOTFOUND");

  sqlite3_close(db);
}

/* Test 6: Priority ordering - higher priority patterns checked first */
static void test_priority_ordering(void){
  sqlite3 *db;
  LyrorePatternDef pat1, pat2, pat3;
  LyrorePatternDef *pResult;
  int rc;

  rc = sqlite3_open(":memory:", &db);
  TEST_ASSERT(rc == SQLITE_OK, "Open DB for priority test");

  memset(&pat1, 0, sizeof(pat1));
  pat1.id = (LyrorePatternId)(LYRORE_PAT_COST_BASE + 10);
  pat1.zName = "low_priority_pattern";
  pat1.category = LYRORE_CAT_COST;
  pat1.priority = 10;
  pat1.matcher.xCostMatch = test_cost_matcher;
  rc = lyroreRegisterPattern(db, &pat1);
  TEST_ASSERT(rc == SQLITE_OK, "Low priority pattern registered");

  memset(&pat2, 0, sizeof(pat2));
  pat2.id = (LyrorePatternId)(LYRORE_PAT_COST_BASE + 11);
  pat2.zName = "high_priority_pattern";
  pat2.category = LYRORE_CAT_COST;
  pat2.priority = 100;
  pat2.matcher.xCostMatch = test_cost_matcher_high_priority;
  rc = lyroreRegisterPattern(db, &pat2);
  TEST_ASSERT(rc == SQLITE_OK, "High priority pattern registered");

  memset(&pat3, 0, sizeof(pat3));
  pat3.id = (LyrorePatternId)(LYRORE_PAT_COST_BASE + 12);
  pat3.zName = "medium_priority_pattern";
  pat3.category = LYRORE_CAT_COST;
  pat3.priority = 50;
  pat3.matcher.xCostMatch = test_cost_matcher;
  rc = lyroreRegisterPattern(db, &pat3);
  TEST_ASSERT(rc == SQLITE_OK, "Medium priority pattern registered");

  gMatcherCallCount = 0;
  gMatcherReturnValue = 1;
  pResult = lyroreDispatchCostPattern(db, 0, 0);
  TEST_ASSERT(pResult != 0, "Dispatch returns a pattern");
  TEST_ASSERT(gMatcherCallCount == 100, "High priority matcher called first (increment=100)");
  TEST_ASSERT(strcmp(pResult->zName, "high_priority_pattern") == 0, 
              "Dispatch returns high priority pattern");

  sqlite3_close(db);
}

/* Test 7: Dispatch returns default when no matcher matches */
static void test_dispatch_default_fallback(void){
  sqlite3 *db;
  LyrorePatternDef pat;
  LyrorePatternDef *pResult;
  int rc;

  rc = sqlite3_open(":memory:", &db);
  TEST_ASSERT(rc == SQLITE_OK, "Open DB for default fallback test");

  memset(&pat, 0, sizeof(pat));
  pat.id = (LyrorePatternId)(LYRORE_PAT_COST_BASE + 20);
  pat.zName = "non_matching_pattern";
  pat.category = LYRORE_CAT_COST;
  pat.priority = 100;
  pat.matcher.xCostMatch = test_cost_matcher;
  rc = lyroreRegisterPattern(db, &pat);
  TEST_ASSERT(rc == SQLITE_OK, "Non-matching pattern registered");

  gMatcherCallCount = 0;
  gMatcherReturnValue = 0;
  pResult = lyroreDispatchCostPattern(db, 0, 0);
  
  TEST_ASSERT(pResult != 0, "Dispatch returns a pattern (default)");
  TEST_ASSERT(gMatcherCallCount == 1, "Matcher was called");
  TEST_ASSERT(pResult->id == LYRORE_PAT_DEFAULT, "Dispatch returns default pattern");
  TEST_ASSERT(strcmp(pResult->zName, "default") == 0, "Default pattern has correct name");

  sqlite3_close(db);
}

/* Test 8: Category isolation - cost patterns don't affect plan registry */
static void test_category_isolation(void){
  sqlite3 *db;
  LyrorePatternDef costPat, planPat, exprPat;
  LyrorePatternDef *pCost, *pPlan, *pExpr;
  int rc;

  rc = sqlite3_open(":memory:", &db);
  TEST_ASSERT(rc == SQLITE_OK, "Open DB for category isolation test");

  memset(&costPat, 0, sizeof(costPat));
  costPat.id = (LyrorePatternId)(LYRORE_PAT_COST_BASE + 30);
  costPat.zName = "isolated_cost_pattern";
  costPat.category = LYRORE_CAT_COST;
  costPat.priority = 100;
  costPat.matcher.xCostMatch = test_cost_matcher;
  rc = lyroreRegisterPattern(db, &costPat);
  TEST_ASSERT(rc == SQLITE_OK, "Cost pattern registered");

  memset(&planPat, 0, sizeof(planPat));
  planPat.id = (LyrorePatternId)(LYRORE_PAT_PLAN_BASE + 30);
  planPat.zName = "isolated_plan_pattern";
  planPat.category = LYRORE_CAT_PLAN;
  planPat.priority = 100;
  planPat.matcher.xPlanMatch = test_plan_matcher;
  rc = lyroreRegisterPattern(db, &planPat);
  TEST_ASSERT(rc == SQLITE_OK, "Plan pattern registered");

  memset(&exprPat, 0, sizeof(exprPat));
  exprPat.id = (LyrorePatternId)(LYRORE_PAT_EXPR_BASE + 30);
  exprPat.zName = "isolated_expr_pattern";
  exprPat.category = LYRORE_CAT_EXPR;
  exprPat.priority = 100;
  exprPat.matcher.xExprMatch = test_expr_matcher;
  rc = lyroreRegisterPattern(db, &exprPat);
  TEST_ASSERT(rc == SQLITE_OK, "Expr pattern registered");

  pCost = lyroreGetPattern(db, (LyrorePatternId)(LYRORE_PAT_COST_BASE + 30));
  pPlan = lyroreGetPattern(db, (LyrorePatternId)(LYRORE_PAT_PLAN_BASE + 30));
  pExpr = lyroreGetPattern(db, (LyrorePatternId)(LYRORE_PAT_EXPR_BASE + 30));
  
  TEST_ASSERT(pCost != 0 && strcmp(pCost->zName, "isolated_cost_pattern") == 0,
              "Cost pattern retrieved correctly");
  TEST_ASSERT(pPlan != 0 && strcmp(pPlan->zName, "isolated_plan_pattern") == 0,
              "Plan pattern retrieved correctly");
  TEST_ASSERT(pExpr != 0 && strcmp(pExpr->zName, "isolated_expr_pattern") == 0,
              "Expr pattern retrieved correctly");

  gMatcherCallCount = 0;
  gMatcherReturnValue = 1;
  lyroreDispatchCostPattern(db, 0, 0);
  TEST_ASSERT(gMatcherCallCount == 1, "Cost dispatch only calls cost matcher (count=1)");

  gMatcherCallCount = 0;
  lyroreDispatchPlanPattern(db, 0);
  TEST_ASSERT(gMatcherCallCount == 1000, "Plan dispatch only calls plan matcher (count=1000)");

  gMatcherCallCount = 0;
  lyroreDispatchExprPattern(db, 0, 0);
  TEST_ASSERT(gMatcherCallCount == 10000, "Expr dispatch only calls expr matcher (count=10000)");

  sqlite3_close(db);
}

/* Test 9: Multiple patterns with priority ordering */
static void test_multiple_patterns_priority(void){
  sqlite3 *db;
  LyrorePatternDef pats[5];
  LyrorePatternDef *pResult;
  int rc, i;
  int contextValues[5];
  int priorities[5] = {30, 10, 50, 20, 40};
  
  rc = sqlite3_open(":memory:", &db);
  TEST_ASSERT(rc == SQLITE_OK, "Open DB for multiple patterns test");

  for(i = 0; i < 5; i++){
    memset(&pats[i], 0, sizeof(pats[i]));
    pats[i].id = (LyrorePatternId)(LYRORE_PAT_COST_BASE + 40 + i);
    pats[i].zName = "multi_pattern";
    pats[i].category = LYRORE_CAT_COST;
    pats[i].priority = priorities[i];
    pats[i].matcher.xCostMatch = test_cost_matcher;
    contextValues[i] = 0;
    pats[i].pMatchCtx = &contextValues[i];
    rc = lyroreRegisterPattern(db, &pats[i]);
    TEST_ASSERT(rc == SQLITE_OK, "Multi-pattern registered");
  }

  contextValues[2] = 1;  /* Pattern with priority 50 will match */
  
  gMatcherCallCount = 0;
  pResult = lyroreDispatchCostPattern(db, 0, 0);
  TEST_ASSERT(pResult != 0, "Dispatch returns a pattern");
  TEST_ASSERT(pResult->priority == 50, "Highest matching priority pattern returned");

  sqlite3_close(db);
}

/* Test 10: Get default pattern by ID */
static void test_get_default_pattern(void){
  sqlite3 *db;
  LyrorePatternDef *pDefault;
  int rc;

  rc = sqlite3_open(":memory:", &db);
  TEST_ASSERT(rc == SQLITE_OK, "Open DB for default pattern test");

  pDefault = lyroreGetPattern(db, LYRORE_PAT_DEFAULT);
  TEST_ASSERT(pDefault != 0, "Default pattern can be retrieved by ID");
  TEST_ASSERT(pDefault->id == LYRORE_PAT_DEFAULT, "Default pattern has correct ID");
  TEST_ASSERT(strcmp(pDefault->zName, "default") == 0, "Default pattern has correct name");
  TEST_ASSERT(pDefault->priority == -1, "Default pattern has lowest priority (-1)");

  sqlite3_close(db);
}

/* Test 11: Pattern with NULL matcher returns no match */
static void test_null_matcher(void){
  sqlite3 *db;
  LyrorePatternDef pat;
  LyrorePatternDef *pResult;
  int rc;

  rc = sqlite3_open(":memory:", &db);
  TEST_ASSERT(rc == SQLITE_OK, "Open DB for null matcher test");

  memset(&pat, 0, sizeof(pat));
  pat.id = (LyrorePatternId)(LYRORE_PAT_COST_BASE + 50);
  pat.zName = "null_matcher_pattern";
  pat.category = LYRORE_CAT_COST;
  pat.priority = 1000;
  pat.matcher.xCostMatch = 0;
  rc = lyroreRegisterPattern(db, &pat);
  TEST_ASSERT(rc == SQLITE_OK, "Pattern with NULL matcher registered");

  gMatcherCallCount = 0;
  pResult = lyroreDispatchCostPattern(db, 0, 0);
  TEST_ASSERT(pResult != 0, "Dispatch returns a pattern");
  TEST_ASSERT(pResult->id == LYRORE_PAT_DEFAULT, "NULL matcher pattern skipped, default returned");
  TEST_ASSERT(gMatcherCallCount == 0, "No matcher was called");

  sqlite3_close(db);
}

/* Test 12: Registry survives pattern operations */
static void test_registry_integrity(void){
  sqlite3 *db;
  LyrorePatternDef pat;
  LyrorePatternDef *pResult;
  int rc, i;

  rc = sqlite3_open(":memory:", &db);
  TEST_ASSERT(rc == SQLITE_OK, "Open DB for registry integrity test");

  for(i = 0; i < 10; i++){
    memset(&pat, 0, sizeof(pat));
    pat.id = (LyrorePatternId)(LYRORE_PAT_COST_BASE + 60 + i);
    pat.zName = "integrity_test_pattern";
    pat.category = LYRORE_CAT_COST;
    pat.priority = i * 10;
    pat.matcher.xCostMatch = test_cost_matcher;
    rc = lyroreRegisterPattern(db, &pat);
    TEST_ASSERT(rc == SQLITE_OK, "Pattern registered in loop");
  }

  for(i = 0; i < 10; i += 2){
    rc = lyroreUnregisterPattern(db, (LyrorePatternId)(LYRORE_PAT_COST_BASE + 60 + i));
    TEST_ASSERT(rc == SQLITE_OK, "Pattern unregistered in loop");
  }

  for(i = 1; i < 10; i += 2){
    pResult = lyroreGetPattern(db, (LyrorePatternId)(LYRORE_PAT_COST_BASE + 60 + i));
    TEST_ASSERT(pResult != 0, "Remaining pattern still accessible");
    TEST_ASSERT(pResult->priority == i * 10, "Remaining pattern has correct priority");
  }

  for(i = 0; i < 10; i += 2){
    pResult = lyroreGetPattern(db, (LyrorePatternId)(LYRORE_PAT_COST_BASE + 60 + i));
    TEST_ASSERT(pResult == 0, "Removed pattern returns NULL");
  }

  gMatcherCallCount = 0;
  gMatcherReturnValue = 1;
  pResult = lyroreDispatchCostPattern(db, 0, 0);
  TEST_ASSERT(pResult != 0, "Dispatch works after partial unregistration");
  TEST_ASSERT(pResult->id != LYRORE_PAT_DEFAULT, "Non-default pattern matched");

  sqlite3_close(db);
}

/* Test 13: Pattern dispatch with match context */
static void test_matcher_context(void){
  sqlite3 *db;
  LyrorePatternDef pat;
  LyrorePatternDef *pResult;
  int rc;
  int contextValue;

  rc = sqlite3_open(":memory:", &db);
  TEST_ASSERT(rc == SQLITE_OK, "Open DB for matcher context test");

  memset(&pat, 0, sizeof(pat));
  pat.id = (LyrorePatternId)(LYRORE_PAT_COST_BASE + 70);
  pat.zName = "context_pattern";
  pat.category = LYRORE_CAT_COST;
  pat.priority = 100;
  pat.matcher.xCostMatch = test_cost_matcher;
  contextValue = 0;
  pat.pMatchCtx = &contextValue;
  rc = lyroreRegisterPattern(db, &pat);
  TEST_ASSERT(rc == SQLITE_OK, "Context pattern registered");

  pResult = lyroreDispatchCostPattern(db, 0, 0);
  TEST_ASSERT(pResult->id == LYRORE_PAT_DEFAULT, "Pattern doesn't match with context=0");

  contextValue = 1;
  pResult = lyroreDispatchCostPattern(db, 0, 0);
  TEST_ASSERT(pResult->id == (LyrorePatternId)(LYRORE_PAT_COST_BASE + 70), 
              "Pattern matches with context=1");

  sqlite3_close(db);
}

/* Test 14: Error handling - NULL database */
static void test_error_null_db(void){
  LyrorePatternDef pat;
  LyrorePatternDef *pResult;
  int rc;

  memset(&pat, 0, sizeof(pat));
  pat.id = (LyrorePatternId)(LYRORE_PAT_COST_BASE + 80);
  pat.zName = "error_test_pattern";
  pat.category = LYRORE_CAT_COST;
  pat.priority = 100;

  rc = lyroreRegisterPattern(0, &pat);
  TEST_ASSERT(rc == SQLITE_ERROR, "Register with NULL db returns error");

  rc = lyroreUnregisterPattern(0, (LyrorePatternId)(LYRORE_PAT_COST_BASE + 80));
  TEST_ASSERT(rc == SQLITE_ERROR, "Unregister with NULL db returns error");

  pResult = lyroreGetPattern(0, (LyrorePatternId)(LYRORE_PAT_COST_BASE + 80));
  TEST_ASSERT(pResult == 0, "GetPattern with NULL db returns NULL");

  pResult = lyroreDispatchCostPattern(0, 0, 0);
  TEST_ASSERT(pResult == 0, "Dispatch with NULL db returns NULL");
}

/* Test 15: PRAGMA persist with patterns */
static void test_pragma_with_patterns(void){
  sqlite3 *db;
  LyrorePatternDef pat;
  int rc;
  char *zPath = "/tmp/lyrore_pattern_pragma_test.db";

  remove(zPath);

  rc = sqlite3_open(zPath, &db);
  TEST_ASSERT(rc == SQLITE_OK, "Open DB for pragma with patterns test");

  memset(&pat, 0, sizeof(pat));
  pat.id = (LyrorePatternId)(LYRORE_PAT_COST_BASE + 90);
  pat.zName = "pragma_test_pattern";
  pat.category = LYRORE_CAT_COST;
  pat.priority = 100;
  rc = lyroreRegisterPattern(db, &pat);
  TEST_ASSERT(rc == SQLITE_OK, "Pattern registered for pragma test");

  rc = sqlite3_exec(db, "PRAGMA lyrore_persist", 0, 0, 0);
  TEST_ASSERT(rc == SQLITE_OK, "PRAGMA lyrore_persist works with patterns");

  rc = sqlite3_exec(db, "PRAGMA lyrore_reset", 0, 0, 0);
  TEST_ASSERT(rc == SQLITE_OK, "PRAGMA lyrore_reset works with patterns");

  sqlite3_close(db);
  remove(zPath);
}

/* Test 16: Empty registry dispatch returns default */
static void test_empty_registry_dispatch(void){
  sqlite3 *db;
  LyrorePatternDef *pResult;
  int rc;

  rc = sqlite3_open(":memory:", &db);
  TEST_ASSERT(rc == SQLITE_OK, "Open DB for empty registry test");

  /* No patterns registered - dispatch should return default */
  pResult = lyroreDispatchCostPattern(db, 0, 0);
  TEST_ASSERT(pResult != 0, "Dispatch on empty registry returns non-NULL");
  TEST_ASSERT(pResult->id == LYRORE_PAT_DEFAULT, "Empty registry returns default pattern");

  pResult = lyroreDispatchPlanPattern(db, 0);
  TEST_ASSERT(pResult != 0, "Plan dispatch on empty registry returns non-NULL");
  TEST_ASSERT(pResult->id == LYRORE_PAT_DEFAULT, "Empty plan registry returns default");

  pResult = lyroreDispatchExprPattern(db, 0, 0);
  TEST_ASSERT(pResult != 0, "Expr dispatch on empty registry returns non-NULL");
  TEST_ASSERT(pResult->id == LYRORE_PAT_DEFAULT, "Empty expr registry returns default");

  sqlite3_close(db);
}

int main(int argc, char **argv){
  (void)argc;
  (void)argv;

  printf("=== Lyrore Pattern Matching Framework Tests ===\n\n");
  printf("--- Infrastructure Tests ---\n");
  test_lyrore_enabled();
  test_pattern_state_table();
  test_query_passthrough();

  printf("\n--- Pattern Framework API Tests ---\n");
  test_pattern_registration();
  test_pattern_unregistration();
  test_priority_ordering();
  test_dispatch_default_fallback();
  test_category_isolation();
  test_multiple_patterns_priority();
  test_get_default_pattern();
  test_null_matcher();
  test_registry_integrity();
  test_matcher_context();
  test_error_null_db();
  test_pragma_with_patterns();
  test_empty_registry_dispatch();

  printf("\n=== Test Summary ===\n");
  printf("Passed: %d\n", nPass);
  printf("Failed: %d\n", nFail);
  printf("Total:  %d\n", nPass + nFail);

  return nFail > 0 ? 1 : 0;
}
