/*
** Lyrore C ABI Implementation
** 
** Provides hook wrappers for LyroreMain callbacks and
** internal SQLite function wrappers for C++ SDK access.
*/

#ifdef SQLITE_ENABLE_LYRORE

#include "sqliteInt.h"
#include "lyrore_cabi.h"

/*
** Hook wrapper functions - called from SQLite core
** These call through function pointers registered by LyroreMain
*/

/* Function pointers for LyroreMain callbacks */
static LyrorePreParseFunc g_preparse_func = 0;
static LyrorePreOptFunc g_preopt_main_func = 0;
static LyroreEstimateFunc g_estimate_main_func = 0;
static LyrorePostQueryFunc g_postquery_main_func = 0;
static LyroreAnalyzeFunc g_analyze_main_func = 0;

int lyroreInvokePreOptHooks(sqlite3 *db, void *pParse, void *pSelect){
  if( !(db->flags & SQLITE_LyroreEnabled) ) return SQLITE_OK;
  if( db->pLyroreMain && g_preopt_main_func ){
    return g_preopt_main_func(db->pLyroreMain, db, pParse, pSelect);
  }
  return SQLITE_OK;
}

void lyroreInvokeEstimateHooks(sqlite3 *db, void *pBuilder, void *pLoop){
  if( !(db->flags & SQLITE_LyroreCost) ) return;
  if( db->pLyroreMain && g_estimate_main_func ){
    g_estimate_main_func(db->pLyroreMain, db, pBuilder, pLoop);
  }
}

void lyroreInvokeAnalyzeHooks(sqlite3 *db, int iDb){
  if( !(db->flags & SQLITE_LyroreEnabled) ) return;
  if( db->pLyroreMain && g_analyze_main_func ){
    g_analyze_main_func(db->pLyroreMain, db, iDb);
  }
}

void lyroreInvokePostQueryHooks(sqlite3 *db, void *pVdbe){
  if( !(db->flags & SQLITE_LyroreEnabled) ) return;
  if( db->pLyroreMain && g_postquery_main_func ){
    g_postquery_main_func(db->pLyroreMain, db, pVdbe);
  }
}

/*
** Pre-parse hook for SQL transformation
** Called from prepare.c before sqlite3RunParser()
** Uses thread-local storage for the modified SQL string
*/
static __thread char* g_preparse_modified_sql = 0;

/*
** Register LyroreMain callbacks.
** Called from C++ when LyroreMain singleton is created.
*/
void lyrore_register_main_callbacks(
    LyrorePreParseFunc preparse_func,
    LyrorePreOptFunc preopt_func,
    LyroreEstimateFunc estimate_func,
    LyrorePostQueryFunc postquery_func,
    LyroreAnalyzeFunc analyze_func
){
  g_preparse_func = preparse_func;
  g_preopt_main_func = preopt_func;
  g_estimate_main_func = estimate_func;
  g_postquery_main_func = postquery_func;
  g_analyze_main_func = analyze_func;
}

/*
** Set/get LyroreMain pointer for a database connection.
*/
void lyrore_main_set_db(sqlite3* db, LyroreMainHandle* main){
  db->pLyroreMain = main;
}

LyroreMainHandle* lyrore_main_get_db(sqlite3* db){
  return db->pLyroreMain;
}

int lyroreInvokePreParseHook(sqlite3 *db, const char** pzSql){
  char* zModified = 0;
  int rc;

  /* Free any previous modified SQL */
  if( g_preparse_modified_sql ){
    sqlite3_free(g_preparse_modified_sql);
    g_preparse_modified_sql = 0;
  }

  /* Check if Lyrore is enabled */
  if( !(db->flags & SQLITE_LyroreEnabled) ){
    return SQLITE_OK;
  }

  /* Call through function pointer if LyroreMain is available */
  if( db->pLyroreMain && g_preparse_func ){
    rc = g_preparse_func(db, *pzSql, &zModified);
    if( rc!=SQLITE_OK ){
      return rc;
    }
    if( zModified ){
      g_preparse_modified_sql = zModified;
      *pzSql = g_preparse_modified_sql;
    }
  }

  return SQLITE_OK;
}

