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
** This file contains the implementation of the Lyrore Pattern Matching
** Framework. It provides pattern registration, dispatch, and persistence.
*/
#include "sqliteInt.h"

#ifdef SQLITE_ENABLE_LYRORE

#include "lyrore_model.h"
#include "lyrore_features.h"
#include "lyrore_pattern.h"

/*
** Initial allocation size for pattern array
*/
#define LYRORE_PAT_INITIAL_ALLOC 8

/*
** Helper: Get registry for a category from context
*/
static LyrorePatternRegistry *lyroreGetRegistry(
  LyroreContext *ctx, 
  LyrorePatternCategory cat
){
  if( ctx==0 ) return 0;
  switch( cat ){
    case LYRORE_CAT_COST: return &ctx->costPatterns;
    case LYRORE_CAT_PLAN: return &ctx->planPatterns;
    case LYRORE_CAT_EXPR: return &ctx->exprPatterns;
    default: return 0;
  }
}

/*
** Initialize a pattern registry with default pattern.
*/
void lyroreInitPatternRegistry(LyrorePatternRegistry *pReg){
  if( pReg==0 ) return;
  memset(pReg, 0, sizeof(*pReg));
  
  /* Initialize default pattern */
  pReg->defaultPat.id = LYRORE_PAT_DEFAULT;
  pReg->defaultPat.zName = "default";
  pReg->defaultPat.category = LYRORE_CAT_COST;  /* Will be set properly on use */
  pReg->defaultPat.priority = -1;  /* Lower than any real pattern */
  pReg->defaultPat.matcher.xGenericMatch = 0;
  pReg->defaultPat.features.xGenericFeatures = 0;
  pReg->defaultPat.nExtendedFeatures = 0;
  pReg->defaultPat.pCostModel = 0;
  pReg->defaultPat.pCostState = 0;
  pReg->defaultPat.pSelectModel = 0;
  pReg->defaultPat.pSelectState = 0;
  pReg->defaultPat.pMatchCtx = 0;
}

/*
** Destroy a pattern registry, freeing all patterns.
*/
void lyroreDestroyPatternRegistry(LyrorePatternRegistry *pReg){
  int i;
  if( pReg==0 ) return;
  
  /* Free pattern definitions (but not the patterns themselves - 
  ** they may be statically allocated by caller) */
  for(i = 0; i < pReg->nPatterns; i++){
    LyrorePatternDef *pPat = pReg->aPatterns[i];
    if( pPat ){
      /* Destroy pattern-specific model state if present */
      if( pPat->pCostModel && pPat->pCostModel->xDestroy && pPat->pCostState ){
        pPat->pCostModel->xDestroy(pPat->pCostState);
      }
      if( pPat->pSelectModel && pPat->pSelectModel->xDestroy && pPat->pSelectState ){
        pPat->pSelectModel->xDestroy(pPat->pSelectState);
      }
    }
  }
  
  sqlite3_free(pReg->aPatterns);
  memset(pReg, 0, sizeof(*pReg));
}

/*
** Insert a pattern into the registry, maintaining priority order.
** Returns the insertion index.
*/
static int lyroreInsertPatternSorted(
  LyrorePatternRegistry *pReg,
  LyrorePatternDef *pDef
){
  int i;
  int insertIdx;
  
  /* Find insertion point (descending priority order) */
  insertIdx = pReg->nPatterns;
  for(i = 0; i < pReg->nPatterns; i++){
    if( pReg->aPatterns[i]->priority < pDef->priority ){
      insertIdx = i;
      break;
    }
  }
  
  /* Shift elements to make room */
  for(i = pReg->nPatterns; i > insertIdx; i--){
    pReg->aPatterns[i] = pReg->aPatterns[i-1];
  }
  
  pReg->aPatterns[insertIdx] = pDef;
  pReg->nPatterns++;
  
  return insertIdx;
}

