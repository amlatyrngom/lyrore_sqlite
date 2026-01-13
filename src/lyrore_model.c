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
** This file contains the implementation of the Lyrore model management
** API, including model registration, lookup, and feature utilities.
*/
#include "sqliteInt.h"

#ifdef SQLITE_ENABLE_LYRORE
#include "lyrore_model.h"

/*
** Register a model for a specific purpose.
**
** Returns SQLITE_OK on success.
** Returns SQLITE_ERROR if purpose already registered.
** Returns SQLITE_NOMEM if allocation fails.
*/
int lyroreRegisterModel(
  sqlite3 *db,
  const char *zPurpose,
  LyroreModelOps *pOps,
  const char *zConfig
){
  LyroreContext *ctx;
  LyroreModelEntry *pEntry;
  void *pState;

  if( db==0 || zPurpose==0 || pOps==0 ) return SQLITE_ERROR;
  ctx = db->pLyrore;
  if( ctx==0 ) return SQLITE_ERROR;

  /* Check if purpose already registered */
  for(pEntry = ctx->pModels; pEntry; pEntry = pEntry->pNext){
    if( sqlite3_stricmp(pEntry->zPurpose, zPurpose)==0 ){
      return SQLITE_ERROR;  /* Already registered */
    }
  }

  /* Allocate entry */
  pEntry = sqlite3MallocZero(sizeof(*pEntry));
  if( pEntry==0 ) return SQLITE_NOMEM;

  /* Copy purpose string */
  pEntry->zPurpose = sqlite3_mprintf("%s", zPurpose);
  if( pEntry->zPurpose==0 ){
    sqlite3_free(pEntry);
    return SQLITE_NOMEM;
  }

  /* Create model state */
  if( pOps->xCreate ){
    pState = pOps->xCreate(db, zConfig);
    if( pState==0 ){
      sqlite3_free(pEntry->zPurpose);
      sqlite3_free(pEntry);
      return SQLITE_NOMEM;
    }
    pEntry->pState = pState;
  }

  /* Try to load from persistence */
  if( pOps->xLoad && pEntry->pState && ctx->pStateDb ){
    pOps->xLoad(pEntry->pState, ctx->pStateDb);
  }

  pEntry->pOps = pOps;

  /* Add to linked list (prepend) */
  pEntry->pNext = ctx->pModels;
  ctx->pModels = pEntry;
  ctx->nModels++;

  return SQLITE_OK;
}

/*
** Unregister a model.
**
** Returns SQLITE_OK on success.
** Returns SQLITE_NOTFOUND if purpose not found.
*/
int lyroreUnregisterModel(sqlite3 *db, const char *zPurpose){
  LyroreContext *ctx;
  LyroreModelEntry *pEntry, *pPrev = 0;

  if( db==0 || zPurpose==0 ) return SQLITE_ERROR;
  ctx = db->pLyrore;
  if( ctx==0 ) return SQLITE_ERROR;

  /* Find entry */
  for(pEntry = ctx->pModels; pEntry; pPrev = pEntry, pEntry = pEntry->pNext){
    if( sqlite3_stricmp(pEntry->zPurpose, zPurpose)==0 ){
      /* Save before destroying if dirty */
      if( ctx->dirty > 0 && pEntry->pOps->xSave && pEntry->pState && ctx->pStateDb ){
        pEntry->pOps->xSave(pEntry->pState, ctx->pStateDb);
      }

      /* Destroy model state */
      if( pEntry->pOps->xDestroy && pEntry->pState ){
        pEntry->pOps->xDestroy(pEntry->pState);
      }

      /* Remove from linked list */
      if( pPrev ){
        pPrev->pNext = pEntry->pNext;
      }else{
        ctx->pModels = pEntry->pNext;
      }

      sqlite3_free(pEntry->zPurpose);
      sqlite3_free(pEntry);
      ctx->nModels--;
      return SQLITE_OK;
    }
  }

  return SQLITE_NOTFOUND;
}

/*
** Get model operations for a purpose.
**
** Returns NULL if not found.
*/
LyroreModelOps *lyroreGetModel(sqlite3 *db, const char *zPurpose){
  LyroreContext *ctx;
  LyroreModelEntry *pEntry;

  if( db==0 || zPurpose==0 ) return 0;
  ctx = db->pLyrore;
  if( ctx==0 ) return 0;

  for(pEntry = ctx->pModels; pEntry; pEntry = pEntry->pNext){
    if( sqlite3_stricmp(pEntry->zPurpose, zPurpose)==0 ){
      return pEntry->pOps;
    }
  }
  return 0;
}

/*
** Get model state for a purpose.
**
** Returns NULL if not found.
*/
void *lyroreGetModelState(sqlite3 *db, const char *zPurpose){
  LyroreContext *ctx;
  LyroreModelEntry *pEntry;

  if( db==0 || zPurpose==0 ) return 0;
  ctx = db->pLyrore;
  if( ctx==0 ) return 0;

  for(pEntry = ctx->pModels; pEntry; pEntry = pEntry->pNext){
    if( sqlite3_stricmp(pEntry->zPurpose, zPurpose)==0 ){
      return pEntry->pState;
    }
  }
  return 0;
}

