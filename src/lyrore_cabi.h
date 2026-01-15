/*
** Lyrore C ABI Interface
** 
** Minimal C glue (~100 LOC) for integrating C++ plugin SDK with SQLite.
** This is the only Lyrore C code that needs to live inside SQLite.
*/
#ifndef SQLITE_LYRORE_CABI_H
#define SQLITE_LYRORE_CABI_H

#ifdef SQLITE_ENABLE_LYRORE

/* Opaque pointer to C++ context */
typedef struct LyroreCppContext LyroreCppContext;

/* Context lifecycle */
LyroreCppContext* lyrore_cpp_create(sqlite3* db);
void lyrore_cpp_destroy(LyroreCppContext* ctx);

/* Plugin loading */
int lyrore_cpp_load_plugin(LyroreCppContext* ctx, const char* path);

/* Hook invocation */
int lyrore_cpp_invoke_preopt(LyroreCppContext* ctx, void* pParse, void* pSelect);
void lyrore_cpp_invoke_estimate(LyroreCppContext* ctx, void* pBuilder, void* pLoop);
void lyrore_cpp_invoke_analyze(LyroreCppContext* ctx, int iDb);
void lyrore_cpp_invoke_postquery(LyroreCppContext* ctx, void* pVdbe);

/* 
** SQL function to register plugins:
**   SELECT lyrore_register('/path/to/plugin.so');
*/
void lyrore_register_functions(sqlite3* db);

/* Context lifecycle - called from main.c */
int lyroreInit(sqlite3 *db);
void lyroreShutdown(sqlite3 *db);

/* State management - called from pragma.c */
void lyrorePersistNow(sqlite3 *db);
void lyroreReset(sqlite3 *db);

/* Hook wrappers - called from hook integration points 
** Note: Use void* for internal types that may not be visible in all contexts
*/
int lyroreInvokePreOptHooks(sqlite3 *db, void *pParse, void *pSelect);
void lyroreInvokeEstimateHooks(sqlite3 *db, void *pBuilder, void *pLoop);
void lyroreInvokeAnalyzeHooks(sqlite3 *db, int iDb);
void lyroreInvokePostQueryHooks(sqlite3 *db, void *pVdbe);

#endif /* SQLITE_ENABLE_LYRORE */
#endif /* SQLITE_LYRORE_CABI_H */
