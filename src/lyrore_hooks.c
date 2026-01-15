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
** This file contains the implementation of Lyrore hook management.
*/
#include "sqliteInt.h"
#ifdef SQLITE_ENABLE_LYRORE

#include "lyrore_model.h"
#include "lyrore_hooks.h"

/*
** Initialize hook arrays in LyroreContext.
*/
void lyroreInitHooks(LyroreContext *pCtx){
  memset(&pCtx->preOptHooks, 0, sizeof(pCtx->preOptHooks));
  memset(&pCtx->estimateHooks, 0, sizeof(pCtx->estimateHooks));
  memset(&pCtx->postQueryHooks, 0, sizeof(pCtx->postQueryHooks));
  memset(&pCtx->analyzeHooks, 0, sizeof(pCtx->analyzeHooks));
  pCtx->pPlugins = 0;
  pCtx->nPlugins = 0;
}

/*
** Free hook arrays in LyroreContext.
*/
void lyroreFreeHooks(LyroreContext *pCtx){
  sqlite3_free(pCtx->preOptHooks.a);
  sqlite3_free(pCtx->estimateHooks.a);
  sqlite3_free(pCtx->postQueryHooks.a);
  sqlite3_free(pCtx->analyzeHooks.a);
  memset(&pCtx->preOptHooks, 0, sizeof(pCtx->preOptHooks));
  memset(&pCtx->estimateHooks, 0, sizeof(pCtx->estimateHooks));
  memset(&pCtx->postQueryHooks, 0, sizeof(pCtx->postQueryHooks));
  memset(&pCtx->analyzeHooks, 0, sizeof(pCtx->analyzeHooks));
}

/*
** Comparison functions for sorting hooks by priority (descending).
*/
static int preOptHookCmp(const void *a, const void *b){
  const LyrorePreOptHook *ha = (const LyrorePreOptHook*)a;
  const LyrorePreOptHook *hb = (const LyrorePreOptHook*)b;
  return hb->priority - ha->priority;  /* Descending order */
}

static int estimateHookCmp(const void *a, const void *b){
  const LyroreEstimateHook *ha = (const LyroreEstimateHook*)a;
  const LyroreEstimateHook *hb = (const LyroreEstimateHook*)b;
  return hb->priority - ha->priority;  /* Descending order */
}

/*
** Register a pre-optimization hook.
*/
int lyroreRegisterPreOptHook(sqlite3 *db, const LyrorePreOptHook *pHook){
  LyroreContext *pCtx = db->pLyrore;
  LyrorePreOptHook *aNew;
  int n, nAlloc;

  if( !pCtx ) return SQLITE_ERROR;
  if( !pHook || !pHook->xRewrite ) return SQLITE_ERROR;

  n = pCtx->preOptHooks.n;
  nAlloc = pCtx->preOptHooks.nAlloc;

  if( n >= nAlloc ){
    int newAlloc = nAlloc ? nAlloc * 2 : 4;
    aNew = sqlite3_realloc(pCtx->preOptHooks.a, newAlloc * sizeof(LyrorePreOptHook));
    if( !aNew ) return SQLITE_NOMEM;
    pCtx->preOptHooks.a = aNew;
    pCtx->preOptHooks.nAlloc = newAlloc;
  }

  /* Copy hook data */
  pCtx->preOptHooks.a[n] = *pHook;
  pCtx->preOptHooks.n++;

  /* Re-sort by priority (descending) */
  qsort(pCtx->preOptHooks.a, pCtx->preOptHooks.n, sizeof(LyrorePreOptHook), preOptHookCmp);

  return SQLITE_OK;
}

