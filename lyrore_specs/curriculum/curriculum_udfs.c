/*
 * SQLite C UDF Extension for Curriculum Workload
 * 
 * UDFs implemented:
 *   - parse_json_score(json_str, field, default) -> int
 *   - get_nation_info(nkey, include_region) -> text
 *   - get_supplier_status(skey) -> text
 *   - get_supplier_risk_score(skey) -> real
 *   - get_customer_order_total(ckey, year) -> real
 * 
 * Compile with:
 *   gcc -shared -fPIC -o curriculum_udfs.so curriculum_udfs.c -I/usr/include -lm
 * 
 * Or with custom SQLite:
 *   gcc -shared -fPIC -o curriculum_udfs.so curriculum_udfs.c -I<sqlite_src>/src -lm
 */

#include <sqlite3ext.h>
SQLITE_EXTENSION_INIT1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>

/* ============================================================================
 * P4: parse_json_score(json_str TEXT, field TEXT, default_val INT) -> INT
 * 
 * Simple JSON parser - extracts integer value for a given field.
 * Expected format: {"field": value, ...}
 * ============================================================================ */
static void parse_json_score_func(
    sqlite3_context *context,
    int argc,
    sqlite3_value **argv
) {
    const char *json_str;
    const char *field;
    int default_val;

    if (argc != 3) {
        sqlite3_result_error(context, "parse_json_score requires 3 arguments", -1);
        return;
    }

    /* Handle NULL inputs */
    if (sqlite3_value_type(argv[0]) == SQLITE_NULL) {
        sqlite3_result_int(context, sqlite3_value_int(argv[2]));
        return;
    }

    json_str = (const char *)sqlite3_value_text(argv[0]);
    field = (const char *)sqlite3_value_text(argv[1]);
    default_val = sqlite3_value_int(argv[2]);

    if (!json_str || !field) {
        sqlite3_result_int(context, default_val);
        return;
    }

    /* Simple JSON parsing - find "field": value */
    char search_pattern[256];
    snprintf(search_pattern, sizeof(search_pattern), "\"%s\":", field);

    const char *pos = strstr(json_str, search_pattern);
    if (!pos) {
        sqlite3_result_int(context, default_val);
        return;
    }

    /* Move past the field name and colon */
    pos += strlen(search_pattern);

    /* Skip whitespace */
    while (*pos && isspace(*pos)) pos++;

    if (!*pos) {
        sqlite3_result_int(context, default_val);
        return;
    }

    /* Parse the integer value */
    char *endptr;
    long value = strtol(pos, &endptr, 10);

    if (endptr == pos) {
        /* No valid number found */
        sqlite3_result_int(context, default_val);
        return;
    }

    sqlite3_result_int(context, (int)value);
}

/* ============================================================================
 * S1: get_nation_info(nkey INT, include_region INT) -> TEXT
 * 
 * Returns nation name, optionally with region in parentheses.
 * Uses SQL query internally.
 * ============================================================================ */
static void get_nation_info_func(
    sqlite3_context *context,
    int argc,
    sqlite3_value **argv
) {
    sqlite3 *db;
    sqlite3_stmt *stmt;
    int nkey;
    int include_region;
    int rc;
    char result[512];

    if (argc != 2) {
        sqlite3_result_error(context, "get_nation_info requires 2 arguments", -1);
        return;
    }

    if (sqlite3_value_type(argv[0]) == SQLITE_NULL) {
        sqlite3_result_null(context);
        return;
    }

    nkey = sqlite3_value_int(argv[0]);
    include_region = sqlite3_value_int(argv[1]);

    db = sqlite3_context_db_handle(context);

    if (include_region) {
        /* Query with region */
        const char *sql = 
            "SELECT n.n_name, r.r_name FROM nation n "
            "JOIN region r ON n.n_regionkey = r.r_regionkey "
            "WHERE n.n_nationkey = ?";

        rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
        if (rc != SQLITE_OK) {
            sqlite3_result_error(context, "Failed to prepare statement", -1);
            return;
        }

        sqlite3_bind_int(stmt, 1, nkey);

        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *nation_name = (const char *)sqlite3_column_text(stmt, 0);
            const char *region_name = (const char *)sqlite3_column_text(stmt, 1);
            snprintf(result, sizeof(result), "%s (%s)", 
                     nation_name ? nation_name : "", 
                     region_name ? region_name : "");
            sqlite3_result_text(context, result, -1, SQLITE_TRANSIENT);
        } else {
            sqlite3_result_null(context);
        }

        sqlite3_finalize(stmt);
    } else {
        /* Query without region */
        const char *sql = "SELECT n_name FROM nation WHERE n_nationkey = ?";

        rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
        if (rc != SQLITE_OK) {
            sqlite3_result_error(context, "Failed to prepare statement", -1);
            return;
        }

        sqlite3_bind_int(stmt, 1, nkey);

        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *nation_name = (const char *)sqlite3_column_text(stmt, 0);
            sqlite3_result_text(context, nation_name, -1, SQLITE_TRANSIENT);
        } else {
            sqlite3_result_null(context);
        }

        sqlite3_finalize(stmt);
    }
}