/*
** Register a pattern with the appropriate registry.
** The pattern definition is copied into registry-managed memory.
*/
int lyroreRegisterPattern(sqlite3 *db, LyrorePatternDef *pDef){
  LyroreContext *ctx;
  LyrorePatternRegistry *pReg;
  LyrorePatternDef *pNew;
  LyrorePatternDef **aNew;
  
  if( db==0 || pDef==0 ) return SQLITE_ERROR;
  ctx = db->pLyrore;
  if( ctx==0 ) return SQLITE_ERROR;
  
  /* Get registry for this category */
  pReg = lyroreGetRegistry(ctx, pDef->category);
  if( pReg==0 ) return SQLITE_ERROR;
  
  /* Check if we need to expand the array */
  if( pReg->nPatterns >= pReg->nAlloc ){
    int nNew = pReg->nAlloc==0 ? LYRORE_PAT_INITIAL_ALLOC : pReg->nAlloc * 2;
    aNew = sqlite3_realloc(pReg->aPatterns, nNew * sizeof(LyrorePatternDef*));
    if( aNew==0 ) return SQLITE_NOMEM;
    pReg->aPatterns = aNew;
    pReg->nAlloc = nNew;
  }
  
  /* Allocate and copy the pattern definition */
  pNew = sqlite3_malloc(sizeof(LyrorePatternDef));
  if( pNew==0 ) return SQLITE_NOMEM;
  memcpy(pNew, pDef, sizeof(LyrorePatternDef));
  
  /* Insert sorted by priority */
  lyroreInsertPatternSorted(pReg, pNew);
  
  return SQLITE_OK;
}

/*
** Unregister a pattern by ID. Searches all registries.
*/
int lyroreUnregisterPattern(sqlite3 *db, LyrorePatternId id){
  LyroreContext *ctx;
  LyrorePatternRegistry *aRegs[3];
  int r;
  int i;
  int j;
  
  if( db==0 ) return SQLITE_ERROR;
  ctx = db->pLyrore;
  if( ctx==0 ) return SQLITE_ERROR;
  
  aRegs[0] = &ctx->costPatterns;
  aRegs[1] = &ctx->planPatterns;
  aRegs[2] = &ctx->exprPatterns;
  
  for(r = 0; r < 3; r++){
    LyrorePatternRegistry *pReg = aRegs[r];
    for(i = 0; i < pReg->nPatterns; i++){
      if( pReg->aPatterns[i]->id == id ){
        LyrorePatternDef *pPat = pReg->aPatterns[i];
        
        /* Destroy pattern-specific model state */
        if( pPat->pCostModel && pPat->pCostModel->xDestroy && pPat->pCostState ){
          pPat->pCostModel->xDestroy(pPat->pCostState);
        }
        if( pPat->pSelectModel && pPat->pSelectModel->xDestroy && pPat->pSelectState ){
          pPat->pSelectModel->xDestroy(pPat->pSelectState);
        }
        
        /* Free the pattern definition */
        sqlite3_free(pPat);
        
        /* Shift array */
        for(j = i; j < pReg->nPatterns - 1; j++){
          pReg->aPatterns[j] = pReg->aPatterns[j+1];
        }
        pReg->nPatterns--;
        
        return SQLITE_OK;
      }
    }
  }
  
  return SQLITE_NOTFOUND;
}

/*
** Get a pattern by ID. Searches all registries.
*/
LyrorePatternDef *lyroreGetPattern(sqlite3 *db, LyrorePatternId id){
  LyroreContext *ctx;
  LyrorePatternRegistry *aRegs[3];
  int r;
  int i;
  
  if( db==0 ) return 0;
  ctx = db->pLyrore;
  if( ctx==0 ) return 0;
  
  /* Check for default pattern request */
  if( id == LYRORE_PAT_DEFAULT ){
    return &ctx->costPatterns.defaultPat;
  }
  
  aRegs[0] = &ctx->costPatterns;
  aRegs[1] = &ctx->planPatterns;
  aRegs[2] = &ctx->exprPatterns;
  
  for(r = 0; r < 3; r++){
    LyrorePatternRegistry *pReg = aRegs[r];
    for(i = 0; i < pReg->nPatterns; i++){
      if( pReg->aPatterns[i]->id == id ){
        return pReg->aPatterns[i];
      }
    }
  }
  
  return 0;
}

