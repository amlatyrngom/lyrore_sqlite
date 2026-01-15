/*
** product_filter UDF - INTENTIONALLY SLOW VERSION (v2)
** Increased iteration count for clearer speedup measurement
*/
#include "sqlite3ext.h"
SQLITE_EXTENSION_INIT1

static void product_filter_func(
    sqlite3_context *ctx,
    int argc,
    sqlite3_value **argv
){
    if(argc != 3) {
        sqlite3_result_error(ctx, "product_filter requires 3 arguments", -1);
        return;
    }
    
    double price = sqlite3_value_double(argv[0]);
    double qty = sqlite3_value_double(argv[1]);
    double threshold = sqlite3_value_double(argv[2]);
    
    /* More iterations for clearer overhead measurement */
    volatile double sum = 0.0;
    for(int i = 0; i < 2000; i++) {
        sum += (price * 0.001) + (qty * 0.001);
    }
    
    double product = price * qty + (sum * 0.0);
    
    if(product < threshold) {
        sqlite3_result_int(ctx, 1);
    } else {
        sqlite3_result_int(ctx, 0);
    }
}

#ifdef _WIN32
__declspec(dllexport)
#endif
int sqlite3_productfilterudf_init(
    sqlite3 *db,
    char **pzErrMsg,
    const sqlite3_api_routines *pApi
){
    SQLITE_EXTENSION_INIT2(pApi);
    sqlite3_create_function(db, "product_filter", 3, SQLITE_UTF8, 0,
                            product_filter_func, 0, 0);
    return SQLITE_OK;
}
