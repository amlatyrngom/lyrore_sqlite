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
** This file contains the API for Lyrore plugin management.
** Plugins are dynamically loaded .so files that register hooks.
*/
#ifndef SQLITE_LYRORE_PLUGIN_H
#define SQLITE_LYRORE_PLUGIN_H

#ifdef SQLITE_ENABLE_LYRORE

/*
** Load a plugin from the given path.
** Returns SQLITE_OK on success, error code otherwise.
** Requires SQLITE_LyrorePlugins flag to be set.
*/
int lyroreLoadPlugin(sqlite3 *db, const char *zPath, char **pzErrMsg);

/*
** Unload a plugin by name.
** Returns SQLITE_OK on success, SQLITE_NOTFOUND if not loaded.
*/
int lyroreUnloadPlugin(sqlite3 *db, const char *zName);

/*
** Unload all plugins for a connection.
*/
void lyroreUnloadAllPlugins(sqlite3 *db);

/*
** SQL function: lyrore_register(path)
** Registers the SQL function for plugin loading.
*/
void lyroreRegisterPluginFunction(sqlite3 *db);

#endif /* SQLITE_ENABLE_LYRORE */
#endif /* SQLITE_LYRORE_PLUGIN_H */