/*
** Register an estimate hook.
*/
int lyroreRegisterEstimateHook(sqlite3 *db, const LyroreEstimateHook *pHook){
  LyroreContext *pCtx = db->pLyrore;
  LyroreEstimateHook *aNew;
  int n, nAlloc;

  if( !pCtx ) return SQLITE_ERROR;
  if( !pHook || (!pHook->xMatch && !pHook->xAdjust) ) return SQLITE_ERROR;

  n = pCtx->estimateHooks.n;
  nAlloc = pCtx->estimateHooks.nAlloc;

  if( n >= nAlloc ){
    int newAlloc = nAlloc ? nAlloc * 2 : 4;
    aNew = sqlite3_realloc(pCtx->estimateHooks.a, newAlloc * sizeof(LyroreEstimateHook));
    if( !aNew ) return SQLITE_NOMEM;
    pCtx->estimateHooks.a = aNew;
    pCtx->estimateHooks.nAlloc = newAlloc;
  }

  pCtx->estimateHooks.a[n] = *pHook;
  pCtx->estimateHooks.n++;

  qsort(pCtx->estimateHooks.a, pCtx->estimateHooks.n, sizeof(LyroreEstimateHook), estimateHookCmp);

  return SQLITE_OK;
}

/*
** Register a post-query hook.
*/
int lyroreRegisterPostQueryHook(sqlite3 *db, const LyrorePostQueryHook *pHook){
  LyroreContext *pCtx = db->pLyrore;
  LyrorePostQueryHook *aNew;
  int n, nAlloc;

  if( !pCtx ) return SQLITE_ERROR;
  if( !pHook || !pHook->xCollect ) return SQLITE_ERROR;

  n = pCtx->postQueryHooks.n;
  nAlloc = pCtx->postQueryHooks.nAlloc;

  if( n >= nAlloc ){
    int newAlloc = nAlloc ? nAlloc * 2 : 4;
    aNew = sqlite3_realloc(pCtx->postQueryHooks.a, newAlloc * sizeof(LyrorePostQueryHook));
    if( !aNew ) return SQLITE_NOMEM;
    pCtx->postQueryHooks.a = aNew;
    pCtx->postQueryHooks.nAlloc = newAlloc;
  }

  pCtx->postQueryHooks.a[n] = *pHook;
  pCtx->postQueryHooks.n++;

  /* Post-query hooks don't have priority, run in registration order */
  return SQLITE_OK;
}

/*
** Register an analyze hook.
*/
int lyroreRegisterAnalyzeHook(sqlite3 *db, const LyroreAnalyzeHook *pHook){
  LyroreContext *pCtx = db->pLyrore;
  LyroreAnalyzeHook *aNew;
  int n, nAlloc;

  if( !pCtx ) return SQLITE_ERROR;
  if( !pHook || !pHook->xAnalyze ) return SQLITE_ERROR;

  n = pCtx->analyzeHooks.n;
  nAlloc = pCtx->analyzeHooks.nAlloc;

  if( n >= nAlloc ){
    int newAlloc = nAlloc ? nAlloc * 2 : 4;
    aNew = sqlite3_realloc(pCtx->analyzeHooks.a, newAlloc * sizeof(LyroreAnalyzeHook));
    if( !aNew ) return SQLITE_NOMEM;
    pCtx->analyzeHooks.a = aNew;
    pCtx->analyzeHooks.nAlloc = newAlloc;
  }

  pCtx->analyzeHooks.a[n] = *pHook;
  pCtx->analyzeHooks.n++;

  return SQLITE_OK;
}

/*****************************************************************************
** AST Transformation Helper for UDF->Native expression rewriting
**
** This function walks an expression tree and transforms:
**   product_filter(a, b, c) = 1  ->  (a * b) < c
**
** The product_filter UDF is a placeholder for expensive user-defined functions
** that can be replaced with equivalent native SQL expressions.
*****************************************************************************/

/*
** Context for the UDF transformation walker.
*/
typedef struct UdfTransformCtx {
  Parse *pParse;
  sqlite3 *db;
  int nTransformed;
} UdfTransformCtx;

/*
** Check if an expression is a call to product_filter with 3 arguments.
*/
static int isProductFilterCall(Expr *pExpr){
  if( pExpr->op!=TK_FUNCTION ) return 0;
  if( !pExpr->u.zToken ) return 0;
  if( sqlite3_stricmp(pExpr->u.zToken, "product_filter")!=0 ) return 0;
  if( !ExprUseXList(pExpr) ) return 0;
  if( !pExpr->x.pList || pExpr->x.pList->nExpr!=3 ) return 0;
  return 1;
}

