# Lyrore SDK Makefile
# Build C++ plugins for SQLite Lyrore
#
# IMPORTANT: Plugins do NOT include SDK sources (ly_expr.cpp, cpp_context.cpp).
# These are compiled into libsqlite3.so and symbols are resolved at runtime.

# Configuration - adjust paths as needed
SQLITE_SRC ?= $(HOME)/sqlite/src
SQLITE_BUILD ?= $(HOME)/sqlite_build
SDK_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
SDK_INCLUDE := $(SDK_DIR)include
SDK_SRC := $(SDK_DIR)src

# Compiler settings
CXX ?= g++
CXXFLAGS := -std=c++17 -fPIC -O2 -Wall -Wextra
CXXFLAGS += -I$(SDK_INCLUDE) -I$(SQLITE_SRC) -I$(SQLITE_BUILD)
LDFLAGS := -shared -ldl

# Default target
.PHONY: all clean examples tests

all: examples tests

# Plugin build rule
%.so: %.cpp
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $<

# Example plugins
examples: udf_transform.so histogram.so passthrough_test.so pattern_e2e_test.so

udf_transform.so: examples/udf_transform.cpp
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $<

histogram.so: examples/histogram.cpp
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $<

passthrough_test.so: examples/passthrough_test.cpp
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $<

pattern_e2e_test.so: examples/pattern_e2e_test.cpp
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $<

# Test plugins
tests: pattern_unit_test.so

pattern_unit_test.so: tests/pattern_unit_test.cpp
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $<

# UDF for testing
product_filter_udf.so: product_filter_udf.c
	gcc -fPIC -shared -O2 -I$(SQLITE_BUILD) -o $@ $<

examples: product_filter_udf.so

clean:
	rm -f *.so

# Helper to build SQLite with Lyrore support
sqlite:
	mkdir -p $(SQLITE_BUILD)
	cd $(SQLITE_BUILD) && $(SQLITE_SRC)/../configure
	cd $(SQLITE_BUILD) && $(MAKE) -j4 "OPTS=-DSQLITE_ENABLE_LYRORE=1 -DSQLITE_ENABLE_STMT_SCANSTATUS=1" "LDFLAGS=-rdynamic"

.PHONY: sqlite

pattern_match_verify_test.so: tests/pattern_match_verify_test.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -shared -ldl -o $@ $<

# Real pattern match test (actually tests matching!)
real_pattern_match_test.so: tests/real_pattern_match_test.cpp
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $<

tests: pattern_unit_test.so real_pattern_match_test.so
