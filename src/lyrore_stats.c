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
** This file contains the implementation of Lyrore state management,
** including initialization, persistence, and execution history tracking.
**
** CRITICAL: Uses a dedicated state connection to prevent conflicts
** with user transactions.
*/
#include "sqliteInt.h"

#ifdef SQLITE_ENABLE_LYRORE
#include "lyrore_model.h"
#include "lyrore_stats.h"
#include "lyrore_pattern.h"

/*
** Static flag to prevent recursive initialization when
** opening the state database connection. This is safe because
** recursion happens within the same call stack.
*/
static int lyroreInitializing = 0;

/*
** SQL statements for state table creation
*/
static const char *azCreateStateTables[] = {
  "CREATE TABLE IF NOT EXISTS lyrore_model_state("
    "purpose TEXT, "
    "context_key TEXT, "
    "state_blob BLOB, "
    "sample_count INT, "
    "last_updated INT, "
    "PRIMARY KEY(purpose, context_key))",

  "CREATE TABLE IF NOT EXISTS lyrore_exec_history("
    "pattern_sig TEXT PRIMARY KEY, "
    "exec_count INT, "
    "total_time_us INT, "
    "last_exec_time_us INT)",

  "CREATE TABLE IF NOT EXISTS lyrore_column_stats("
    "tbl TEXT, "
    "col TEXT, "
    "distinct_count INT, "
    "total_count INT, "
    "PRIMARY KEY(tbl, col))",

  "CREATE TABLE IF NOT EXISTS lyrore_pattern_state("
    "pattern_name TEXT NOT NULL, "
    "model_slot TEXT NOT NULL, "
    "state_blob BLOB, "
    "sample_count INTEGER DEFAULT 0, "
    "last_updated INTEGER, "
    "PRIMARY KEY(pattern_name, model_slot))",

  0  /* Sentinel */
};

/*
** Initialize Lyrore context for a database connection.
** Called from openDatabase() in main.c.
**
** This creates the LyroreContext, opens a dedicated state connection,
** and creates the state tables.
*/
int lyroreInit(sqlite3 *db){
  LyroreContext *ctx;
  const char *zPath;
  int rc = SQLITE_OK;
  int i;

  if( db==0 ) return SQLITE_ERROR;

  /* Prevent recursive initialization when opening state DB */
  if( lyroreInitializing ){
    return SQLITE_OK;
  }

  /* Allocate context */
  ctx = sqlite3MallocZero(sizeof(*ctx));
  if( ctx==0 ) return SQLITE_NOMEM;

  ctx->persistThreshold = 100;  /* Default: persist every 100 queries */
  ctx->pModels = 0;
  ctx->nModels = 0;
  ctx->dirty = 0;
  ctx->pStateDb = 0;

  /* Initialize pattern registries (Step 1.5) */
  lyroreInitPatternRegistry(&ctx->costPatterns);
  lyroreInitPatternRegistry(&ctx->planPatterns);
  lyroreInitPatternRegistry(&ctx->exprPatterns);

  /* Get database path */
  zPath = sqlite3_db_filename(db, "main");

  /* Open dedicated state connection (skip for in-memory DBs) */
  if( zPath && zPath[0] && strcmp(zPath, ":memory:")!=0 ){
    /* Set guard to prevent recursion */
    lyroreInitializing = 1;

    rc = sqlite3_open_v2(zPath, &ctx->pStateDb,
                         SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, 0);

    /* Clear guard */
    lyroreInitializing = 0;

    if( rc!=SQLITE_OK ){
      sqlite3_free(ctx);
      return rc;
    }

    /* Create state tables */
    for(i = 0; azCreateStateTables[i]; i++){
      rc = sqlite3_exec(ctx->pStateDb, azCreateStateTables[i], 0, 0, 0);
      if( rc!=SQLITE_OK ){
        sqlite3_close(ctx->pStateDb);
        sqlite3_free(ctx);
        return rc;
      }
    }
  }

  db->pLyrore = ctx;
  db->flags |= SQLITE_LyroreEnabled;

  return SQLITE_OK;
}