/*
** Initialize a LyroreFeatures struct to zeros.
*/
void lyroreInitFeatures(LyroreFeatures *pFeat){
  if( pFeat ){
    memset(pFeat, 0, sizeof(*pFeat));
  }
}

/*
** Free any allocated memory in LyroreFeatures.
*/
void lyroreFreeFeatures(LyroreFeatures *pFeat){
  if( pFeat && pFeat->aExtended ){
    sqlite3_free(pFeat->aExtended);
    pFeat->aExtended = 0;
    pFeat->nExtended = 0;
  }
}

/*
** Convert features to flat array (all features).
**
** Returns number of features written.
*/
int lyroreFeaturesToArray(LyroreFeatures *pFeat, double *aOut, int nMax){
  return lyroreFeaturesToArrayMasked(pFeat, aOut, nMax, LYRORE_FEAT_ALL);
}

/*
** Convert features to flat array with mask.
**
** Returns number of features written.
** Features are written in canonical order based on mask bits.
*/
int lyroreFeaturesToArrayMasked(
  LyroreFeatures *pFeat,
  double *aOut,
  int nMax,
  u32 featureMask
){
  int n = 0;

  if( pFeat==0 || aOut==0 || nMax<=0 ) return 0;

  /* Query structure features */
  if( (featureMask & LYRORE_FEAT_NTABLES) && n < nMax ){
    aOut[n++] = (double)pFeat->nTables;
  }
  if( (featureMask & LYRORE_FEAT_NJOINS) && n < nMax ){
    aOut[n++] = (double)pFeat->nJoins;
  }
  if( (featureMask & LYRORE_FEAT_NPREDICATES) && n < nMax ){
    aOut[n++] = (double)pFeat->nPredicates;
  }
  if( (featureMask & LYRORE_FEAT_NAGGREGATIONS) && n < nMax ){
    aOut[n++] = (double)pFeat->nAggregations;
  }
  if( (featureMask & LYRORE_FEAT_QUERYDEPTH) && n < nMax ){
    aOut[n++] = (double)pFeat->queryDepth;
  }
  if( (featureMask & LYRORE_FEAT_NPROJECTIONS) && n < nMax ){
    aOut[n++] = (double)pFeat->nProjections;
  }

  /* Cost estimate features */
  if( (featureMask & LYRORE_FEAT_ESTTOTALROWS) && n < nMax ){
    aOut[n++] = pFeat->estTotalRows;
  }
  if( (featureMask & LYRORE_FEAT_ESTOUTPUTROWS) && n < nMax ){
    aOut[n++] = pFeat->estOutputRows;
  }
  if( (featureMask & LYRORE_FEAT_ESTCOST) && n < nMax ){
    aOut[n++] = pFeat->estCost;
  }
  if( (featureMask & LYRORE_FEAT_AVGSELECTIVITY) && n < nMax ){
    aOut[n++] = pFeat->avgSelectivity;
  }

  /* Access pattern features */
  if( (featureMask & LYRORE_FEAT_NINDEXSCANS) && n < nMax ){
    aOut[n++] = (double)pFeat->nIndexScans;
  }
  if( (featureMask & LYRORE_FEAT_NTABLESCANS) && n < nMax ){
    aOut[n++] = (double)pFeat->nTableScans;
  }
  if( (featureMask & LYRORE_FEAT_HASEQUALITY) && n < nMax ){
    aOut[n++] = (double)pFeat->hasEquality;
  }
  if( (featureMask & LYRORE_FEAT_HASRANGE) && n < nMax ){
    aOut[n++] = (double)pFeat->hasRange;
  }
  if( (featureMask & LYRORE_FEAT_HASLIKE) && n < nMax ){
    aOut[n++] = (double)pFeat->hasLike;
  }

  /* Historical features */
  if( (featureMask & LYRORE_FEAT_LASTEXECTIME) && n < nMax ){
    aOut[n++] = pFeat->lastExecTime;
  }
  if( (featureMask & LYRORE_FEAT_AVGEXECTIME) && n < nMax ){
    aOut[n++] = pFeat->avgExecTime;
  }
  if( (featureMask & LYRORE_FEAT_EXECCOUNT) && n < nMax ){
    aOut[n++] = (double)pFeat->execCount;
  }

  /* Extended features (if present and mask allows all features) */
  if( (featureMask & LYRORE_FEAT_ALL) == LYRORE_FEAT_ALL ){
    int i;
    for(i = 0; i < pFeat->nExtended && n < nMax; i++){
      aOut[n++] = pFeat->aExtended[i];
    }
  }

  return n;
}

#endif /* SQLITE_ENABLE_LYRORE */