/*
** Dispatch to find matching cost pattern.
** Iterates patterns by priority, returns first match or default.
*/
LyrorePatternDef *lyroreDispatchCostPattern(
  sqlite3 *db,
  struct WhereLoop *pLoop,
  struct Table *pTab
){
  LyroreContext *ctx;
  LyrorePatternRegistry *pReg;
  int i;
  
  if( db==0 ) return 0;
  ctx = db->pLyrore;
  if( ctx==0 ) return 0;
  
  pReg = &ctx->costPatterns;
  
  /* Iterate patterns by priority (descending) */
  for(i = 0; i < pReg->nPatterns; i++){
    LyrorePatternDef *pPat = pReg->aPatterns[i];
    if( pPat->matcher.xCostMatch &&
        pPat->matcher.xCostMatch(pLoop, pTab, pPat->pMatchCtx) ){
      return pPat;
    }
  }
  
  return &pReg->defaultPat;
}

/*
** Dispatch to find matching plan pattern.
*/
LyrorePatternDef *lyroreDispatchPlanPattern(
  sqlite3 *db,
  struct WhereInfo *pWInfo
){
  LyroreContext *ctx;
  LyrorePatternRegistry *pReg;
  int i;
  
  if( db==0 ) return 0;
  ctx = db->pLyrore;
  if( ctx==0 ) return 0;
  
  pReg = &ctx->planPatterns;
  
  for(i = 0; i < pReg->nPatterns; i++){
    LyrorePatternDef *pPat = pReg->aPatterns[i];
    if( pPat->matcher.xPlanMatch &&
        pPat->matcher.xPlanMatch(pWInfo, pPat->pMatchCtx) ){
      return pPat;
    }
  }
  
  return &pReg->defaultPat;
}

/*
** Dispatch to find matching expression pattern.
*/
LyrorePatternDef *lyroreDispatchExprPattern(
  sqlite3 *db,
  struct Parse *pParse,
  struct Expr *pExpr
){
  LyroreContext *ctx;
  LyrorePatternRegistry *pReg;
  int i;
  
  if( db==0 ) return 0;
  ctx = db->pLyrore;
  if( ctx==0 ) return 0;
  
  pReg = &ctx->exprPatterns;
  
  for(i = 0; i < pReg->nPatterns; i++){
    LyrorePatternDef *pPat = pReg->aPatterns[i];
    if( pPat->matcher.xExprMatch &&
        pPat->matcher.xExprMatch(pParse, pExpr, pPat->pMatchCtx) ){
      return pPat;
    }
  }
  
  return &pReg->defaultPat;
}

/*
** Pattern-aware cost/cardinality estimation.
** Dispatches to matching pattern, extracts features, calls model.
** Returns 1 if estimates were adjusted, 0 if caller should use defaults.
*/
int lyrorePatternAdjustEstimates(
  sqlite3 *db,
  struct WhereLoop *pLoop,
  struct Table *pTab
){
  LyrorePatternDef *pPat;
  LyroreFeatures feat;
  double aFeat[64];
  int nFeat;
  LyroreModelOps *pOps;
  void *pState;
  double prediction;
  
  pPat = lyroreDispatchCostPattern(db, pLoop, pTab);
  if( pPat==0 || pPat->id==LYRORE_PAT_DEFAULT ){
    return 0;  /* No pattern match, use SQLite defaults */
  }
  
  /* Extract features */
  lyroreInitFeatures(&feat);
  if( pPat->features.xCostFeatures ){
    pPat->features.xCostFeatures(pLoop, pTab, &feat);
  }else{
    lyroreExtractLoopFeatures(pLoop, pTab, &feat);
  }
  nFeat = lyroreFeaturesToArray(&feat, aFeat, 64);
  
  /* Get model (pattern-specific or global) */
  pOps = pPat->pCostModel ? pPat->pCostModel : lyroreGetModel(db, "cost");
  pState = pPat->pCostState ? pPat->pCostState : lyroreGetModelState(db, "cost");
  
  if( pOps && pOps->xPredict ){
    /* Get prediction and apply as cost multiplier */
    prediction = pOps->xPredict(pState, aFeat, nFeat);
    if( prediction > 0.0 ){
      /* Apply as multiplicative factor to rRun 
      ** Note: In future, could use xPredict2 for separate cardinality/cost adjustments */
      i64 currentCost = (i64)lyroreLogEstToDouble((LogEst)pLoop->rRun);
      i64 adjustedCost = (i64)(currentCost * prediction);
      if( adjustedCost < 1 ) adjustedCost = 1;
      pLoop->rRun = sqlite3LogEst((u64)adjustedCost);
    }
    lyroreFreeFeatures(&feat);
    return 1;
  }
  
  lyroreFreeFeatures(&feat);
  return 0;
}

