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
** This file contains function declarations for the Lyrore Pattern
** Matching Framework. Struct definitions are in lyrore_model.h.
*/
#ifndef SQLITE_LYRORE_PATTERN_H
#define SQLITE_LYRORE_PATTERN_H

#ifdef SQLITE_ENABLE_LYRORE

/*
** Pattern Registration API
*/
int lyroreRegisterPattern(sqlite3 *db, LyrorePatternDef *pDef);
int lyroreUnregisterPattern(sqlite3 *db, LyrorePatternId id);
LyrorePatternDef *lyroreGetPattern(sqlite3 *db, LyrorePatternId id);

/*
** Pattern Dispatch API
*/
LyrorePatternDef *lyroreDispatchCostPattern(sqlite3*, struct WhereLoop*, struct Table*);
LyrorePatternDef *lyroreDispatchPlanPattern(sqlite3*, struct WhereInfo*);
LyrorePatternDef *lyroreDispatchExprPattern(sqlite3*, struct Parse*, struct Expr*);

/*
** High-Level Pattern-Aware Functions
*/
int lyrorePatternAdjustEstimates(sqlite3*, struct WhereLoop*, struct Table*);
int lyrorePatternSelectPlan(sqlite3*, struct WhereInfo*, int nOptions);
int lyrorePatternSelectFlavor(sqlite3*, struct Parse*, struct Expr*, int nFlavors);

/*
** State Persistence
*/
int lyroreSavePatternState(sqlite3 *db, LyrorePatternDef *pPat);
int lyroreLoadPatternState(sqlite3 *db, LyrorePatternDef *pPat);
int lyrorePersistAllPatterns(sqlite3 *db);

/*
** Registry lifecycle (called from lyroreInit/lyroreShutdown)
*/
void lyroreInitPatternRegistry(LyrorePatternRegistry *pReg);
void lyroreDestroyPatternRegistry(LyrorePatternRegistry *pReg);

#endif /* SQLITE_ENABLE_LYRORE */
#endif /* SQLITE_LYRORE_PATTERN_H */
