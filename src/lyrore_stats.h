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
** This file contains declarations for Lyrore state management,
** including initialization, persistence, and execution history tracking.
*/
#ifndef SQLITE_LYRORE_STATS_H
#define SQLITE_LYRORE_STATS_H

#ifdef SQLITE_ENABLE_LYRORE

/*
** Lifecycle functions
*/
int lyroreInit(sqlite3 *db);
void lyroreShutdown(sqlite3 *db);

/*
** Persistence functions
*/
int lyroreMaybePersist(sqlite3 *db);
int lyrorePersistNow(sqlite3 *db);
int lyroreReset(sqlite3 *db);

/*
** Execution history
*/
int lyroreRecordExecution(sqlite3 *db, const char *zPatternSig, i64 execTimeUs);
int lyroreGetExecHistory(sqlite3 *db, const char *zPatternSig,
                         int *pExecCount, i64 *pTotalTimeUs, i64 *pLastTimeUs);

/*
** Column statistics (for future steps)
*/
int lyroreUpdateColumnStats(sqlite3 *db, const char *zTable, const char *zColumn,
                            int distinctCount, int totalCount);

#endif /* SQLITE_ENABLE_LYRORE */
#endif /* SQLITE_LYRORE_STATS_H */