/*
** Pattern-aware plan selection.
** Returns selected option index (0 to nOptions-1).
*/
int lyrorePatternSelectPlan(
  sqlite3 *db,
  struct WhereInfo *pWInfo,
  int nOptions
){
  LyrorePatternDef *pPat;
  LyroreFeatures feat;
  double aFeat[64];
  int nFeat;
  LyroreModelOps *pOps;
  void *pState;
  int selection;
  
  if( nOptions <= 1 ) return 0;
  
  pPat = lyroreDispatchPlanPattern(db, pWInfo);
  if( pPat==0 || pPat->id==LYRORE_PAT_DEFAULT ){
    return 0;  /* Default: first option */
  }
  
  /* Extract features */
  lyroreInitFeatures(&feat);
  if( pPat->features.xPlanFeatures ){
    pPat->features.xPlanFeatures(pWInfo, &feat);
  }else{
    lyroreExtractPlanFeatures(pWInfo, &feat);
  }
  nFeat = lyroreFeaturesToArray(&feat, aFeat, 64);
  
  /* Get model (pattern-specific or global) */
  pOps = pPat->pSelectModel ? pPat->pSelectModel : lyroreGetModel(db, "plan");
  pState = pPat->pSelectState ? pPat->pSelectState : lyroreGetModelState(db, "plan");
  
  if( pOps && pOps->xSelect ){
    selection = pOps->xSelect(pState, aFeat, nFeat, nOptions);
    if( selection < 0 || selection >= nOptions ){
      selection = 0;  /* Bounds check */
    }
    lyroreFreeFeatures(&feat);
    return selection;
  }
  
  lyroreFreeFeatures(&feat);
  return 0;
}

/*
** Pattern-aware expression flavor selection.
** Returns selected flavor index (0 to nFlavors-1).
*/
int lyrorePatternSelectFlavor(
  sqlite3 *db,
  struct Parse *pParse,
  struct Expr *pExpr,
  int nFlavors
){
  LyrorePatternDef *pPat;
  LyroreFeatures feat;
  double aFeat[64];
  int nFeat;
  LyroreModelOps *pOps;
  void *pState;
  int selection;
  
  if( nFlavors <= 1 ) return 0;
  
  pPat = lyroreDispatchExprPattern(db, pParse, pExpr);
  if( pPat==0 || pPat->id==LYRORE_PAT_DEFAULT ){
    return 0;  /* Default: first flavor */
  }
  
  /* Extract features */
  lyroreInitFeatures(&feat);
  if( pPat->features.xExprFeatures ){
    pPat->features.xExprFeatures(pParse, pExpr, &feat);
  }else{
    lyroreExtractExprFeatures(pParse, pExpr, &feat);
  }
  nFeat = lyroreFeaturesToArray(&feat, aFeat, 64);
  
  /* Get model (pattern-specific or global) */
  pOps = pPat->pSelectModel ? pPat->pSelectModel : lyroreGetModel(db, "flavor");
  pState = pPat->pSelectState ? pPat->pSelectState : lyroreGetModelState(db, "flavor");
  
  if( pOps && pOps->xSelect ){
    selection = pOps->xSelect(pState, aFeat, nFeat, nFlavors);
    if( selection < 0 || selection >= nFlavors ){
      selection = 0;
    }
    lyroreFreeFeatures(&feat);
    return selection;
  }
  
  lyroreFreeFeatures(&feat);
  return 0;
}

