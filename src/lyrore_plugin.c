/*
** 2024-01-14
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** This file contains the implementation of Lyrore plugin management.
** Plugins are loaded via the VFS dlopen API for cross-platform support.
*/
#include "sqliteInt.h"
#ifdef SQLITE_ENABLE_LYRORE

#include "lyrore_model.h"
#include "lyrore_hooks.h"
#include "lyrore_plugin.h"

/*
** Helper: Register all hooks from a plugin into the connection.
*/
static int registerPluginHooks(sqlite3 *db, LyrorePluginInfo *pInfo){
  int i, rc;

  /* Register pre-opt hooks */
  for(i = 0; i < pInfo->nPreOptHooks; i++){
    rc = lyroreRegisterPreOptHook(db, &pInfo->aPreOptHooks[i]);
    if( rc != SQLITE_OK ) return rc;
  }

  /* Register estimate hooks */
  for(i = 0; i < pInfo->nEstimateHooks; i++){
    rc = lyroreRegisterEstimateHook(db, &pInfo->aEstimateHooks[i]);
    if( rc != SQLITE_OK ) return rc;
  }

  /* Register post-query hooks */
  for(i = 0; i < pInfo->nPostQueryHooks; i++){
    rc = lyroreRegisterPostQueryHook(db, &pInfo->aPostQueryHooks[i]);
    if( rc != SQLITE_OK ) return rc;
  }

  /* Register analyze hooks */
  for(i = 0; i < pInfo->nAnalyzeHooks; i++){
    rc = lyroreRegisterAnalyzeHook(db, &pInfo->aAnalyzeHooks[i]);
    if( rc != SQLITE_OK ) return rc;
  }

  return SQLITE_OK;
}

/*
** Load a plugin from the given path.
** Security: Requires SQLITE_LyrorePlugins flag to be set.
*/
int lyroreLoadPlugin(sqlite3 *db, const char *zPath, char **pzErrMsg){
  LyroreContext *pCtx;
  LyrorePluginEntry *pEntry = 0;
  LyrorePluginInfo *(*xGetInfo)(void) = 0;
  LyrorePluginInfo *pInfo = 0;
  sqlite3_vfs *pVfs;
  void *pHandle = 0;
  int rc = SQLITE_OK;

  if( pzErrMsg ) *pzErrMsg = 0;

  /* Security check: plugins must be explicitly enabled */
  if( !(db->flags & SQLITE_LyrorePlugins) ){
    if( pzErrMsg ){
      *pzErrMsg = sqlite3_mprintf("Plugin loading disabled. "
                                   "Use PRAGMA lyrore_plugins=ON first.");
    }
    return SQLITE_ERROR;
  }

  pCtx = db->pLyrore;
  if( !pCtx ){
    if( pzErrMsg ) *pzErrMsg = sqlite3_mprintf("Lyrore not initialized");
    return SQLITE_ERROR;
  }

  /* Check if already loaded */
  for(pEntry = pCtx->pPlugins; pEntry; pEntry = pEntry->pNext){
    if( sqlite3_stricmp(pEntry->zPath, zPath) == 0 ){
      if( pzErrMsg ) *pzErrMsg = sqlite3_mprintf("Plugin already loaded: %s", zPath);
      return SQLITE_ERROR;
    }
  }

  /* Get the VFS for dlopen operations */
  pVfs = sqlite3_vfs_find(0);
  if( !pVfs || !pVfs->xDlOpen ){
    if( pzErrMsg ) *pzErrMsg = sqlite3_mprintf("Dynamic loading not supported");
    return SQLITE_ERROR;
  }

  /* Load the shared library */
  pHandle = pVfs->xDlOpen(pVfs, zPath);
  if( !pHandle ){
    char zErr[256];
    pVfs->xDlError(pVfs, sizeof(zErr), zErr);
    if( pzErrMsg ) *pzErrMsg = sqlite3_mprintf("Cannot load plugin: %s", zErr);
    return SQLITE_ERROR;
  }

  /* Look up the entry point */
  xGetInfo = (LyrorePluginInfo*(*)(void))pVfs->xDlSym(pVfs, pHandle, LYRORE_PLUGIN_ENTRY);
  if( !xGetInfo ){
    if( pzErrMsg ){
      *pzErrMsg = sqlite3_mprintf("Entry point '%s' not found", LYRORE_PLUGIN_ENTRY);
    }
    pVfs->xDlClose(pVfs, pHandle);
    return SQLITE_ERROR;
  }

  /* Get plugin info */
  pInfo = xGetInfo();
  if( !pInfo ){
    if( pzErrMsg ) *pzErrMsg = sqlite3_mprintf("Plugin returned NULL info");
    pVfs->xDlClose(pVfs, pHandle);
    return SQLITE_ERROR;
  }

  /* Version check */
  if( pInfo->version != LYRORE_PLUGIN_VERSION ){
    if( pzErrMsg ){
      *pzErrMsg = sqlite3_mprintf("Plugin version mismatch: expected %d, got %d",
                                   LYRORE_PLUGIN_VERSION, pInfo->version);
    }
    pVfs->xDlClose(pVfs, pHandle);
    return SQLITE_ERROR;
  }

  /* Create plugin entry */
  pEntry = sqlite3_malloc(sizeof(LyrorePluginEntry));
  if( !pEntry ){
    pVfs->xDlClose(pVfs, pHandle);
    return SQLITE_NOMEM;
  }
  memset(pEntry, 0, sizeof(LyrorePluginEntry));

  pEntry->zName = sqlite3_mprintf("%s", pInfo->name ? pInfo->name : "unnamed");
  pEntry->zPath = sqlite3_mprintf("%s", zPath);
  pEntry->pHandle = pHandle;
  pEntry->pInfo = pInfo;

  if( !pEntry->zName || !pEntry->zPath ){
    sqlite3_free(pEntry->zName);
    sqlite3_free(pEntry->zPath);
    sqlite3_free(pEntry);
    pVfs->xDlClose(pVfs, pHandle);
    return SQLITE_NOMEM;
  }

  /* Call plugin init if provided */
  if( pInfo->xInit ){
    rc = pInfo->xInit(db, &pEntry->pCtx);
    if( rc != SQLITE_OK ){
      if( pzErrMsg ) *pzErrMsg = sqlite3_mprintf("Plugin init failed");
      sqlite3_free(pEntry->zName);
      sqlite3_free(pEntry->zPath);
      sqlite3_free(pEntry);
      pVfs->xDlClose(pVfs, pHandle);
      return rc;
    }
  }

  /* Register all hooks from the plugin */
  rc = registerPluginHooks(db, pInfo);
  if( rc != SQLITE_OK ){
    if( pInfo->xShutdown ) pInfo->xShutdown(pEntry->pCtx);
    sqlite3_free(pEntry->zName);
    sqlite3_free(pEntry->zPath);
    sqlite3_free(pEntry);
    pVfs->xDlClose(pVfs, pHandle);
    return rc;
  }

  /* Add to linked list */
  pEntry->pNext = pCtx->pPlugins;
  pCtx->pPlugins = pEntry;
  pCtx->nPlugins++;

  return SQLITE_OK;
}

