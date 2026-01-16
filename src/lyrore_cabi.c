/*
** Lyrore C ABI Implementation
** 
** Provides SQL function for plugin registration, hook wrappers,
** and internal SQLite function wrappers for C++ SDK access.
*/

#ifdef SQLITE_ENABLE_LYRORE

#include "sqliteInt.h"
#include "lyrore_cabi.h"
#include <dlfcn.h>

/* 
** C++ SDK functions are linked directly into the binary.
** These are declared in lyrore_cabi.h and implemented in cpp_context.cpp.
*/

/*
** SQL function: lyrore_register(path)
** Loads a plugin from the given path.
*/
static void lyroreRegisterFunc(
  sqlite3_context *context,
  int argc,
  sqlite3_value **argv
){
  sqlite3 *db = sqlite3_context_db_handle(context);
  const char *path;
  int rc;

  if( argc!=1 ){
    sqlite3_result_error(context, "lyrore_register requires exactly 1 argument", -1);
    return;
  }

  /* Check if plugins are enabled */
  if( !(db->flags & SQLITE_LyrorePlugins) ){
    sqlite3_result_error(context, "Plugins disabled. Use: PRAGMA lyrore_plugins=ON", -1);
    return;
  }

  path = (const char*)sqlite3_value_text(argv[0]);
  if( !path ){
    sqlite3_result_error(context, "Invalid plugin path", -1);
    return;
  }

  /* Create C++ context if needed */
  if( !db->pLyrore ){
    db->pLyrore = lyrore_cpp_create(db);
    if( !db->pLyrore ){
      sqlite3_result_error(context, "Failed to create Lyrore context", -1);
      return;
    }
  }

  /* Load the plugin via C++ SDK */
  rc = lyrore_cpp_load_plugin(db->pLyrore, path);
  if( rc!=SQLITE_OK ){
    sqlite3_result_error(context, "Failed to load plugin", -1);
    return;
  }

  sqlite3_result_text(context, "Plugin loaded successfully", -1, SQLITE_STATIC);
}

/*
** Register the lyrore_register SQL function.
*/
void lyrore_register_functions(sqlite3* db){
  sqlite3_create_function(db, "lyrore_register", 1, 
                          SQLITE_UTF8|SQLITE_DIRECTONLY, 
                          NULL, lyroreRegisterFunc, NULL, NULL);
}

/*
** Hook wrapper functions - called from SQLite core
*/

int lyroreInvokePreOptHooks(sqlite3 *db, void *pParse, void *pSelect){
  LyroreCppContext *ctx = db->pLyrore;
  if( !ctx ) return SQLITE_OK;
  if( !(db->flags & SQLITE_LyroreEnabled) ) return SQLITE_OK;
  return lyrore_cpp_invoke_preopt(ctx, pParse, pSelect);
}

void lyroreInvokeEstimateHooks(sqlite3 *db, void *pBuilder, void *pLoop){
  LyroreCppContext *ctx = db->pLyrore;
  if( !ctx ) return;
  if( !(db->flags & SQLITE_LyroreCost) ) return;
  lyrore_cpp_invoke_estimate(ctx, pBuilder, pLoop);
}

void lyroreInvokeAnalyzeHooks(sqlite3 *db, int iDb){
  LyroreCppContext *ctx = db->pLyrore;
  if( !ctx ) return;
  if( !(db->flags & SQLITE_LyroreEnabled) ) return;
  lyrore_cpp_invoke_analyze(ctx, iDb);
}

void lyroreInvokePostQueryHooks(sqlite3 *db, void *pVdbe){
  LyroreCppContext *ctx = db->pLyrore;
  if( !ctx ) return;
  if( !(db->flags & SQLITE_LyroreEnabled) ) return;
  lyrore_cpp_invoke_postquery(ctx, pVdbe);
}


/* ============================================================
** Lyrore Lifecycle Functions
** Called from main.c and pragma.c
** ============================================================ */

/*
** Initialize Lyrore for a database connection.
** Called from sqlite3_open* after basic setup.
*/
int lyroreInit(sqlite3 *db){
  /* Register lyrore_register() SQL function */
  lyrore_register_functions(db);

  /* Context is created lazily when first plugin is loaded */
  return SQLITE_OK;
}

/*
** Shutdown Lyrore for a database connection.
** Called from sqlite3_close* before cleanup.
*/
void lyroreShutdown(sqlite3 *db){
  if( db->pLyrore ){
    lyrore_cpp_destroy(db->pLyrore);
    db->pLyrore = 0;
  }
}

/*
** Persist Lyrore state (placeholder for future use).
** Called by PRAGMA lyrore_persist.
*/
void lyrorePersistNow(sqlite3 *db){
  /* Currently a no-op - state persistence not yet implemented */
  (void)db;
}

/*
** Reset Lyrore state.
** Called by PRAGMA lyrore_reset.
*/
void lyroreReset(sqlite3 *db){
  if( db->pLyrore ){
    /* Destroy and recreate context to reset state */
    lyrore_cpp_destroy(db->pLyrore);
    db->pLyrore = 0;
  }
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

LogEst_wrapper lyrore_sqlite3LogEst(u64_wrapper x){
  return (LogEst_wrapper)sqlite3LogEst((u64)x);
}

u64_wrapper lyrore_sqlite3LogEstToInt(LogEst_wrapper x){
  return (u64_wrapper)sqlite3LogEstToInt((LogEst)x);
}

#endif /* SQLITE_ENABLE_LYRORE */