/*
** Save a single pattern's model state to the database.
*/
int lyroreSavePatternState(sqlite3 *db, LyrorePatternDef *pPat){
  LyroreContext *ctx;
  int rc = SQLITE_OK;
  
  if( db==0 || pPat==0 ) return SQLITE_ERROR;
  ctx = db->pLyrore;
  if( ctx==0 || ctx->pStateDb==0 ) return SQLITE_OK;  /* No-op for in-memory */
  
  /* Save cost model state */
  if( pPat->pCostModel && pPat->pCostModel->xSave && pPat->pCostState ){
    rc = pPat->pCostModel->xSave(pPat->pCostState, ctx->pStateDb);
    if( rc!=SQLITE_OK ) return rc;
  }
  
  /* Save select model state */
  if( pPat->pSelectModel && pPat->pSelectModel->xSave && pPat->pSelectState ){
    rc = pPat->pSelectModel->xSave(pPat->pSelectState, ctx->pStateDb);
  }
  
  return rc;
}

/*
** Load a single pattern's model state from the database.
*/
int lyroreLoadPatternState(sqlite3 *db, LyrorePatternDef *pPat){
  LyroreContext *ctx;
  int rc = SQLITE_OK;
  
  if( db==0 || pPat==0 ) return SQLITE_ERROR;
  ctx = db->pLyrore;
  if( ctx==0 || ctx->pStateDb==0 ) return SQLITE_OK;
  
  /* Load cost model state */
  if( pPat->pCostModel && pPat->pCostModel->xLoad && pPat->pCostState ){
    rc = pPat->pCostModel->xLoad(pPat->pCostState, ctx->pStateDb);
    if( rc!=SQLITE_OK && rc!=SQLITE_NOTFOUND ) return rc;
  }
  
  /* Load select model state */
  if( pPat->pSelectModel && pPat->pSelectModel->xLoad && pPat->pSelectState ){
    rc = pPat->pSelectModel->xLoad(pPat->pSelectState, ctx->pStateDb);
    if( rc!=SQLITE_OK && rc!=SQLITE_NOTFOUND ) return rc;
  }
  
  return SQLITE_OK;
}

/*
** Persist all pattern states across all registries.
*/
int lyrorePersistAllPatterns(sqlite3 *db){
  LyroreContext *ctx;
  LyrorePatternRegistry *aRegs[3];
  int r;
  int i;
  int rc = SQLITE_OK;
  
  if( db==0 ) return SQLITE_OK;
  ctx = db->pLyrore;
  if( ctx==0 || ctx->pStateDb==0 ) return SQLITE_OK;
  
  aRegs[0] = &ctx->costPatterns;
  aRegs[1] = &ctx->planPatterns;
  aRegs[2] = &ctx->exprPatterns;
  
  rc = sqlite3_exec(ctx->pStateDb, "BEGIN IMMEDIATE", 0, 0, 0);
  if( rc!=SQLITE_OK ) return rc;
  
  for(r = 0; r < 3; r++){
    LyrorePatternRegistry *pReg = aRegs[r];
    for(i = 0; i < pReg->nPatterns; i++){
      rc = lyroreSavePatternState(db, pReg->aPatterns[i]);
      if( rc!=SQLITE_OK ){
        sqlite3_exec(ctx->pStateDb, "ROLLBACK", 0, 0, 0);
        return rc;
      }
    }
  }
  
  return sqlite3_exec(ctx->pStateDb, "COMMIT", 0, 0, 0);
}

#endif /* SQLITE_ENABLE_LYRORE */
