# Lyrore SDK Makefile
# Build C++ plugins for SQLite Lyrore
#
# IMPORTANT: Plugins do NOT include SDK sources (cpp_context.cpp, pattern.cpp).
# These are compiled into libsqlite3.so and symbols are resolved at runtime.

# Configuration - adjust paths as needed
SQLITE_SRC ?= $(HOME)/sqlite/src
SQLITE_BUILD ?= $(HOME)/sqlite_build
SDK_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
SDK_INCLUDE := $(SDK_DIR)include
SDK_SRC := $(SDK_DIR)src

# Compiler settings
CXX ?= g++
CXXFLAGS := -std=c++17 -DSQLITE_ENABLE_LYRORE=1 -fPIC -O2 -Wall -Wextra
CXXFLAGS += -I$(SDK_INCLUDE) -I$(SQLITE_SRC) -I$(SQLITE_BUILD)
LDFLAGS_SHARED := -shared -ldl
LDFLAGS_EXE := -rdynamic -L$(SQLITE_BUILD) -Wl,-rpath,$(SQLITE_BUILD) -lsqlite3 -ldl -lpthread

# Default target
.PHONY: all clean examples tests step5_tests

all: examples tests step5_tests

# Example plugins
examples: udf_transform.so histogram.so pattern_e2e_test.so product_filter_udf.so fast_groupby.so

udf_transform.so: examples/udf_transform.cpp
	$(CXX) $(CXXFLAGS) $(LDFLAGS_SHARED) -o $@ $<

histogram.so: examples/histogram.cpp
	$(CXX) $(CXXFLAGS) $(LDFLAGS_SHARED) -o $@ $<

pattern_e2e_test.so: examples/pattern_e2e_test.cpp
	$(CXX) $(CXXFLAGS) $(LDFLAGS_SHARED) -o $@ $<

fast_groupby.so: examples/fast_groupby.cpp
	$(CXX) $(CXXFLAGS) $(LDFLAGS_SHARED) -o $@ $<

# UDF for testing
product_filter_udf.so: product_filter_udf.c
	gcc -fPIC -shared -O2 -I$(SQLITE_BUILD) -o $@ $<

# Test plugins (existing)
tests: pattern_unit_test.so real_pattern_match_test.so custom_op_test.so storage_test.so

pattern_unit_test.so: tests/pattern_unit_test.cpp
	$(CXX) $(CXXFLAGS) $(LDFLAGS_SHARED) -o $@ $<

real_pattern_match_test.so: tests/real_pattern_match_test.cpp
	$(CXX) $(CXXFLAGS) $(LDFLAGS_SHARED) -o $@ $<

custom_op_test.so: tests/custom_op_test.cpp
	$(CXX) $(CXXFLAGS) $(LDFLAGS_SHARED) -o $@ $<

storage_test.so: tests/storage_test.cpp
	$(CXX) $(CXXFLAGS) $(LDFLAGS_SHARED) -o $@ $<

# Step 5 test plugins and executable
step5_tests: counter_v1.so counter_v2.so dialect_test.so stats_test.so setter_test.so reader_test.so lyrore_main_test

counter_v1.so: tests/counter_v1.cpp
	$(CXX) $(CXXFLAGS) $(LDFLAGS_SHARED) -o $@ $<

counter_v2.so: tests/counter_v2.cpp
	$(CXX) $(CXXFLAGS) $(LDFLAGS_SHARED) -o $@ $<

dialect_test.so: tests/dialect_test.cpp
	$(CXX) $(CXXFLAGS) $(LDFLAGS_SHARED) -o $@ $<

stats_test.so: tests/stats_test.cpp
	$(CXX) $(CXXFLAGS) $(LDFLAGS_SHARED) -o $@ $<

setter_test.so: tests/setter_test.cpp
	$(CXX) $(CXXFLAGS) $(LDFLAGS_SHARED) -o $@ $<

reader_test.so: tests/reader_test.cpp
	$(CXX) $(CXXFLAGS) $(LDFLAGS_SHARED) -o $@ $<

# Main test executable - links against libsqlite3.so
lyrore_main_test: tests/lyrore_main_test.cpp src/lyrore_main.cpp
	$(CXX) $(CXXFLAGS) -o $@ tests/lyrore_main_test.cpp src/lyrore_main.cpp $(LDFLAGS_EXE)

clean:
	rm -f *.so lyrore_main_test

# Helper to build SQLite with Lyrore support
.PHONY: sqlite
sqlite:
	mkdir -p $(SQLITE_BUILD)
	cd $(SQLITE_BUILD) && $(SQLITE_SRC)/../configure
	cd $(SQLITE_BUILD) && $(MAKE) -j4 "OPTS=-DSQLITE_ENABLE_LYRORE=1 -DSQLITE_ENABLE_STMT_SCANSTATUS=1 -DSQLITE_ENABLE_PREUPDATE_HOOK=1" "LDFLAGS=-rdynamic"