/* Cleanup function for thread-local modified SQL */
void lyrorePreParseCleanup(void){
  if( g_preparse_modified_sql ){
    sqlite3_free(g_preparse_modified_sql);
    g_preparse_modified_sql = 0;
  }
}

/* ============================================================
** Lyrore Lifecycle Functions
** ============================================================ */

/*
** Initialize Lyrore for a database connection.
** Called from sqlite3_open* after basic setup.
** LyroreMain will set db->pLyroreMain when it manages the connection.
*/
int lyroreInit(sqlite3 *db){
  (void)db;
  return SQLITE_OK;
}

/*
** Shutdown Lyrore for a database connection.
** Called from sqlite3_close* before cleanup.
*/
void lyroreShutdown(sqlite3 *db){
  /* LyroreMain manages its own cleanup */
  db->pLyroreMain = 0;
}

/*
** Persist Lyrore state (placeholder for future use).
*/
void lyrorePersistNow(sqlite3 *db){
  (void)db;
}

/*
** Reset Lyrore state.
*/
void lyroreReset(sqlite3 *db){
  (void)db;
}

/* ============================================================
** Internal SQLite Function Wrappers for C++ SDK
** These wrap SQLITE_PRIVATE functions for external linkage
** ============================================================ */

Expr* lyrore_sqlite3Expr(sqlite3* db, int op, const char* zToken){
  return sqlite3Expr(db, op, zToken);
}

Expr* lyrore_sqlite3ExprDup(sqlite3* db, Expr* pExpr, int dupFlags){
  return sqlite3ExprDup(db, pExpr, dupFlags);
}

void lyrore_sqlite3ExprDelete(sqlite3* db, Expr* pExpr){
  sqlite3ExprDelete(db, pExpr);
}

ExprList* lyrore_sqlite3ExprListAppend(Parse* pParse, ExprList* pList, Expr* pExpr){
  return sqlite3ExprListAppend(pParse, pList, pExpr);
}

void lyrore_sqlite3ExprListDelete(sqlite3* db, ExprList* pList){
  sqlite3ExprListDelete(db, pList);
}

Select* lyrore_sqlite3SelectDup(sqlite3* db, Select* pSelect, int flags){
  return sqlite3SelectDup(db, pSelect, flags);
}

void lyrore_sqlite3SelectDelete(sqlite3* db, Select* pSelect){
  sqlite3SelectDelete(db, pSelect);
}

void lyrore_sqlite3DbFree(sqlite3* db, void* p){
  sqlite3DbFree(db, p);
}

int lyrore_sqlite3StrICmp(const char* zLeft, const char* zRight){
  return sqlite3StrICmp(zLeft, zRight);
}

char* lyrore_sqlite3DbStrDup(sqlite3* db, const char* z){
  return sqlite3DbStrDup(db, z);
}

LogEst_wrapper lyrore_sqlite3LogEst(u64_wrapper x){
  return (LogEst_wrapper)sqlite3LogEst((u64)x);
}

u64_wrapper lyrore_sqlite3LogEstToInt(LogEst_wrapper x){
  return (u64_wrapper)sqlite3LogEstToInt((LogEst)x);
}

/* SrcList wrapper functions */
SrcList* lyrore_sqlite3SrcListAppend(Parse* pParse, SrcList* pList, 
                                     Token* pTable, Token* pDatabase){
  return sqlite3SrcListAppend(pParse, pList, pTable, pDatabase);
}

void lyrore_sqlite3SrcListDelete(sqlite3* db, SrcList* pList){
  sqlite3SrcListDelete(db, pList);
}

/* Additional functions for AST rewrite support */
Table* lyrore_sqlite3FindTable(sqlite3* db, const char* zName, const char* zDatabase){
  return sqlite3FindTable(db, zName, zDatabase);
}

void lyrore_sqlite3SrcListAssignCursors(Parse* pParse, SrcList* pList){
  sqlite3SrcListAssignCursors(pParse, pList);
}

int lyrore_get_parse_nTab(Parse* pParse){
  return pParse ? pParse->nTab : 0;
}

#endif /* SQLITE_ENABLE_LYRORE */

void* lyrore_sqlite3DbMallocZero(sqlite3* db, u64 n){
  return sqlite3DbMallocZero(db, n);
}
