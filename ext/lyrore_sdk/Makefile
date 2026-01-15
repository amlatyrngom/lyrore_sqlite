# Lyrore SDK Makefile
# Build C++ plugins for SQLite Lyrore
#
# IMPORTANT: Plugins do NOT include SDK sources (ly_expr.cpp, cpp_context.cpp).
# These are compiled into libsqlite3.so and symbols are resolved at runtime.
# However, plugins DO need access to SQLite headers for type definitions.

# Configuration - adjust paths as needed
SQLITE_SRC ?= $(HOME)/sqlite/src
SQLITE_BUILD ?= $(HOME)/sqlite_build
SDK_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
SDK_INCLUDE := $(SDK_DIR)include
SDK_SRC := $(SDK_DIR)src

# Compiler settings
CXX ?= g++
CXXFLAGS := -std=c++17 -fPIC -O2 -Wall -Wextra
# Include paths: SDK header, SQLite source (for sqliteInt.h), SQLite build (for generated headers)
CXXFLAGS += -I$(SDK_INCLUDE) -I$(SQLITE_SRC) -I$(SQLITE_BUILD)
LDFLAGS := -shared -ldl

# Default target
.PHONY: all clean examples

all: examples

# Build a plugin: make myplugin.so PLUGIN_SRC=myplugin.cpp
# Plugins compile their own code - SDK symbols are resolved from libsqlite3.so
%.so: %.cpp
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $<

# Example plugins - only compile plugin code, not SDK sources
examples: udf_transform.so histogram.so passthrough_test.so

udf_transform.so: examples/udf_transform.cpp
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $<

histogram.so: examples/histogram.cpp
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $<

passthrough_test.so: examples/passthrough_test.cpp
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $<

clean:
	rm -f *.so

# Helper to build SQLite with Lyrore support
sqlite:
	mkdir -p $(SQLITE_BUILD)
	cd $(SQLITE_BUILD) && $(SQLITE_SRC)/../configure
	cd $(SQLITE_BUILD) && $(MAKE) -j4 "OPTS=-DSQLITE_ENABLE_LYRORE=1 -DSQLITE_ENABLE_STMT_SCANSTATUS=1" "LDFLAGS=-rdynamic"

.PHONY: sqlite

# UDF for testing
product_filter_udf.so: product_filter_udf.c
	gcc -fPIC -shared -O2 -I$(SQLITE_BUILD) -o $@ $<

examples: udf_transform.so histogram.so passthrough_test.so product_filter_udf.so