/*
** Transform a product_filter(a, b, c) expression into (a * b) < c
** Returns the new expression, or NULL on error.
*/
static Expr *transformProductFilter(Parse *pParse, Expr *pFunc){
  sqlite3 *db = pParse->db;
  ExprList *pArgs = pFunc->x.pList;
  Expr *pA, *pB, *pC;
  Expr *pMul, *pLt;
  
  /* Get the three arguments - duplicate them since originals will be freed */
  pA = sqlite3ExprDup(db, pArgs->a[0].pExpr, 0);
  pB = sqlite3ExprDup(db, pArgs->a[1].pExpr, 0);
  pC = sqlite3ExprDup(db, pArgs->a[2].pExpr, 0);
  
  if( !pA || !pB || !pC ){
    sqlite3ExprDelete(db, pA);
    sqlite3ExprDelete(db, pB);
    sqlite3ExprDelete(db, pC);
    return 0;
  }
  
  /* Build (a * b) using TK_STAR */
  pMul = sqlite3PExpr(pParse, TK_STAR, pA, pB);
  if( !pMul ){
    sqlite3ExprDelete(db, pC);
    return 0;
  }
  
  /* Build (a * b) < c using TK_LT */
  pLt = sqlite3PExpr(pParse, TK_LT, pMul, pC);
  return pLt;
}

/*
** Walker callback to find and transform product_filter expressions.
** Looks for pattern: product_filter(a,b,c) = 1
*/
static int udfTransformCallback(Walker *pWalker, Expr *pExpr){
  UdfTransformCtx *pCtx = (UdfTransformCtx*)pWalker->u.pNC;
  Parse *pParse = pWalker->pParse;
  
  /* Pattern: product_filter(a,b,c) = 1 */
  if( pExpr->op==TK_EQ ){
    Expr *pLeft = pExpr->pLeft;
    Expr *pRight = pExpr->pRight;
    
    /* Check for func = 1 */
    if( pLeft && isProductFilterCall(pLeft) ){
      if( pRight && pRight->op==TK_INTEGER ){
        int val = 0;
        if( pRight->flags & EP_IntValue ){
          val = pRight->u.iValue;
        }else if( pRight->u.zToken ){
          val = sqlite3Atoi(pRight->u.zToken);
        }
        if( val==1 ){
          /* Transform! Replace this EQ node with (a*b)<c */
          Expr *pNew = transformProductFilter(pParse, pLeft);
          if( pNew ){
            /* Free old children */
            sqlite3ExprDelete(pCtx->db, pExpr->pLeft);
            sqlite3ExprDelete(pCtx->db, pExpr->pRight);
            /* Copy new expression structure into this node */
            pExpr->op = pNew->op;
            pExpr->pLeft = pNew->pLeft;
            pExpr->pRight = pNew->pRight;
            pExpr->flags = (pExpr->flags & ~EP_Leaf) | (pNew->flags & EP_Propagate);
            /* Free the wrapper (but not its children) */
            pNew->pLeft = 0;
            pNew->pRight = 0;
            sqlite3ExprDelete(pCtx->db, pNew);
            pCtx->nTransformed++;
            return WRC_Prune; /* Don't descend into transformed node */
          }
        }
      }
    }
    /* Check for 1 = func (commuted) */
    if( pRight && isProductFilterCall(pRight) ){
      if( pLeft && pLeft->op==TK_INTEGER ){
        int val = 0;
        if( pLeft->flags & EP_IntValue ){
          val = pLeft->u.iValue;
        }else if( pLeft->u.zToken ){
          val = sqlite3Atoi(pLeft->u.zToken);
        }
        if( val==1 ){
          Expr *pNew = transformProductFilter(pParse, pRight);
          if( pNew ){
            sqlite3ExprDelete(pCtx->db, pExpr->pLeft);
            sqlite3ExprDelete(pCtx->db, pExpr->pRight);
            pExpr->op = pNew->op;
            pExpr->pLeft = pNew->pLeft;
            pExpr->pRight = pNew->pRight;
            pExpr->flags = (pExpr->flags & ~EP_Leaf) | (pNew->flags & EP_Propagate);
            pNew->pLeft = 0;
            pNew->pRight = 0;
            sqlite3ExprDelete(pCtx->db, pNew);
            pCtx->nTransformed++;
            return WRC_Prune;
          }
        }
      }
    }
  }
  
  return WRC_Continue;
}

