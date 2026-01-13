# SQLite Curriculum Workload Specialization Design Document

## Overview

This document describes the SQLite-specific implementation of the curriculum workload,
including UDF implementations, dialect adaptations, and limitations.

## UDF Implementation Strategy

### Language Selection

Following the language selection rules:
- **Python-only UDFs (P1-P3)**: Implemented in Python via `sqlite3.create_function()`
- **SQL/C UDFs (P4, S1-S4)**: Implemented in C as SQLite extensions
- **Aggregate UDF (ema)**: Implemented in Python via `sqlite3.create_aggregate()`
- **Table-Valued Function (generate_date_range)**: Implemented as recursive CTE workaround

### UDF Summary

| UDF | Type | Implementation | Notes |
|-----|------|----------------|-------|
| P1: calc_discount_tier | Scalar | Python | Simple conditionals |
| P2: format_phone | Scalar | Python | String manipulation |
| P3: compute_hash_bucket | Scalar | Python | Hash computation loop |
| P4: parse_json_score | Scalar | C Extension | JSON parsing |
| S1: get_nation_info | Scalar | C Extension | Executes SQL internally |
| S2: get_supplier_status | Scalar | C Extension | Executes SQL internally |
| S3: get_supplier_risk_score | Scalar | C Extension | Executes SQL internally, uses log() |
| S4: get_customer_order_total | Scalar | C Extension | Executes SQL internally |
| ema | Aggregate | Python | Exponential moving average |
| generate_date_range | TVF | Recursive CTE | SQLite lacks native TVF support |

### C Extension Details

The C extension (`curriculum_udfs.c`) implements UDFs P4 and S1-S4:

1. **parse_json_score**: Simple JSON parser that extracts integer values
2. **get_nation_info**: Uses `sqlite3_context_db_handle()` to execute queries
3. **get_supplier_status**: Queries supplier table, returns status string
4. **get_supplier_risk_score**: Aggregates partsupp data, computes risk formula
5. **get_customer_order_total**: Aggregates orders by customer and year

Compilation:
```bash
gcc -shared -fPIC -o curriculum_udfs.so curriculum_udfs.c -I/usr/include -lm -lsqlite3
```

## SQLite Dialect Adaptations

### Date/Time Functions

| Original | SQLite |
|----------|--------|
| `DATE '1998-12-01'` | `'1998-12-01'` |
| `DATE '...' - INTERVAL '90' DAY` | `date('...', '-90 days')` |
| `EXTRACT(YEAR FROM col)` | `CAST(strftime('%Y', col) AS INTEGER)` |

### String Functions

| Original | SQLite |
|----------|--------|
| `SUBSTRING(col FROM 1 FOR 2)` | `SUBSTR(col, 1, 2)` |

### Type Casting

| Original | SQLite |
|----------|--------|
| `CAST(x AS VARCHAR)` | `CAST(x AS TEXT)` |

### Boolean Parameters

SQLite UDFs receive integers for boolean parameters:
- `get_nation_info(nkey, 0)` instead of `get_nation_info(nkey, FALSE)`
- `get_nation_info(nkey, 1)` instead of `get_nation_info(nkey, TRUE)`

## Table-Valued Function Workaround

SQLite requires C extensions for TVFs. As a workaround, `generate_date_range` is
implemented using recursive CTEs:

```sql
-- Original (not supported in SQLite):
SELECT * FROM generate_date_range('1995-01-01', '1995-01-10', 1)

-- SQLite workaround:
WITH RECURSIVE date_range(date_val) AS (
    SELECT '1995-01-01'
    UNION ALL
    SELECT date(date_val, '+1 day')
    FROM date_range
    WHERE date_val < '1995-01-10'
)
SELECT * FROM date_range
```

## Performance Considerations

### C UDFs with Internal SQL

The S1-S4 UDFs execute SQL queries internally, which has implications:
- Each UDF call executes one or more SQL statements
- For queries calling UDFs per-row, this can be slow
- The optimizer cannot inline these UDFs

This is intentional for testing UDF optimization strategies (Prism-style inlining).

### Python UDF Overhead

Python UDFs have call overhead compared to C. For performance-critical paths:
- P1-P3 could be ported to C
- The EMA aggregate could use a C implementation

### Index Usage

The schema includes indexes on common join/filter columns:
- Foreign keys (nation, customer, orders, lineitem)
- Date columns (o_orderdate, l_shipdate)
- Partsupp supplier key

## Limitations

1. **No native TVF support**: Using recursive CTE workaround
2. **JSON functions**: SQLite's JSON1 extension works differently than PostgreSQL's
3. **UDF determinism**: C UDFs calling SQL cannot be marked DETERMINISTIC
4. **Extension loading**: Requires `enable_load_extension(True)`

## Testing Verification

Results should be verified against DuckDB for TPCH Q1-Q22 to ensure correctness
of dialect adaptations. The UDF queries (Q_NEW_*) are SQLite-specific.
