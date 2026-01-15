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
** This file contains the API for Lyrore hook management.
** Hooks are the extension points for plugins to customize SQLite behavior.
*/
#ifndef SQLITE_LYRORE_HOOKS_H
#define SQLITE_LYRORE_HOOKS_H

#ifdef SQLITE_ENABLE_LYRORE

/*
** Hook Registration API
** These functions add hooks to the per-connection registry.
** Returns SQLITE_OK on success, error code otherwise.
*/
int lyroreRegisterPreOptHook(sqlite3 *db, const LyrorePreOptHook *pHook);
int lyroreRegisterEstimateHook(sqlite3 *db, const LyroreEstimateHook *pHook);
int lyroreRegisterPostQueryHook(sqlite3 *db, const LyrorePostQueryHook *pHook);
int lyroreRegisterAnalyzeHook(sqlite3 *db, const LyroreAnalyzeHook *pHook);

/*
** Register built-in hooks (e.g., UDF transformation).
** Called during Lyrore context initialization.
*/
int lyroreRegisterBuiltinHooks(sqlite3 *db);

/*
** UDF Transformation API
** Transform known UDF patterns to native SQL expressions.
** Returns the number of transformations performed.
*/
int lyroreTransformUdfCalls(sqlite3 *db, Parse *pParse, Select *pSelect);

/*
** Hook Invocation API
** These functions are called at specific points in query processing.
** They iterate through registered hooks in priority order (descending).
*/

/* 
** Invoke pre-optimization hooks.
** Called after name resolution, before built-in optimizations.
** Location: select.c, after sqlite3SelectPrep()
*/
int lyroreInvokePreOptHooks(sqlite3 *db, Parse *pParse, Select *p);

/*
** Invoke estimate hooks.
** Called for each WhereLoop candidate during plan generation.
** Location: where.c, inside whereLoopInsert() before whereLoopAdjustCost()
*/
void lyroreInvokeEstimateHooks(sqlite3 *db, WhereLoopBuilder *pBuilder, 
                               WhereLoop *pLoop);

/*
** Invoke post-query hooks.
** Called when query execution completes.
** Location: vdbeaux.c, inside sqlite3VdbeHalt()
*/
void lyroreInvokePostQueryHooks(sqlite3 *db, Vdbe *p);

/*
** Invoke analyze hooks.
** Called after loading analysis data.
** Location: vdbe.c, after OP_LoadAnalysis
*/
void lyroreInvokeAnalyzeHooks(sqlite3 *db, int iDb);

/*
** Initialize/cleanup hook arrays in LyroreContext.
*/
void lyroreInitHooks(LyroreContext *pCtx);
void lyroreFreeHooks(LyroreContext *pCtx);

#endif /* SQLITE_ENABLE_LYRORE */
#endif /* SQLITE_LYRORE_HOOKS_H */