/*
** Shutdown Lyrore context for a database connection.
** Called from sqlite3Close() in main.c.
**
** This persists any dirty state, destroys registered models,
** and closes the state connection.
*/
void lyroreShutdown(sqlite3 *db){
  LyroreContext *ctx;
  LyroreModelEntry *pEntry;
  LyroreModelEntry *pNext;

  if( db==0 ) return;
  ctx = db->pLyrore;
  if( ctx==0 ) return;

  /* Persist any dirty state (including pattern state) */
  if( ctx->dirty > 0 ){
    lyrorePersistNow(db);
  }

  /* Persist and destroy pattern registries (Step 1.5) */
  lyrorePersistAllPatterns(db);
  lyroreDestroyPatternRegistry(&ctx->costPatterns);
  lyroreDestroyPatternRegistry(&ctx->planPatterns);
  lyroreDestroyPatternRegistry(&ctx->exprPatterns);

  /* Destroy all registered models */
  pEntry = ctx->pModels;
  while( pEntry ){
    pNext = pEntry->pNext;
    if( pEntry->pOps && pEntry->pOps->xDestroy ){
      pEntry->pOps->xDestroy(pEntry->pState);
    }
    sqlite3_free(pEntry->zPurpose);
    sqlite3_free(pEntry);
    pEntry = pNext;
  }

  /* Close state connection */
  if( ctx->pStateDb ){
    sqlite3_close(ctx->pStateDb);
  }

  sqlite3_free(ctx);
  db->pLyrore = 0;
}

/*
** Maybe persist state if dirty count exceeds threshold.
*/
int lyroreMaybePersist(sqlite3 *db){
  LyroreContext *ctx;
  if( db==0 ) return SQLITE_OK;
  ctx = db->pLyrore;
  if( ctx==0 || ctx->dirty < ctx->persistThreshold ) return SQLITE_OK;
  return lyrorePersistNow(db);
}

/*
** Force persist all model state now.
*/
int lyrorePersistNow(sqlite3 *db){
  LyroreContext *ctx;
  LyroreModelEntry *pEntry;
  int rc;

  if( db==0 ) return SQLITE_OK;
  ctx = db->pLyrore;
  if( ctx==0 || ctx->pStateDb==0 ) return SQLITE_OK;  /* No-op for in-memory */

  rc = sqlite3_exec(ctx->pStateDb, "BEGIN IMMEDIATE", 0, 0, 0);
  if( rc!=SQLITE_OK ) return rc;

  /* Save each model that has xSave */
  pEntry = ctx->pModels;
  while( pEntry ){
    if( pEntry->pOps && pEntry->pOps->xSave && pEntry->pState ){
      rc = pEntry->pOps->xSave(pEntry->pState, ctx->pStateDb);
      if( rc!=SQLITE_OK ){
        sqlite3_exec(ctx->pStateDb, "ROLLBACK", 0, 0, 0);
        return rc;
      }
    }
    pEntry = pEntry->pNext;
  }

  rc = sqlite3_exec(ctx->pStateDb, "COMMIT", 0, 0, 0);
  if( rc==SQLITE_OK ){
    ctx->dirty = 0;
  }
  return rc;
}

/*
** Reset all Lyrore state (clear tables and model state).
*/
int lyroreReset(sqlite3 *db){
  LyroreContext *ctx;
  if( db==0 ) return SQLITE_OK;
  ctx = db->pLyrore;
  if( ctx==0 ) return SQLITE_OK;

  /* Clear state tables */
  if( ctx->pStateDb ){
    sqlite3_exec(ctx->pStateDb, "DELETE FROM lyrore_model_state", 0, 0, 0);
    sqlite3_exec(ctx->pStateDb, "DELETE FROM lyrore_exec_history", 0, 0, 0);
    sqlite3_exec(ctx->pStateDb, "DELETE FROM lyrore_column_stats", 0, 0, 0);
    sqlite3_exec(ctx->pStateDb, "DELETE FROM lyrore_pattern_state", 0, 0, 0);
  }

  /* Reset dirty counter */
  ctx->dirty = 0;

  return SQLITE_OK;
}