/*
** Unload a plugin by name.
*/
int lyroreUnloadPlugin(sqlite3 *db, const char *zName){
  LyroreContext *pCtx;
  LyrorePluginEntry *pEntry, *pPrev = 0;
  sqlite3_vfs *pVfs;

  pCtx = db->pLyrore;
  if( !pCtx ) return SQLITE_ERROR;

  for(pEntry = pCtx->pPlugins; pEntry; pPrev = pEntry, pEntry = pEntry->pNext){
    if( sqlite3_stricmp(pEntry->zName, zName) == 0 ){
      /* Call shutdown callback */
      if( pEntry->pInfo && pEntry->pInfo->xShutdown ){
        pEntry->pInfo->xShutdown(pEntry->pCtx);
      }

      /* Remove from list */
      if( pPrev ){
        pPrev->pNext = pEntry->pNext;
      } else {
        pCtx->pPlugins = pEntry->pNext;
      }
      pCtx->nPlugins--;

      /* Close the shared library */
      pVfs = sqlite3_vfs_find(0);
      if( pVfs && pVfs->xDlClose && pEntry->pHandle ){
        pVfs->xDlClose(pVfs, pEntry->pHandle);
      }

      /* Free memory */
      sqlite3_free(pEntry->zName);
      sqlite3_free(pEntry->zPath);
      sqlite3_free(pEntry);

      return SQLITE_OK;
    }
  }

  return SQLITE_NOTFOUND;
}

/*
** Unload all plugins for a connection.
*/
void lyroreUnloadAllPlugins(sqlite3 *db){
  LyroreContext *pCtx;
  LyrorePluginEntry *pEntry, *pNext;
  sqlite3_vfs *pVfs;

  pCtx = db->pLyrore;
  if( !pCtx ) return;

  pVfs = sqlite3_vfs_find(0);

  for(pEntry = pCtx->pPlugins; pEntry; pEntry = pNext){
    pNext = pEntry->pNext;

    /* Call shutdown callback */
    if( pEntry->pInfo && pEntry->pInfo->xShutdown ){
      pEntry->pInfo->xShutdown(pEntry->pCtx);
    }

    /* Close the shared library */
    if( pVfs && pVfs->xDlClose && pEntry->pHandle ){
      pVfs->xDlClose(pVfs, pEntry->pHandle);
    }

    /* Free memory */
    sqlite3_free(pEntry->zName);
    sqlite3_free(pEntry->zPath);
    sqlite3_free(pEntry);
  }

  pCtx->pPlugins = 0;
  pCtx->nPlugins = 0;
}

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
  const char *zPath;
  char *zErrMsg = 0;
  int rc;

  if( argc != 1 ){
    sqlite3_result_error(context, "lyrore_register requires exactly one argument", -1);
    return;
  }

  zPath = (const char*)sqlite3_value_text(argv[0]);
  if( !zPath || zPath[0] == 0 ){
    sqlite3_result_error(context, "Plugin path cannot be empty", -1);
    return;
  }

  rc = lyroreLoadPlugin(db, zPath, &zErrMsg);
  if( rc != SQLITE_OK ){
    if( zErrMsg ){
      sqlite3_result_error(context, zErrMsg, -1);
      sqlite3_free(zErrMsg);
    } else {
      sqlite3_result_error(context, "Failed to load plugin", -1);
    }
    return;
  }

  sqlite3_result_text(context, "Plugin loaded successfully", -1, SQLITE_STATIC);
}

/*
** Register the lyrore_register() SQL function.
*/
void lyroreRegisterPluginFunction(sqlite3 *db){
  sqlite3_create_function(db, "lyrore_register", 1, 
                          SQLITE_UTF8 | SQLITE_DIRECTONLY, 
                          0, lyroreRegisterFunc, 0, 0);
}

#endif /* SQLITE_ENABLE_LYRORE */
