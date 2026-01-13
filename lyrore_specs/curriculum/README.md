# SQLite Curriculum Workload

SQLite implementation of the TPCH-extended curriculum workload for testing workload-specific DBMS optimizations.

## Quick Start

```bash
# 1. Generate data (SF=0.1)
cd /path/to/agnostic/curriculum
python generate_workload_data.py --sf 0.1 --output_dir /path/to/this/curriculum/data

# 2. Compile C UDFs
cd /path/to/this/curriculum
gcc -shared -fPIC -o curriculum_udfs.so curriculum_udfs.c -I/usr/include -lm -lsqlite3

# 3. Load data into SQLite
python use_sqlite.py load --config default_db_config.json

# 4. Run all queries
python use_sqlite.py run_queries --config default_db_config.json -o results.json

# 5. Run with benchmarking (3 runs)
python use_sqlite.py run_queries --config default_db_config.json -o results.json --for_bench 3
```

## Files

| File | Description |
|------|-------------|
| `use_sqlite.py` | Main CLI script for data loading and query execution |
| `use_custom_sqlite.py` | Wrapper for using custom SQLite binaries |
| `curriculum_udfs.c` | C source for UDFs P4, S1-S4 |
| `curriculum_udfs.so` | Compiled C extension (generated) |
| `default_db_config.json` | Default configuration file |
| `specialization_design_doc.md` | SQLite-specific design decisions |
| `baseline_unsupported.json` | List of unsupported features |

## Configuration

Edit `default_db_config.json` or create your own:

```json
{
  "db_path": "/tmp/curriculum_sqlite.db",
  "data_dir": "./data",
  "scale_factor": 0.1,
  "extension_path": "./curriculum_udfs.so",
  "custom_lib_path": null
}
```

| Field | Description |
|-------|-------------|
| `db_path` | Path to SQLite database file |
| `data_dir` | Directory containing CSV data files |
| `scale_factor` | TPC-H scale factor (for reference) |
| `extension_path` | Path to compiled C extension |
| `custom_lib_path` | Path to custom libsqlite3.so (optional) |

## CLI Commands

### Load Data

```bash
python use_sqlite.py load --config db_config.json
```

Loads CSV files from `data_dir` into the SQLite database.

### Run All Queries

```bash
# Single run
python use_sqlite.py run_queries --config db_config.json -o results.json

# Benchmark mode (1 warmup + N runs)
python use_sqlite.py run_queries --config db_config.json -o results.json --for_bench 3

# Run specific queries only
python use_sqlite.py run_queries --config db_config.json -o results.json --queries Q1 Q2 Q_NEW_01
```

### Run Single Query

```bash
# By query ID
python use_sqlite.py run_query --config db_config.json --query_id Q1 -o result.json

# Raw SQL
python use_sqlite.py run_query --config db_config.json --raw_query "SELECT COUNT(*) FROM lineitem" -o result.json
```

### Verify Results

```bash
python use_sqlite.py verify --config db_config.json --output_file results.json --expected_file expected.json
```

## Using Custom SQLite

To test with a custom-compiled SQLite (e.g., with Lyrore patches):

### 1. Compile Custom SQLite

```bash
cd ~/sqlite
./configure --prefix=/tmp/custom_sqlite
make
make install
```

### 2. Recompile C Extension (Optional)

If using different SQLite headers:
```bash
gcc -shared -fPIC -o curriculum_udfs_custom.so curriculum_udfs.c \
    -I/tmp/custom_sqlite/include -L/tmp/custom_sqlite/lib -lm -lsqlite3
```

### 3. Run with Custom Library

```bash
python use_custom_sqlite.py \
    --custom_lib /tmp/custom_sqlite/lib/libsqlite3.so \
    --custom_extension ./curriculum_udfs_custom.so \
    load --config db_config.json
```

## UDFs

### Python UDFs (P1-P3)

| UDF | Signature | Description |
|-----|-----------|-------------|
| `calc_discount_tier` | `(discount REAL) -> INT` | Returns tier 0-3 based on discount |
| `format_phone` | `(phone TEXT) -> TEXT` | Formats phone as `+({area}){local}` |
| `compute_hash_bucket` | `(key INT, name TEXT, buckets INT) -> INT` | Hash-based bucket assignment |

### C UDFs (P4, S1-S4)

| UDF | Signature | Description |
|-----|-----------|-------------|
| `parse_json_score` | `(json TEXT, field TEXT, default INT) -> INT` | Extract integer from JSON |
| `get_nation_info` | `(nkey INT, include_region INT) -> TEXT` | Get nation name (optionally with region) |
| `get_supplier_status` | `(skey INT) -> TEXT` | Returns PREMIUM/STANDARD/PROBATION |
| `get_supplier_risk_score` | `(skey INT) -> REAL` | Compute supplier risk score |
| `get_customer_order_total` | `(ckey INT, year INT) -> REAL` | Total orders for customer in year |

### Aggregate UDF

| UDF | Signature | Description |
|-----|-----------|-------------|
| `ema` | `(value REAL, alpha REAL) -> REAL` | Exponential moving average |

## Queries

- **Q1-Q22**: Standard TPC-H queries (adapted for SQLite dialect)
- **Q_NEW_01-Q_NEW_20**: UDF-centric queries for optimization testing

## Dependencies

- Python 3.8+
- GCC (for compiling C extension)
- libsqlite3-dev (for headers)

Python packages (standard library only):
- sqlite3
- json
- csv
- argparse

## Troubleshooting

### Extension Loading Failed

Ensure Python's sqlite3 was compiled with extension support:
```python
import sqlite3
conn = sqlite3.connect(':memory:')
conn.enable_load_extension(True)  # Should not raise error
```

### UDF Not Found

Make sure the extension is compiled and path is correct:
```bash
ls -la curriculum_udfs.so
file curriculum_udfs.so  # Should show "shared object"
```

### Different Results from DuckDB

Some floating-point differences are expected due to:
- Different aggregation order
- Different floating-point precision
- Date function implementation differences
