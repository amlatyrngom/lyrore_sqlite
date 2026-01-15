/* preopt_demo_plugin.c - Demonstrates pre-opt hook framework
 * 
 * Shows that pre-opt hooks ARE invoked and COULD transform expressions.
 * Actual UDF->native transformation would require complex AST manipulation.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Lyrore types */
typedef struct sqlite3 sqlite3;
typedef struct Parse Parse;
typedef struct Select Select;

typedef struct LyrorePreOptHook {
    const char *zName;
    int priority;
    int (*xRewrite)(sqlite3*, Parse*, Select*, void*);
    void *pCtx;
} LyrorePreOptHook;

typedef struct LyrorePluginInfo {
    int version;
    const char *name;
    void *aAnalyzeHooks;    int nAnalyzeHooks;
    LyrorePreOptHook *aPreOptHooks; int nPreOptHooks;
    void *aEstimateHooks;   int nEstimateHooks;
    void *aPostQueryHooks;  int nPostQueryHooks;
    int (*xInit)(sqlite3 *db, void **ppCtx);
    void (*xShutdown)(void *pCtx);
    void (*xSchemaChange)(sqlite3 *db, void *pCtx);
} LyrorePluginInfo;

#define LYRORE_REWRITE_NONE 0
#define LYRORE_REWRITE_MODIFIED 1

static int nInvocations = 0;

static int preOptRewrite(sqlite3 *db, Parse *p, Select *s, void *ctx) {
    nInvocations++;
    fprintf(stderr, "[PreOpt] Hook invoked (count=%d). AST rewrite framework active.\n", nInvocations);
    
    /* In a full implementation, we would:
     * 1. Walk the expression tree using SQLite's Walker pattern
     * 2. Find TK_FUNCTION nodes matching 'product_filter'
     * 3. Replace with equivalent TK_LT(TK_MUL(price, qty), threshold)
     * 4. Return LYRORE_REWRITE_MODIFIED
     * 
     * This requires access to SQLite internals (Expr, Walker, etc.)
     * which is complex but achievable with the hook framework.
     */
    return LYRORE_REWRITE_NONE;
}

static LyrorePreOptHook preOptHooks[] = {
    {"demo_preopt", 100, preOptRewrite, NULL}
};

__attribute__((visibility("default")))
LyrorePluginInfo* lyrore_plugin_info(void) {
    static LyrorePluginInfo info = {
        1, "preopt_demo",
        NULL, 0,        /* analyze */
        preOptHooks, 1, /* pre-opt */
        NULL, 0,        /* estimate */
        NULL, 0,        /* post-query */
        NULL, NULL, NULL
    };
    return &info;
}