/*
** Public API: Transform UDF calls to native expressions in a SELECT's WHERE clause.
** Returns number of transformations made.
*/
int lyroreTransformUdfCalls(sqlite3 *db, Parse *pParse, Select *pSelect){
  Walker w;
  UdfTransformCtx ctx;
  
  if( !pSelect || !pSelect->pWhere ) return 0;
  
  memset(&w, 0, sizeof(w));
  memset(&ctx, 0, sizeof(ctx));
  
  ctx.pParse = pParse;
  ctx.db = db;
  ctx.nTransformed = 0;
  
  w.pParse = pParse;
  w.xExprCallback = udfTransformCallback;
  w.xSelectCallback = sqlite3SelectWalkNoop;
  w.u.pNC = (NameContext*)&ctx;  /* Use union to pass our context */
  
  sqlite3WalkExpr(&w, pSelect->pWhere);
  
  return ctx.nTransformed;
}

/*****************************************************************************
** Built-in pre-opt hook that performs UDF transformation
*****************************************************************************/
static int builtinUdfTransformHook(sqlite3 *db, Parse *pParse, Select *p, void *pCtx){
  int n = lyroreTransformUdfCalls(db, pParse, p);
  (void)pCtx;  /* Unused */
  return n > 0 ? LYRORE_REWRITE_MODIFIED : LYRORE_REWRITE_NONE;
}

/*
** Register the built-in UDF transformation hook.
** Called during Lyrore initialization.
*/
int lyroreRegisterBuiltinHooks(sqlite3 *db){
  static const LyrorePreOptHook udfHook = {
    "builtin_udf_transform",  /* zName */
    100,                      /* priority */
    builtinUdfTransformHook,  /* xRewrite */
    0                         /* pCtx */
  };
  return lyroreRegisterPreOptHook(db, &udfHook);
}

/*****************************************************************************
** Hook Invocation Functions
*****************************************************************************/

/*
** Invoke pre-optimization hooks.
** Returns SQLITE_OK if all hooks succeed, error code otherwise.
*/
int lyroreInvokePreOptHooks(sqlite3 *db, Parse *pParse, Select *p){
  LyroreContext *pCtx;
  int i, rc;

  if( !db || !(db->flags & SQLITE_LyroreEnabled) ) return SQLITE_OK;

  pCtx = db->pLyrore;
  if( !pCtx || pCtx->preOptHooks.n == 0 ) return SQLITE_OK;

  for(i = 0; i < pCtx->preOptHooks.n; i++){
    LyrorePreOptHook *pHook = &pCtx->preOptHooks.a[i];
    if( pHook->xRewrite ){
      rc = pHook->xRewrite(db, pParse, p, pHook->pCtx);
      if( rc == LYRORE_REWRITE_ERROR ){
        return SQLITE_ERROR;
      }
    }
  }

  return SQLITE_OK;
}

/*
** Invoke estimate hooks for a WhereLoop candidate.
*/
void lyroreInvokeEstimateHooks(sqlite3 *db, WhereLoopBuilder *pBuilder, 
                               WhereLoop *pLoop){
  LyroreContext *pCtx;
  int i;

  if( !db || !(db->flags & SQLITE_LyroreCost) ) return;

  pCtx = db->pLyrore;
  if( !pCtx || pCtx->estimateHooks.n == 0 ) return;

  for(i = 0; i < pCtx->estimateHooks.n; i++){
    LyroreEstimateHook *pHook = &pCtx->estimateHooks.a[i];
    int shouldAdjust = 1;

    /* If xMatch is provided, only call xAdjust if it returns true */
    if( pHook->xMatch ){
      shouldAdjust = pHook->xMatch(db, pBuilder, pLoop, pHook->pCtx);
    }

    if( shouldAdjust && pHook->xAdjust ){
      pHook->xAdjust(db, pBuilder, pLoop, pHook->pCtx);
    }
  }
}

