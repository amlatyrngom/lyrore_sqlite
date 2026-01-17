/*
** Lyrore C ABI Interface
** 
** Minimal C glue for integrating C++ plugin SDK with SQLite.
** This is the only Lyrore C code that needs to live inside SQLite.
*/
#ifndef SQLITE_LYRORE_CABI_H
#define SQLITE_LYRORE_CABI_H

#ifdef __cplusplus
extern "C" {
#endif

#ifdef SQLITE_ENABLE_LYRORE

/* Forward declarations for types used in wrappers */
struct Expr;
struct ExprList;
struct Select;
struct Parse;
struct sqlite3;

/* Use numeric type aliases that don't require sqliteInt.h */
typedef unsigned long long u64_wrapper;
typedef short LogEst_wrapper;

/* Opaque pointer to C++ context */
typedef struct LyroreCppContext LyroreCppContext;

/* Forward declaration for LyroreMain */
typedef struct LyroreMainHandle LyroreMainHandle;

/* Pre-parse hook (NEW in Step 5) */
int lyroreInvokePreParseHook(sqlite3* db, const char** pzSql);

/* Function pointer types for LyroreMain callbacks (registered dynamically) */
typedef int (*LyrorePreParseFunc)(sqlite3* db, const char* zSql, char** pzModified);
typedef int (*LyrorePreOptFunc)(void* pLyroreMain, sqlite3* db, void* pParse, void* pSelect);
typedef void (*LyroreEstimateFunc)(void* pLyroreMain, sqlite3* db, void* pBuilder, void* pLoop);
typedef void (*LyrorePostQueryFunc)(void* pLyroreMain, sqlite3* db, void* pVdbe);
typedef void (*LyroreAnalyzeFunc)(void* pLyroreMain, sqlite3* db, int iDb);

/* Register LyroreMain callbacks (called when LyroreMain is instantiated) */
void lyrore_register_main_callbacks(
    LyrorePreParseFunc preparse_func,
    LyrorePreOptFunc preopt_func,
    LyroreEstimateFunc estimate_func,
    LyrorePostQueryFunc postquery_func,
    LyroreAnalyzeFunc analyze_func
);

/* Set/get LyroreMain pointer for a database connection */
void lyrore_main_set_db(sqlite3* db, LyroreMainHandle* main);
LyroreMainHandle* lyrore_main_get_db(sqlite3* db);

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

/* Context lifecycle - called from main.c */
int lyroreInit(sqlite3 *db);
void lyroreShutdown(sqlite3 *db);

/* State management */
void lyrorePersistNow(sqlite3 *db);
void lyroreReset(sqlite3 *db);

/* Hook wrappers - called from hook integration points */
int lyroreInvokePreOptHooks(sqlite3 *db, void *pParse, void *pSelect);
void lyroreInvokeEstimateHooks(sqlite3 *db, void *pBuilder, void *pLoop);
void lyroreInvokeAnalyzeHooks(sqlite3 *db, int iDb);
void lyroreInvokePostQueryHooks(sqlite3 *db, void *pVdbe);

/* ============================================================
** Internal SQLite Function Wrappers
** These provide C++ SDK access to SQLITE_PRIVATE functions
** ============================================================ */

/* Expression functions */
struct Expr* lyrore_sqlite3Expr(sqlite3* db, int op, const char* zToken);
struct Expr* lyrore_sqlite3ExprDup(sqlite3* db, struct Expr* pExpr, int dupFlags);
void lyrore_sqlite3ExprDelete(sqlite3* db, struct Expr* pExpr);

/* ExprList functions */
struct ExprList* lyrore_sqlite3ExprListAppend(struct Parse* pParse, 
                                              struct ExprList* pList, 
                                              struct Expr* pExpr);
void lyrore_sqlite3ExprListDelete(sqlite3* db, struct ExprList* pList);


/* SrcList functions */
struct SrcList;
struct SrcList* lyrore_sqlite3SrcListAppend(struct Parse* pParse, 
                                            struct SrcList* pList,
                                            struct Token* pTable,
                                            struct Token* pDatabase);
void lyrore_sqlite3SrcListDelete(sqlite3* db, struct SrcList* pList);

/* Token struct (simplified) */
struct Token;

/* Select functions */
struct Select* lyrore_sqlite3SelectDup(sqlite3* db, struct Select* pSelect, int flags);
void lyrore_sqlite3SelectDelete(sqlite3* db, struct Select* pSelect);

/* Utility functions */
void lyrore_sqlite3DbFree(sqlite3* db, void* p);
int lyrore_sqlite3StrICmp(const char* zLeft, const char* zRight);
char* lyrore_sqlite3DbStrDup(sqlite3* db, const char* z);

/* LogEst conversion functions */
LogEst_wrapper lyrore_sqlite3LogEst(u64_wrapper x);
u64_wrapper lyrore_sqlite3LogEstToInt(LogEst_wrapper x);


/* Additional functions for AST rewrite support */
struct Table* lyrore_sqlite3FindTable(sqlite3* db, const char* zName, const char* zDatabase);
void lyrore_sqlite3SrcListAssignCursors(struct Parse* pParse, struct SrcList* pList);
int lyrore_get_parse_nTab(struct Parse* pParse);
void* lyrore_sqlite3DbMallocZero(sqlite3* db, u64 n);
void lyrore_debug_dump_schema(sqlite3* db);

#endif /* SQLITE_ENABLE_LYRORE */

#ifdef __cplusplus
}
#endif

#endif /* SQLITE_LYRORE_CABI_H */