/* ============================================================================
 * S2: get_supplier_status(skey INT) -> TEXT
 * 
 * Returns supplier status based on account balance:
 *   bal > 5000 -> 'PREMIUM'
 *   bal > 0    -> 'STANDARD'
 *   else       -> 'PROBATION'
 * ============================================================================ */
static void get_supplier_status_func(
    sqlite3_context *context,
    int argc,
    sqlite3_value **argv
) {
    sqlite3 *db;
    sqlite3_stmt *stmt;
    int skey;
    int rc;

    if (argc != 1) {
        sqlite3_result_error(context, "get_supplier_status requires 1 argument", -1);
        return;
    }

    if (sqlite3_value_type(argv[0]) == SQLITE_NULL) {
        sqlite3_result_null(context);
        return;
    }

    skey = sqlite3_value_int(argv[0]);
    db = sqlite3_context_db_handle(context);

    const char *sql = "SELECT s_acctbal FROM supplier WHERE s_suppkey = ?";

    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        sqlite3_result_error(context, "Failed to prepare statement", -1);
        return;
    }

    sqlite3_bind_int(stmt, 1, skey);

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        double bal = sqlite3_column_double(stmt, 0);
        if (bal > 5000) {
            sqlite3_result_text(context, "PREMIUM", -1, SQLITE_STATIC);
        } else if (bal > 0) {
            sqlite3_result_text(context, "STANDARD", -1, SQLITE_STATIC);
        } else {
            sqlite3_result_text(context, "PROBATION", -1, SQLITE_STATIC);
        }
    } else {
        sqlite3_result_null(context);
    }

    sqlite3_finalize(stmt);
}

/* ============================================================================
 * S3: get_supplier_risk_score(skey INT) -> REAL
 * 
 * Returns risk score based on supplier's parts:
 *   part_cnt > 5: (avg_cost * ln(part_cnt + 1)) / (total_qty + 1.0)
 *   part_cnt > 0: avg_cost / (total_qty + 1.0)
 *   else: 0.0
 * ============================================================================ */
static void get_supplier_risk_score_func(
    sqlite3_context *context,
    int argc,
    sqlite3_value **argv
) {
    sqlite3 *db;
    sqlite3_stmt *stmt;
    int skey;
    int rc;

    if (argc != 1) {
        sqlite3_result_error(context, "get_supplier_risk_score requires 1 argument", -1);
        return;
    }

    if (sqlite3_value_type(argv[0]) == SQLITE_NULL) {
        sqlite3_result_null(context);
        return;
    }

    skey = sqlite3_value_int(argv[0]);
    db = sqlite3_context_db_handle(context);

    const char *sql = 
        "SELECT COUNT(*), AVG(ps_supplycost), SUM(ps_availqty) "
        "FROM partsupp WHERE ps_suppkey = ?";

    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        sqlite3_result_error(context, "Failed to prepare statement", -1);
        return;
    }

    sqlite3_bind_int(stmt, 1, skey);

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        int part_cnt = sqlite3_column_int(stmt, 0);
        double avg_cost = sqlite3_column_double(stmt, 1);
        double total_qty = sqlite3_column_double(stmt, 2);
        double risk_score;

        if (part_cnt > 5) {
            risk_score = (avg_cost * log(part_cnt + 1)) / (total_qty + 1.0);
        } else if (part_cnt > 0) {
            risk_score = avg_cost / (total_qty + 1.0);
        } else {
            risk_score = 0.0;
        }

        /* Round to 4 decimal places */
        risk_score = round(risk_score * 10000) / 10000.0;
        sqlite3_result_double(context, risk_score);
    } else {
        sqlite3_result_double(context, 0.0);
    }

    sqlite3_finalize(stmt);
}

