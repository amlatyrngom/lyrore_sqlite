#!/bin/bash
# Build SQLite with Lyrore C++ SDK integrated
# This compiles the SDK implementation into SQLite itself

set -e

SQLITE_SRC="${SQLITE_SRC:-$HOME/sqlite}"
BUILD_DIR="${BUILD_DIR:-$HOME/sqlite_build}"
SDK_DIR="$SQLITE_SRC/ext/lyrore_sdk"

cd "$BUILD_DIR"

# Configure (use amalgamation for now)
"$SQLITE_SRC/configure"

# First build amalgamation normally
make -j4 "OPTS=-DSQLITE_ENABLE_LYRORE=1 -DSQLITE_ENABLE_STMT_SCANSTATUS=1"

# Now relink sqlite3 binary with SDK C++ code
# The SDK needs SQLite headers and full internal access
g++ -std=c++17 -fPIC -O2 -rdynamic \
    -DSQLITE_ENABLE_MATH_FUNCTIONS -DSQLITE_ENABLE_PERCENTILE -DSQLITE_THREADSAFE=1 \
    -DSQLITE_ENABLE_LYRORE=1 -DSQLITE_ENABLE_STMT_SCANSTATUS=1 \
    -D_HAVE_SQLITE_CONFIG_H -DBUILD_sqlite -DNDEBUG \
    -I. -I"$SQLITE_SRC/src" \
    -I"$SQLITE_SRC/ext/rtree" -I"$SQLITE_SRC/ext/icu" \
    -I"$SQLITE_SRC/ext/fts3" -I"$SQLITE_SRC/ext/session" \
    -I"$SQLITE_SRC/ext/misc" -I"$SDK_DIR/include" \
    -o sqlite3_cpp \
    shell.c sqlite3.c \
    "$SDK_DIR/src/ly_expr.cpp" "$SDK_DIR/src/cpp_context.cpp" \
    -DSQLITE_DQS=0 -DSQLITE_ENABLE_FTS4 -DSQLITE_ENABLE_RTREE \
    -DSQLITE_ENABLE_EXPLAIN_COMMENTS -DSQLITE_ENABLE_UNKNOWN_SQL_FUNCTION \
    -DSQLITE_ENABLE_STMTVTAB -DSQLITE_ENABLE_DBPAGE_VTAB \
    -DSQLITE_ENABLE_DBSTAT_VTAB -DSQLITE_ENABLE_BYTECODE_VTAB \
    -DSQLITE_ENABLE_OFFSET_SQL_FUNC -DSQLITE_ENABLE_PERCENTILE \
    -DSQLITE_STRICT_SUBTYPE=1 -lm -ldl

echo "Built: $BUILD_DIR/sqlite3_cpp"
