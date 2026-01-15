/* slow_udf.c - Deliberately slow UDF for performance comparison */
#include <sqlite3ext.h>
#include <time.h>

SQLITE_EXTENSION_INIT1

static void product_filter(
    sqlite3_context *ctx,
    int argc,
    sqlite3_value **argv
){
    double price = sqlite3_value_double(argv[0]);
    int qty = sqlite3_value_int(argv[1]);
    double threshold = sqlite3_value_double(argv[2]);
    
    /* Deliberate slowdown - busy loop */
    volatile int delay = 50000;
    while(delay--) { }
    
    sqlite3_result_int(ctx, (price * qty) < threshold ? 1 : 0);
}

int sqlite3_slowudf_init(
    sqlite3 *db,
    char **pzErrMsg,
    const sqlite3_api_routines *pApi
){
    SQLITE_EXTENSION_INIT2(pApi);
    return sqlite3_create_function(db, "product_filter", 3, SQLITE_UTF8, 0,
                                   product_filter, 0, 0);
}
