#include <stdio.h>
#include <stdlib.h>
#include "sqlite3.h"

int main() {
    sqlite3 *db;
    char *err = NULL;
    int rc;
    
    printf("Opening database...\n");
    rc = sqlite3_open("/tmp/test_lyrore.db", &db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Cannot open database: %s\n", sqlite3_errmsg(db));
        return 1;
    }
    
    printf("Enabling lyrore...\n");
    rc = sqlite3_exec(db, "PRAGMA lyrore_enabled = ON;", NULL, NULL, &err);
    printf("lyrore_enabled: rc=%d\n", rc);
    if (err) { printf("  error: %s\n", err); sqlite3_free(err); err = NULL; }
    
    rc = sqlite3_exec(db, "PRAGMA lyrore_plugins = ON;", NULL, NULL, &err);
    printf("lyrore_plugins: rc=%d\n", rc);
    if (err) { printf("  error: %s\n", err); sqlite3_free(err); err = NULL; }
    
    rc = sqlite3_exec(db, "PRAGMA lyrore_cost = ON;", NULL, NULL, &err);
    printf("lyrore_cost: rc=%d\n", rc);
    if (err) { printf("  error: %s\n", err); sqlite3_free(err); err = NULL; }
    
    printf("\nChecking if lyrore_register function exists...\n");
    sqlite3_stmt *stmt;
    rc = sqlite3_prepare_v2(db, "SELECT * FROM pragma_function_list WHERE name = 'lyrore_register';", -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            printf("Found: %s (narg=%d)\n", sqlite3_column_text(stmt, 0), sqlite3_column_int(stmt, 1));
        }
        sqlite3_finalize(stmt);
    }
    
    printf("\nAttempting to call lyrore_register...\n");
    rc = sqlite3_exec(db, "SELECT lyrore_register('./demo_plugin.so');", NULL, NULL, &err);
    printf("lyrore_register: rc=%d\n", rc);
    if (err) { 
        printf("  error: %s\n", err); 
        sqlite3_free(err); 
        err = NULL; 
    } else {
        printf("  SUCCESS!\n");
    }
    
    sqlite3_close(db);
    return 0;
}
