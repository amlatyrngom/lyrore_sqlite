/*
** Lyrore C ABI Implementation
** 
** Provides SQL function for plugin registration and hook wrappers.
** The C++ SDK functions are linked directly into the sqlite3 binary.
*/

#ifdef SQLITE_ENABLE_LYRORE

#include "sqliteInt.h"
#include "lyrore_cabi.h"
#include <dlfcn.h>

/* 
** C++ SDK functions are now linked directly into the binary.
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

/*
** Initialize Lyrore for a database connection.
*/
int lyroreInit(sqlite3 *db){
  /* Register SQL functions */
  lyrore_register_functions(db);
  return SQLITE_OK;
}

/*
** Shutdown Lyrore for a database connection.
*/
void lyroreShutdown(sqlite3 *db){
  if( db->pLyrore ){
    lyrore_cpp_destroy(db->pLyrore);
    db->pLyrore = NULL;
  }
}

/*
** Persist state (stub - plugins manage their own state)
*/
void lyrorePersistNow(sqlite3 *db){
  (void)db;
}

/*
** Reset state (stub - plugins manage their own state)  
*/
void lyroreReset(sqlite3 *db){
  (void)db;
}

#endif /* SQLITE_ENABLE_LYRORE */