/*
** Invoke post-query hooks.
** Collects query statistics and passes them to all registered hooks.
*/
void lyroreInvokePostQueryHooks(sqlite3 *db, Vdbe *p){
  LyroreContext *pCtx;
  LyroreQueryStats stats;
  int i;

  if( !db || !(db->flags & SQLITE_LyroreEnabled) ) return;

  pCtx = db->pLyrore;
  if( !pCtx || pCtx->postQueryHooks.n == 0 ) return;

  /* Initialize stats structure */
  memset(&stats, 0, sizeof(stats));
  stats.zSql = sqlite3_sql((sqlite3_stmt*)p);
  stats.nVmStep = p->nStmtDefCons; /* Approximate */

  /* Query hash - simple hash of SQL text */
  if( stats.zSql ){
    const char *z = stats.zSql;
    u64 h = 0;
    while( *z ){
      h = h * 31 + (unsigned char)*z++;
    }
    stats.queryHash = h;
  }

  stats.execTimeUs = 0;

  /* Per-loop statistics from scan status if available */
#ifdef SQLITE_ENABLE_STMT_SCANSTATUS
  {
    int idx = 0;
    int nLoops = 0;
    i64 nRow;

    /* Count loops */
    while( sqlite3_stmt_scanstatus((sqlite3_stmt*)p, idx, 
           SQLITE_SCANSTAT_NLOOP, &nRow) == SQLITE_OK ){
      nLoops++;
      idx++;
    }

    if( nLoops > 0 ){
      stats.nLoops = nLoops;
      stats.aLoops = sqlite3_malloc(nLoops * sizeof(LyroreLoopStats));
      if( stats.aLoops ){
        for(idx = 0; idx < nLoops; idx++){
          i64 est = 0, nVisit = 0;
          const char *zExplain = 0;

          sqlite3_stmt_scanstatus((sqlite3_stmt*)p, idx, 
                                  SQLITE_SCANSTAT_EST, &est);
          sqlite3_stmt_scanstatus((sqlite3_stmt*)p, idx, 
                                  SQLITE_SCANSTAT_NVISIT, &nVisit);
          sqlite3_stmt_scanstatus((sqlite3_stmt*)p, idx, 
                                  SQLITE_SCANSTAT_EXPLAIN, &zExplain);

          stats.aLoops[idx].zExplain = zExplain;
          stats.aLoops[idx].estRows = est;
          stats.aLoops[idx].actualRows = nVisit;

          /* Q-error = max(est/actual, actual/est) */
          if( est > 0 && nVisit > 0 ){
            double ratio1 = (double)est / (double)nVisit;
            double ratio2 = (double)nVisit / (double)est;
            stats.aLoops[idx].qError = ratio1 > ratio2 ? ratio1 : ratio2;
          } else {
            stats.aLoops[idx].qError = 0.0;
          }
        }
      }
    }
  }
#endif

  /* Call all post-query hooks */
  for(i = 0; i < pCtx->postQueryHooks.n; i++){
    LyrorePostQueryHook *pHook = &pCtx->postQueryHooks.a[i];
    if( pHook->xCollect ){
      pHook->xCollect(db, p, &stats, pHook->pCtx);
    }
  }

  /* Cleanup */
  sqlite3_free(stats.aLoops);
  sqlite3_free(stats.aCustom);
}

/*
** Invoke analyze hooks after ANALYZE completes.
*/
void lyroreInvokeAnalyzeHooks(sqlite3 *db, int iDb){
  LyroreContext *pCtx;
  int i;

  if( !db || !(db->flags & SQLITE_LyroreEnabled) ) return;

  pCtx = db->pLyrore;
  if( !pCtx || pCtx->analyzeHooks.n == 0 ) return;

  for(i = 0; i < pCtx->analyzeHooks.n; i++){
    LyroreAnalyzeHook *pHook = &pCtx->analyzeHooks.a[i];
    if( pHook->xAnalyze ){
      pHook->xAnalyze(db, iDb, pHook->pCtx);
    }
  }
}

#endif /* SQLITE_ENABLE_LYRORE */
