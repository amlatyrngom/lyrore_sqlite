/*
** Lyrore Feature Extraction Header
** Part of the Lyrore ML-Ready Optimization Framework for SQLite
*/
#ifndef SQLITE_LYRORE_FEATURES_H
#define SQLITE_LYRORE_FEATURES_H

#ifdef SQLITE_ENABLE_LYRORE

/* Forward declarations - these are defined in whereInt.h and sqliteInt.h */
struct WhereInfo;
struct WhereLoop;
struct Table;
struct Parse;
struct Expr;

/* Feature extraction from query plans */
void lyroreExtractPlanFeatures(struct WhereInfo *pWInfo, LyroreFeatures *pFeat);
void lyroreExtractLoopFeatures(struct WhereLoop *pLoop, struct Table *pTab, LyroreFeatures *pFeat);
void lyroreExtractExprFeatures(struct Parse *pParse, struct Expr *pExpr, LyroreFeatures *pFeat);

/* LogEst conversion utilities */
double lyroreLogEstToDouble(LogEst x);

#endif /* SQLITE_ENABLE_LYRORE */
#endif /* SQLITE_LYRORE_FEATURES_H */
