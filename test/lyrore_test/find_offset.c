/* find_offset.c - Determine WhereLoop field offsets */
#include <stdio.h>
#include <stddef.h>

/* Basic SQLite types */
typedef unsigned long long int u64;
typedef u64 Bitmask;
typedef short int LogEst;
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef long long int i64;

/* Forward declarations for types we don't fully define */
struct Index;
struct ExprList;
struct WhereTerm;

/* Exact copy of WhereLoop structure from sqlite3.c */
struct WhereLoop {
  Bitmask prereq;       /* Bitmask of other loops that must run first */
  Bitmask maskSelf;     /* Bitmask identifying table iTab */
#ifdef SQLITE_DEBUG
  char cId;             /* Symbolic ID of this loop for debugging use */
#endif
  u8 iTab;              /* Position in FROM clause of table for this loop */
  u8 iSortIdx;          /* Sorting index number.  0==None */
  LogEst rSetup;        /* One-time setup cost (ex: create transient index) */
  LogEst rRun;          /* Cost of running each loop */
  LogEst nOut;          /* Estimated number of output rows */
  union {
    struct {               /* Information for internal btree tables */
      u16 nEq;               /* Number of equality constraints */
      u16 nBtm;              /* Size of BTM vector */
      u16 nTop;              /* Size of TOP vector */
      u16 nDistinctCol;      /* Index columns used to sort for DISTINCT */
      struct Index *pIndex;         /* Index used, or NULL */
      struct ExprList *pOrderBy;    /* ORDER BY clause if this is really a subquery */
    } btree;
    struct {               /* Information for virtual tables */
      int idxNum;            /* Index number */
      u32 needFree : 1;      /* True if sqlite3_free(idxStr) is needed */
      u32 bOmitOffset : 1;   /* True to let virtual table handle offset */
      u32 bIdxNumHex : 1;    /* Show idxNum as hex in EXPLAIN QUERY PLAN */
      i64 isOrdered;         /* True if satisfies ORDER BY -- Changed to i64 for alignment check */
      u16 omitMask;          /* Terms that may be omitted */
      char *idxStr;          /* Index identifier string */
      u32 mHandleIn;         /* Terms to handle as IN(...) instead of == */
    } vtab;
  } u;
  u32 wsFlags;          /* WHERE_* flags describing the plan */
  u16 nLTerm;           /* Number of entries in aLTerm[] */
  u16 nSkip;            /* Number of NULL aLTerm[] entries */
  u16 nLSlot;           /* Number of slots allocated for aLTerm[] */
#ifdef WHERETRACE_ENABLED
  LogEst rStarDelta;    /* Cost delta due to star-schema heuristic */
#endif
  struct WhereTerm **aLTerm;   /* WhereTerms used */
  struct WhereLoop *pNextLoop; /* Next WhereLoop object in the WhereClause */
  struct WhereTerm *aLTermSpace[3];  /* Initial aLTerm[] space */
};

int main() {
    printf("WhereLoop structure offsets (without SQLITE_DEBUG):\n");
    printf("  sizeof(WhereLoop) = %zu\n", sizeof(struct WhereLoop));
    printf("  prereq offset     = %zu\n", offsetof(struct WhereLoop, prereq));
    printf("  maskSelf offset   = %zu\n", offsetof(struct WhereLoop, maskSelf));
    printf("  iTab offset       = %zu\n", offsetof(struct WhereLoop, iTab));
    printf("  iSortIdx offset   = %zu\n", offsetof(struct WhereLoop, iSortIdx));
    printf("  rSetup offset     = %zu\n", offsetof(struct WhereLoop, rSetup));
    printf("  rRun offset       = %zu\n", offsetof(struct WhereLoop, rRun));
    printf("  nOut offset       = %zu\n", offsetof(struct WhereLoop, nOut));
    printf("  wsFlags offset    = %zu\n", offsetof(struct WhereLoop, wsFlags));
    return 0;
}