/*
** Record query execution for history tracking.
*/
int lyroreRecordExecution(sqlite3 *db, const char *zPatternSig, i64 execTimeUs){
  LyroreContext *ctx;
  sqlite3_stmt *pStmt = 0;
  int rc;

  if( db==0 ) return SQLITE_OK;
  ctx = db->pLyrore;
  if( ctx==0 || ctx->pStateDb==0 || zPatternSig==0 ) return SQLITE_OK;

  rc = sqlite3_prepare_v2(ctx->pStateDb,
    "INSERT INTO lyrore_exec_history(pattern_sig, exec_count, total_time_us, last_exec_time_us) "
    "VALUES(?1, 1, ?2, ?2) "
    "ON CONFLICT(pattern_sig) DO UPDATE SET "
    "exec_count = exec_count + 1, "
    "total_time_us = total_time_us + ?2, "
    "last_exec_time_us = ?2",
    -1, &pStmt, 0);

  if( rc==SQLITE_OK ){
    sqlite3_bind_text(pStmt, 1, zPatternSig, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(pStmt, 2, execTimeUs);
    rc = sqlite3_step(pStmt);
    if( rc==SQLITE_DONE ) rc = SQLITE_OK;
    sqlite3_finalize(pStmt);
  }

  ctx->dirty++;
  return rc;
}

/*
** Get execution history for a pattern signature.
*/
int lyroreGetExecHistory(sqlite3 *db, const char *zPatternSig,
                         int *pExecCount, i64 *pTotalTimeUs, i64 *pLastTimeUs){
  LyroreContext *ctx;
  sqlite3_stmt *pStmt = 0;
  int rc;

  if( db==0 ) return SQLITE_NOTFOUND;
  ctx = db->pLyrore;
  if( ctx==0 || ctx->pStateDb==0 ) return SQLITE_NOTFOUND;

  rc = sqlite3_prepare_v2(ctx->pStateDb,
    "SELECT exec_count, total_time_us, last_exec_time_us "
    "FROM lyrore_exec_history WHERE pattern_sig = ?1",
    -1, &pStmt, 0);

  if( rc==SQLITE_OK ){
    sqlite3_bind_text(pStmt, 1, zPatternSig, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(pStmt);
    if( rc==SQLITE_ROW ){
      if( pExecCount ) *pExecCount = sqlite3_column_int(pStmt, 0);
      if( pTotalTimeUs ) *pTotalTimeUs = sqlite3_column_int64(pStmt, 1);
      if( pLastTimeUs ) *pLastTimeUs = sqlite3_column_int64(pStmt, 2);
      rc = SQLITE_OK;
    }else{
      rc = SQLITE_NOTFOUND;
    }
    sqlite3_finalize(pStmt);
  }

  return rc;
}

/*
** Update column statistics (for future selectivity estimation).
*/
int lyroreUpdateColumnStats(sqlite3 *db, const char *zTable, const char *zColumn,
                            int distinctCount, int totalCount){
  LyroreContext *ctx;
  sqlite3_stmt *pStmt = 0;
  int rc;

  if( db==0 ) return SQLITE_OK;
  ctx = db->pLyrore;
  if( ctx==0 || ctx->pStateDb==0 || zTable==0 || zColumn==0 ) return SQLITE_OK;

  rc = sqlite3_prepare_v2(ctx->pStateDb,
    "INSERT OR REPLACE INTO lyrore_column_stats(tbl, col, distinct_count, total_count) "
    "VALUES(?1, ?2, ?3, ?4)",
    -1, &pStmt, 0);

  if( rc==SQLITE_OK ){
    sqlite3_bind_text(pStmt, 1, zTable, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(pStmt, 2, zColumn, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(pStmt, 3, distinctCount);
    sqlite3_bind_int(pStmt, 4, totalCount);
    rc = sqlite3_step(pStmt);
    if( rc==SQLITE_DONE ) rc = SQLITE_OK;
    sqlite3_finalize(pStmt);
  }

  ctx->dirty++;
  return rc;
}

#endif /* SQLITE_ENABLE_LYRORE */
