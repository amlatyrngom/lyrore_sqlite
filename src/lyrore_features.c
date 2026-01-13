/*
** Lyrore Feature Extraction Implementation
** Part of the Lyrore ML-Ready Optimization Framework for SQLite
**
** This file implements feature extraction from query plans for use
** by ML models in adaptive query optimization.
*/
#include "sqliteInt.h"

#ifdef SQLITE_ENABLE_LYRORE

#include "whereInt.h"
#include "lyrore_model.h"
#include "lyrore_features.h"
#include <math.h>

/*
** Convert LogEst (log2(x)*10) to double.
** LogEst values are used throughout SQLite for cost/row estimates.
** This converts them to actual numeric values.
*/
double lyroreLogEstToDouble(LogEst x){
  if( x<=0 ) return 1.0;
  /* LogEst is log2(N)*10, so N = 2^(x/10) */
  return pow(2.0, (double)x / 10.0);
}

/*
** Extract features from a completed WhereInfo plan.
** This populates query structure and aggregate access pattern features.
** Called after query planning is complete.
**
** pWInfo: The completed WHERE clause analysis
** pFeat: Output structure to populate with features
*/
void lyroreExtractPlanFeatures(WhereInfo *pWInfo, LyroreFeatures *pFeat){
  int i;

  if( pWInfo==0 || pFeat==0 ) return;

  /* Query structure from pTabList */
  if( pWInfo->pTabList ){
    pFeat->nTables = pWInfo->pTabList->nSrc;
    pFeat->nJoins = (pFeat->nTables > 1) ? pFeat->nTables - 1 : 0;
  }

  /* Predicates from WhereClause */
  pFeat->nPredicates = pWInfo->sWC.nTerm;

  /* Output rows estimate */
  pFeat->estOutputRows = lyroreLogEstToDouble(pWInfo->nRowOut);

  /* Aggregate access patterns across all levels */
  pFeat->estCost = 0.0;
  for(i=0; i<pWInfo->nLevel; i++){
    WhereLoop *pLoop = pWInfo->a[i].pWLoop;
    if( pLoop ){
      /* Count index vs table scans */
      if( pLoop->wsFlags & WHERE_INDEXED ){
        pFeat->nIndexScans++;
      }
      if( pLoop->wsFlags & WHERE_IPK ){
        pFeat->nTableScans++;
      }

      /* Check for equality constraints */
      if( pLoop->u.btree.nEq > 0 ){
        pFeat->hasEquality = 1;
      }

      /* Check for range constraints */
      if( pLoop->wsFlags & (WHERE_BTM_LIMIT|WHERE_TOP_LIMIT) ){
        pFeat->hasRange = 1;
      }

      /* Accumulate cost */
      pFeat->estCost += lyroreLogEstToDouble(pLoop->rRun);
    }
  }
}

/*
** Extract features from a single WhereLoop.
** Used during loop costing before the final plan is selected.
**
** pLoop: The WhereLoop to extract features from
** pTab: The table associated with this loop (may be NULL)
** pFeat: Output structure to populate with features
*/
void lyroreExtractLoopFeatures(WhereLoop *pLoop, Table *pTab, LyroreFeatures *pFeat){
  if( pLoop==0 || pFeat==0 ) return;

  /* Cost estimates from the loop */
  pFeat->estCost = lyroreLogEstToDouble(pLoop->rRun);
  pFeat->estOutputRows = lyroreLogEstToDouble(pLoop->nOut);

  /* Table row estimate if available */
  if( pTab ){
    pFeat->estTotalRows = lyroreLogEstToDouble(pTab->nRowLogEst);
    if( pFeat->estTotalRows > 0 && pFeat->estOutputRows > 0 ){
      pFeat->avgSelectivity = pFeat->estOutputRows / pFeat->estTotalRows;
    }
  }

  /* Access pattern flags */
  if( pLoop->wsFlags & WHERE_INDEXED ){
    pFeat->nIndexScans = 1;
  }else{
    pFeat->nIndexScans = 0;
  }

  if( pLoop->wsFlags & WHERE_IPK ){
    pFeat->nTableScans = 1;
  }else{
    pFeat->nTableScans = 0;
  }

  /* Equality constraints */
  pFeat->hasEquality = (pLoop->u.btree.nEq > 0) ? 1 : 0;

  /* Range constraints */
  pFeat->hasRange = (pLoop->wsFlags & (WHERE_BTM_LIMIT|WHERE_TOP_LIMIT)) ? 1 : 0;
}

/*
** Extract features relevant to expression optimization.
** Used for flavor selection during code generation.
**
** pParse: The parser context
** pExpr: The expression to extract features from
** pFeat: Output structure to populate with features
*/
void lyroreExtractExprFeatures(Parse *pParse, Expr *pExpr, LyroreFeatures *pFeat){
  if( pParse==0 || pExpr==0 || pFeat==0 ) return;

  /* Query depth from parser */
  pFeat->queryDepth = pParse->nHeight;

  /* Check for LIKE in expression - scan the tree */
  /* For now, just check the root expression operator */
  /* TK_LIKE_KW is used for LIKE expressions */
  /* Note: A more complete implementation would recursively scan */
}

#endif /* SQLITE_ENABLE_LYRORE */