/* ============================================================================
 * S4: get_customer_order_total(ckey INT, year INT) -> REAL
 * 
 * Returns total order amount for a customer in a given year.
 * ============================================================================ */
static void get_customer_order_total_func(
    sqlite3_context *context,
    int argc,
    sqlite3_value **argv
) {
    sqlite3 *db;
    sqlite3_stmt *stmt;
    int ckey;
    int year;
    int rc;

    if (argc != 2) {
        sqlite3_result_error(context, "get_customer_order_total requires 2 arguments", -1);
        return;
    }

    if (sqlite3_value_type(argv[0]) == SQLITE_NULL) {
        sqlite3_result_double(context, 0.0);
        return;
    }

    ckey = sqlite3_value_int(argv[0]);
    year = sqlite3_value_int(argv[1]);
    db = sqlite3_context_db_handle(context);

    const char *sql = 
        "SELECT COALESCE(SUM(o_totalprice), 0) FROM orders "
        "WHERE o_custkey = ? AND CAST(strftime('%%Y', o_orderdate) AS INTEGER) = ?";

    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        sqlite3_result_error(context, "Failed to prepare statement", -1);
        return;
    }

    sqlite3_bind_int(stmt, 1, ckey);
    sqlite3_bind_int(stmt, 2, year);

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        double total = sqlite3_column_double(stmt, 0);
        sqlite3_result_double(context, total);
    } else {
        sqlite3_result_double(context, 0.0);
    }

    sqlite3_finalize(stmt);
}

/* ============================================================================
 * Extension Entry Point
 * ============================================================================ */
#ifdef _WIN32
__declspec(dllexport)
#endif
int sqlite3_curriculumudfs_init(
    sqlite3 *db,
    char **pzErrMsg,
    const sqlite3_api_routines *pApi
) {
    int rc = SQLITE_OK;
    SQLITE_EXTENSION_INIT2(pApi);

    /* Register parse_json_score */
    rc = sqlite3_create_function(db, "parse_json_score", 3, 
                                  SQLITE_UTF8 | SQLITE_DETERMINISTIC,
                                  NULL, parse_json_score_func, NULL, NULL);
    if (rc != SQLITE_OK) return rc;

    /* Register get_nation_info */
    rc = sqlite3_create_function(db, "get_nation_info", 2,
                                  SQLITE_UTF8,
                                  NULL, get_nation_info_func, NULL, NULL);
    if (rc != SQLITE_OK) return rc;

    /* Register get_supplier_status */
    rc = sqlite3_create_function(db, "get_supplier_status", 1,
                                  SQLITE_UTF8,
                                  NULL, get_supplier_status_func, NULL, NULL);
    if (rc != SQLITE_OK) return rc;

    /* Register get_supplier_risk_score */
    rc = sqlite3_create_function(db, "get_supplier_risk_score", 1,
                                  SQLITE_UTF8,
                                  NULL, get_supplier_risk_score_func, NULL, NULL);
    if (rc != SQLITE_OK) return rc;

    /* Register get_customer_order_total */
    rc = sqlite3_create_function(db, "get_customer_order_total", 2,
                                  SQLITE_UTF8,
                                  NULL, get_customer_order_total_func, NULL, NULL);
    if (rc != SQLITE_OK) return rc;

    return SQLITE_OK;
}
