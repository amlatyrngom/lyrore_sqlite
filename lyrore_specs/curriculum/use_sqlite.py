#!/usr/bin/env python3
"""
SQLite Curriculum Workload CLI

A command-line tool for loading data, running queries, and benchmarking
the curriculum workload on SQLite.

Usage:
    python use_sqlite.py load --config db_config.json
    python use_sqlite.py run_queries --config db_config.json -o results.json [--for_bench N]
    python use_sqlite.py run_query --config db_config.json --query_id=Q1 -o result.json
    python use_sqlite.py verify --config db_config.json --output_file=out.json --expected_file=expected.json
"""

import argparse
import csv
import json
import os
import sqlite3
import sys
import time
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

# Get the directory where this script is located
SCRIPT_DIR = Path(__file__).parent.resolve()

# ============================================================================
# Python UDFs (P1-P3)
# ============================================================================

def calc_discount_tier(discount: Optional[float]) -> Optional[int]:
    """P1: Calculate discount tier based on discount value."""
    if discount is None:
        return None
    if discount < 0.02:
        return 0
    elif discount < 0.05:
        return 1
    elif discount < 0.08:
        return 2
    else:
        return 3


def format_phone(phone: Optional[str]) -> Optional[str]:
    """P2: Format phone number as +({area}){local}."""
    if phone is None:
        return None
    parts = phone.split('-')
    if len(parts) >= 2:
        return f"+({parts[0]}){parts[1]}"
    return phone


def compute_hash_bucket(key: Optional[int], name: Optional[str], num_buckets: int) -> Optional[int]:
    """P3: Compute hash bucket for partitioning."""
    if key is None or name is None:
        return None
    h = key
    for c in name:
        h = (h * 31 + ord(c)) % (2**31)
    return h % num_buckets


# ============================================================================
# Aggregate UDF: EMA (Exponential Moving Average)
# ============================================================================

class EmaAggregate:
    """Exponential Moving Average aggregate function."""

    def __init__(self):
        self.value = None
        self.alpha = 0.3  # Default alpha

    def step(self, x, alpha=None):
        if alpha is not None:
            self.alpha = alpha
        if x is None:
            return
        if self.value is None:
            self.value = x
        else:
            self.value = self.alpha * x + (1 - self.alpha) * self.value

    def finalize(self):
        return self.value if self.value is not None else 0.0


# ============================================================================
# Configuration
# ============================================================================

def load_config(config_path: str) -> Dict[str, Any]:
    """Load configuration from JSON file."""
    with open(config_path, 'r') as f:
        return json.load(f)


def get_data_dir(config: Dict[str, Any]) -> Path:
    """Get the data directory from config or default."""
    if 'data_dir' in config:
        return Path(config['data_dir'])
    # Default: look for data relative to script directory
    return SCRIPT_DIR / 'data'


# ============================================================================
# UDF Registration
# ============================================================================

def register_udfs(conn: sqlite3.Connection, config: Dict[str, Any]) -> None:
    """Register all UDFs with the connection."""

    # Enable extension loading
    conn.enable_load_extension(True)

    # Register Python UDFs (P1-P3)
    conn.create_function("calc_discount_tier", 1, calc_discount_tier, deterministic=True)
    conn.create_function("format_phone", 1, format_phone, deterministic=True)
    conn.create_function("compute_hash_bucket", 3, compute_hash_bucket, deterministic=True)

    # Register aggregate UDF (EMA)
    conn.create_aggregate("ema", 2, EmaAggregate)

    # Load C extension for P4, S1-S4
    ext_path = config.get('extension_path')
    if ext_path is None:
        # Default: look in script directory
        ext_path = str(SCRIPT_DIR / 'curriculum_udfs.so')

    if not os.path.exists(ext_path):
        raise FileNotFoundError(f"C extension not found: {ext_path}. "
                                f"Please compile curriculum_udfs.c first.")

    # Load the extension (remove .so suffix for SQLite)
    ext_path_no_suffix = ext_path.rsplit('.so', 1)[0] if ext_path.endswith('.so') else ext_path
    try:
        conn.load_extension(ext_path_no_suffix)
    except sqlite3.OperationalError as e:
        # Try with full path
        conn.execute(f"SELECT load_extension('{ext_path}')")

    # Disable extension loading for security
    conn.enable_load_extension(False)


def create_connection(config: Dict[str, Any]) -> sqlite3.Connection:
    """Create a database connection with all UDFs registered."""
    db_path = config.get('db_path', '/tmp/curriculum_sqlite.db')
    conn = sqlite3.connect(db_path)
    conn.row_factory = sqlite3.Row
    register_udfs(conn, config)
    return conn


# ============================================================================
# Schema Definition
# ============================================================================

SCHEMA_SQL = """
-- Region table
CREATE TABLE IF NOT EXISTS region (
    r_regionkey INTEGER PRIMARY KEY,
    r_name TEXT NOT NULL,
    r_comment TEXT
);

-- Nation table
CREATE TABLE IF NOT EXISTS nation (
    n_nationkey INTEGER PRIMARY KEY,
    n_name TEXT NOT NULL,
    n_regionkey INTEGER NOT NULL,
    n_comment TEXT,
    FOREIGN KEY (n_regionkey) REFERENCES region(r_regionkey)
);

-- Part table (with JSON extension)
CREATE TABLE IF NOT EXISTS part (
    p_partkey INTEGER PRIMARY KEY,
    p_name TEXT NOT NULL,
    p_mfgr TEXT NOT NULL,
    p_brand TEXT NOT NULL,
    p_type TEXT NOT NULL,
    p_size INTEGER NOT NULL,
    p_container TEXT NOT NULL,
    p_retailprice REAL NOT NULL,
    p_comment TEXT,
    p_attributes TEXT  -- JSON column
);

-- Supplier table
CREATE TABLE IF NOT EXISTS supplier (
    s_suppkey INTEGER PRIMARY KEY,
    s_name TEXT NOT NULL,
    s_address TEXT NOT NULL,
    s_nationkey INTEGER NOT NULL,
    s_phone TEXT NOT NULL,
    s_acctbal REAL NOT NULL,
    s_comment TEXT,
    FOREIGN KEY (s_nationkey) REFERENCES nation(n_nationkey)
);

-- Partsupp table
CREATE TABLE IF NOT EXISTS partsupp (
    ps_partkey INTEGER NOT NULL,
    ps_suppkey INTEGER NOT NULL,
    ps_availqty INTEGER NOT NULL,
    ps_supplycost REAL NOT NULL,
    ps_comment TEXT,
    PRIMARY KEY (ps_partkey, ps_suppkey),
    FOREIGN KEY (ps_partkey) REFERENCES part(p_partkey),
    FOREIGN KEY (ps_suppkey) REFERENCES supplier(s_suppkey)
);

-- Customer table (with JSON extension)
CREATE TABLE IF NOT EXISTS customer (
    c_custkey INTEGER PRIMARY KEY,
    c_name TEXT NOT NULL,
    c_address TEXT NOT NULL,
    c_nationkey INTEGER NOT NULL,
    c_phone TEXT NOT NULL,
    c_acctbal REAL NOT NULL,
    c_mktsegment TEXT NOT NULL,
    c_comment TEXT,
    c_metadata TEXT,  -- JSON column
    FOREIGN KEY (c_nationkey) REFERENCES nation(n_nationkey)
);

-- Orders table (with JSON extension)
CREATE TABLE IF NOT EXISTS orders (
    o_orderkey INTEGER PRIMARY KEY,
    o_custkey INTEGER NOT NULL,
    o_orderstatus TEXT NOT NULL,
    o_totalprice REAL NOT NULL,
    o_orderdate TEXT NOT NULL,
    o_orderpriority TEXT NOT NULL,
    o_clerk TEXT NOT NULL,
    o_shippriority INTEGER NOT NULL,
    o_comment TEXT,
    o_tags TEXT,  -- JSON column
    FOREIGN KEY (o_custkey) REFERENCES customer(c_custkey)
);

-- Lineitem table (with JSON extension)
CREATE TABLE IF NOT EXISTS lineitem (
    l_orderkey INTEGER NOT NULL,
    l_partkey INTEGER NOT NULL,
    l_suppkey INTEGER NOT NULL,
    l_linenumber INTEGER NOT NULL,
    l_quantity REAL NOT NULL,
    l_extendedprice REAL NOT NULL,
    l_discount REAL NOT NULL,
    l_tax REAL NOT NULL,
    l_returnflag TEXT NOT NULL,
    l_linestatus TEXT NOT NULL,
    l_shipdate TEXT NOT NULL,
    l_commitdate TEXT NOT NULL,
    l_receiptdate TEXT NOT NULL,
    l_shipinstruct TEXT NOT NULL,
    l_shipmode TEXT NOT NULL,
    l_comment TEXT,
    l_metrics TEXT,  -- JSON column
    PRIMARY KEY (l_orderkey, l_linenumber),
    FOREIGN KEY (l_orderkey) REFERENCES orders(o_orderkey),
    FOREIGN KEY (l_partkey) REFERENCES part(p_partkey),
    FOREIGN KEY (l_suppkey) REFERENCES supplier(s_suppkey)
);

-- Create indexes for performance
CREATE INDEX IF NOT EXISTS idx_nation_regionkey ON nation(n_regionkey);
CREATE INDEX IF NOT EXISTS idx_supplier_nationkey ON supplier(s_nationkey);
CREATE INDEX IF NOT EXISTS idx_customer_nationkey ON customer(c_nationkey);
CREATE INDEX IF NOT EXISTS idx_orders_custkey ON orders(o_custkey);
CREATE INDEX IF NOT EXISTS idx_orders_orderdate ON orders(o_orderdate);
CREATE INDEX IF NOT EXISTS idx_lineitem_orderkey ON lineitem(l_orderkey);
CREATE INDEX IF NOT EXISTS idx_lineitem_partkey ON lineitem(l_partkey);
CREATE INDEX IF NOT EXISTS idx_lineitem_suppkey ON lineitem(l_suppkey);
CREATE INDEX IF NOT EXISTS idx_lineitem_shipdate ON lineitem(l_shipdate);
CREATE INDEX IF NOT EXISTS idx_partsupp_suppkey ON partsupp(ps_suppkey);
"""


# ============================================================================
# Data Loading
# ============================================================================

def load_data(config: Dict[str, Any]) -> None:
    """Load CSV data into SQLite database."""
    db_path = config.get('db_path', '/tmp/curriculum_sqlite.db')
    data_dir = get_data_dir(config)

    print(f"Loading data from {data_dir} into {db_path}")

    # Remove existing database if it exists
    if os.path.exists(db_path):
        os.remove(db_path)
        print(f"Removed existing database: {db_path}")

    # Create connection (without UDFs for loading)
    conn = sqlite3.connect(db_path)
    cursor = conn.cursor()

    # Create schema
    print("Creating schema...")
    cursor.executescript(SCHEMA_SQL)
    conn.commit()

    # Table loading order (respecting foreign keys)
    tables = [
        ('region', ['r_regionkey', 'r_name', 'r_comment']),
        ('nation', ['n_nationkey', 'n_name', 'n_regionkey', 'n_comment']),
        ('part', ['p_partkey', 'p_name', 'p_mfgr', 'p_brand', 'p_type', 'p_size', 
                  'p_container', 'p_retailprice', 'p_comment', 'p_attributes']),
        ('supplier', ['s_suppkey', 's_name', 's_address', 's_nationkey', 's_phone', 
                      's_acctbal', 's_comment']),
        ('partsupp', ['ps_partkey', 'ps_suppkey', 'ps_availqty', 'ps_supplycost', 'ps_comment']),
        ('customer', ['c_custkey', 'c_name', 'c_address', 'c_nationkey', 'c_phone', 
                      'c_acctbal', 'c_mktsegment', 'c_comment', 'c_metadata']),
        ('orders', ['o_orderkey', 'o_custkey', 'o_orderstatus', 'o_totalprice', 'o_orderdate',
                    'o_orderpriority', 'o_clerk', 'o_shippriority', 'o_comment', 'o_tags']),
        ('lineitem', ['l_orderkey', 'l_partkey', 'l_suppkey', 'l_linenumber', 'l_quantity',
                      'l_extendedprice', 'l_discount', 'l_tax', 'l_returnflag', 'l_linestatus',
                      'l_shipdate', 'l_commitdate', 'l_receiptdate', 'l_shipinstruct', 
                      'l_shipmode', 'l_comment', 'l_metrics']),
    ]

    for table_name, columns in tables:
        csv_path = data_dir / f'{table_name}.csv'
        if not csv_path.exists():
            print(f"WARNING: {csv_path} not found, skipping {table_name}")
            continue

        print(f"Loading {table_name}...")

        with open(csv_path, 'r', newline='') as f:
            reader = csv.DictReader(f)

            # Build insert statement
            placeholders = ', '.join(['?' for _ in columns])
            insert_sql = f"INSERT INTO {table_name} ({', '.join(columns)}) VALUES ({placeholders})"

            rows = []
            for row in reader:
                values = []
                for col in columns:
                    val = row.get(col, None)
                    # Handle empty strings as NULL
                    if val == '':
                        val = None
                    values.append(val)
                rows.append(tuple(values))

            cursor.executemany(insert_sql, rows)
            print(f"  Loaded {len(rows):,} rows into {table_name}")

    conn.commit()

    # Verify row counts
    print("\nVerifying row counts:")
    for table_name, _ in tables:
        cursor.execute(f"SELECT COUNT(*) FROM {table_name}")
        count = cursor.fetchone()[0]
        print(f"  {table_name}: {count:,} rows")

    conn.close()
    print(f"\nData loading complete. Database saved to: {db_path}")


# ============================================================================
# Query Definitions
# ============================================================================

# TPCH Queries Q1-Q22 adapted for SQLite dialect
TPCH_QUERIES = {
    # Q1: Pricing Summary Report
    "Q1": """
SELECT
    l_returnflag,
    l_linestatus,
    SUM(l_quantity) AS sum_qty,
    SUM(l_extendedprice) AS sum_base_price,
    SUM(l_extendedprice * (1 - l_discount)) AS sum_disc_price,
    SUM(l_extendedprice * (1 - l_discount) * (1 + l_tax)) AS sum_charge,
    AVG(l_quantity) AS avg_qty,
    AVG(l_extendedprice) AS avg_price,
    AVG(l_discount) AS avg_disc,
    COUNT(*) AS count_order
FROM lineitem
WHERE l_shipdate <= date('1998-12-01', '-90 days')
GROUP BY l_returnflag, l_linestatus
ORDER BY l_returnflag, l_linestatus
""",

    # Q2: Minimum Cost Supplier
    "Q2": """
SELECT
    s_acctbal, s_name, n_name, p_partkey, p_mfgr,
    s_address, s_phone, s_comment
FROM part, supplier, partsupp, nation, region
WHERE p_partkey = ps_partkey
  AND s_suppkey = ps_suppkey
  AND p_size = 15
  AND p_type LIKE '%BRASS'
  AND s_nationkey = n_nationkey
  AND n_regionkey = r_regionkey
  AND r_name = 'EUROPE'
  AND ps_supplycost = (
    SELECT MIN(ps_supplycost)
    FROM partsupp, supplier, nation, region
    WHERE p_partkey = ps_partkey
      AND s_suppkey = ps_suppkey
      AND s_nationkey = n_nationkey
      AND n_regionkey = r_regionkey
      AND r_name = 'EUROPE'
  )
ORDER BY s_acctbal DESC, n_name, s_name, p_partkey
LIMIT 100
""",

    # Q3: Shipping Priority
    "Q3": """
SELECT
    l_orderkey,
    SUM(l_extendedprice * (1 - l_discount)) AS revenue,
    o_orderdate,
    o_shippriority
FROM customer, orders, lineitem
WHERE c_mktsegment = 'BUILDING'
  AND c_custkey = o_custkey
  AND l_orderkey = o_orderkey
  AND o_orderdate < '1995-03-15'
  AND l_shipdate > '1995-03-15'
GROUP BY l_orderkey, o_orderdate, o_shippriority
ORDER BY revenue DESC, o_orderdate
LIMIT 10
""",

    # Q4: Order Priority Checking
    "Q4": """
SELECT o_orderpriority, COUNT(*) AS order_count
FROM orders
WHERE o_orderdate >= '1993-07-01'
  AND o_orderdate < '1993-10-01'
  AND EXISTS (
    SELECT * FROM lineitem
    WHERE l_orderkey = o_orderkey AND l_commitdate < l_receiptdate
  )
GROUP BY o_orderpriority
ORDER BY o_orderpriority
""",

    # Q5: Local Supplier Volume
    "Q5": """
SELECT n_name, SUM(l_extendedprice * (1 - l_discount)) AS revenue
FROM customer, orders, lineitem, supplier, nation, region
WHERE c_custkey = o_custkey
  AND l_orderkey = o_orderkey
  AND l_suppkey = s_suppkey
  AND c_nationkey = s_nationkey
  AND s_nationkey = n_nationkey
  AND n_regionkey = r_regionkey
  AND r_name = 'ASIA'
  AND o_orderdate >= '1994-01-01'
  AND o_orderdate < '1995-01-01'
GROUP BY n_name
ORDER BY revenue DESC
""",

    # Q6: Forecasting Revenue Change
    "Q6": """
SELECT SUM(l_extendedprice * l_discount) AS revenue
FROM lineitem
WHERE l_shipdate >= '1994-01-01'
  AND l_shipdate < '1995-01-01'
  AND l_discount BETWEEN 0.05 AND 0.07
  AND l_quantity < 24
""",

    # Q7: Volume Shipping
    "Q7": """
SELECT
    supp_nation, cust_nation, l_year,
    SUM(volume) AS revenue
FROM (
    SELECT
        n1.n_name AS supp_nation,
        n2.n_name AS cust_nation,
        CAST(strftime('%Y', l_shipdate) AS INTEGER) AS l_year,
        l_extendedprice * (1 - l_discount) AS volume
    FROM supplier, lineitem, orders, customer, nation n1, nation n2
    WHERE s_suppkey = l_suppkey
      AND o_orderkey = l_orderkey
      AND c_custkey = o_custkey
      AND s_nationkey = n1.n_nationkey
      AND c_nationkey = n2.n_nationkey
      AND ((n1.n_name = 'FRANCE' AND n2.n_name = 'GERMANY')
           OR (n1.n_name = 'GERMANY' AND n2.n_name = 'FRANCE'))
      AND l_shipdate BETWEEN '1995-01-01' AND '1996-12-31'
) AS shipping
GROUP BY supp_nation, cust_nation, l_year
ORDER BY supp_nation, cust_nation, l_year
""",

    # Q8: National Market Share
    "Q8": """
SELECT
    o_year,
    SUM(CASE WHEN nation = 'BRAZIL' THEN volume ELSE 0 END) / SUM(volume) AS mkt_share
FROM (
    SELECT
        CAST(strftime('%Y', o_orderdate) AS INTEGER) AS o_year,
        l_extendedprice * (1 - l_discount) AS volume,
        n2.n_name AS nation
    FROM part, supplier, lineitem, orders, customer, nation n1, nation n2, region
    WHERE p_partkey = l_partkey
      AND s_suppkey = l_suppkey
      AND l_orderkey = o_orderkey
      AND o_custkey = c_custkey
      AND c_nationkey = n1.n_nationkey
      AND n1.n_regionkey = r_regionkey
      AND r_name = 'AMERICA'
      AND s_nationkey = n2.n_nationkey
      AND o_orderdate BETWEEN '1995-01-01' AND '1996-12-31'
      AND p_type = 'ECONOMY ANODIZED STEEL'
) AS all_nations
GROUP BY o_year
ORDER BY o_year
""",

    # Q9: Product Type Profit Measure
    "Q9": """
SELECT
    nation, o_year, SUM(amount) AS sum_profit
FROM (
    SELECT
        n_name AS nation,
        CAST(strftime('%Y', o_orderdate) AS INTEGER) AS o_year,
        l_extendedprice * (1 - l_discount) - ps_supplycost * l_quantity AS amount
    FROM part, supplier, lineitem, partsupp, orders, nation
    WHERE s_suppkey = l_suppkey
      AND ps_suppkey = l_suppkey
      AND ps_partkey = l_partkey
      AND p_partkey = l_partkey
      AND o_orderkey = l_orderkey
      AND s_nationkey = n_nationkey
      AND p_name LIKE '%green%'
) AS profit
GROUP BY nation, o_year
ORDER BY nation, o_year DESC
""",

    # Q10: Returned Item Reporting
    "Q10": """
SELECT
    c_custkey, c_name,
    SUM(l_extendedprice * (1 - l_discount)) AS revenue,
    c_acctbal, n_name, c_address, c_phone, c_comment
FROM customer, orders, lineitem, nation
WHERE c_custkey = o_custkey
  AND l_orderkey = o_orderkey
  AND o_orderdate >= '1993-10-01'
  AND o_orderdate < '1994-01-01'
  AND l_returnflag = 'R'
  AND c_nationkey = n_nationkey
GROUP BY c_custkey, c_name, c_acctbal, c_phone, n_name, c_address, c_comment
ORDER BY revenue DESC
LIMIT 20
""",

    # Q11: Important Stock Identification
    "Q11": """
SELECT ps_partkey, SUM(ps_supplycost * ps_availqty) AS value
FROM partsupp, supplier, nation
WHERE ps_suppkey = s_suppkey
  AND s_nationkey = n_nationkey
  AND n_name = 'GERMANY'
GROUP BY ps_partkey
HAVING SUM(ps_supplycost * ps_availqty) > (
    SELECT SUM(ps_supplycost * ps_availqty) * 0.0001
    FROM partsupp, supplier, nation
    WHERE ps_suppkey = s_suppkey
      AND s_nationkey = n_nationkey
      AND n_name = 'GERMANY'
)
ORDER BY value DESC
""",

    # Q12: Shipping Modes and Order Priority
    "Q12": """
SELECT
    l_shipmode,
    SUM(CASE WHEN o_orderpriority = '1-URGENT' OR o_orderpriority = '2-HIGH'
             THEN 1 ELSE 0 END) AS high_line_count,
    SUM(CASE WHEN o_orderpriority <> '1-URGENT' AND o_orderpriority <> '2-HIGH'
             THEN 1 ELSE 0 END) AS low_line_count
FROM orders, lineitem
WHERE o_orderkey = l_orderkey
  AND l_shipmode IN ('MAIL', 'SHIP')
  AND l_commitdate < l_receiptdate
  AND l_shipdate < l_commitdate
  AND l_receiptdate >= '1994-01-01'
  AND l_receiptdate < '1995-01-01'
GROUP BY l_shipmode
ORDER BY l_shipmode
""",

    # Q13: Customer Distribution
    "Q13": """
SELECT c_count, COUNT(*) AS custdist
FROM (
    SELECT c_custkey, COUNT(o_orderkey) AS c_count
    FROM customer LEFT OUTER JOIN orders ON c_custkey = o_custkey
                                         AND o_comment NOT LIKE '%special%requests%'
    GROUP BY c_custkey
) AS c_orders
GROUP BY c_count
ORDER BY custdist DESC, c_count DESC
""",

    # Q14: Promotion Effect
    "Q14": """
SELECT 100.00 * SUM(CASE WHEN p_type LIKE 'PROMO%'
                         THEN l_extendedprice * (1 - l_discount) ELSE 0 END) 
       / SUM(l_extendedprice * (1 - l_discount)) AS promo_revenue
FROM lineitem, part
WHERE l_partkey = p_partkey
  AND l_shipdate >= '1995-09-01'
  AND l_shipdate < '1995-10-01'
""",

    # Q15: Top Supplier
    "Q15": """
WITH revenue AS (
    SELECT l_suppkey AS supplier_no,
           SUM(l_extendedprice * (1 - l_discount)) AS total_revenue
    FROM lineitem
    WHERE l_shipdate >= '1996-01-01'
      AND l_shipdate < '1996-04-01'
    GROUP BY l_suppkey
)
SELECT s_suppkey, s_name, s_address, s_phone, total_revenue
FROM supplier, revenue
WHERE s_suppkey = supplier_no
  AND total_revenue = (SELECT MAX(total_revenue) FROM revenue)
ORDER BY s_suppkey
""",

    # Q16: Parts/Supplier Relationship
    "Q16": """
SELECT p_brand, p_type, p_size, COUNT(DISTINCT ps_suppkey) AS supplier_cnt
FROM partsupp, part
WHERE p_partkey = ps_partkey
  AND p_brand <> 'Brand#45'
  AND p_type NOT LIKE 'MEDIUM POLISHED%'
  AND p_size IN (49, 14, 23, 45, 19, 3, 36, 9)
  AND ps_suppkey NOT IN (
    SELECT s_suppkey FROM supplier WHERE s_comment LIKE '%Customer%Complaints%'
  )
GROUP BY p_brand, p_type, p_size
ORDER BY supplier_cnt DESC, p_brand, p_type, p_size
""",

    # Q17: Small-Quantity-Order Revenue
    "Q17": """
SELECT SUM(l_extendedprice) / 7.0 AS avg_yearly
FROM lineitem, part
WHERE p_partkey = l_partkey
  AND p_brand = 'Brand#23'
  AND p_container = 'MED BOX'
  AND l_quantity < (
    SELECT 0.2 * AVG(l_quantity)
    FROM lineitem
    WHERE l_partkey = p_partkey
  )
""",

    # Q18: Large Volume Customer
    "Q18": """
SELECT c_name, c_custkey, o_orderkey, o_orderdate, o_totalprice, SUM(l_quantity)
FROM customer, orders, lineitem
WHERE o_orderkey IN (
    SELECT l_orderkey FROM lineitem GROUP BY l_orderkey HAVING SUM(l_quantity) > 300
  )
  AND c_custkey = o_custkey
  AND o_orderkey = l_orderkey
GROUP BY c_name, c_custkey, o_orderkey, o_orderdate, o_totalprice
ORDER BY o_totalprice DESC, o_orderdate
LIMIT 100
""",

    # Q19: Discounted Revenue
    "Q19": """
SELECT SUM(l_extendedprice * (1 - l_discount)) AS revenue
FROM lineitem, part
WHERE (
    p_partkey = l_partkey
    AND p_brand = 'Brand#12'
    AND p_container IN ('SM CASE', 'SM BOX', 'SM PACK', 'SM PKG')
    AND l_quantity >= 1 AND l_quantity <= 11
    AND p_size BETWEEN 1 AND 5
    AND l_shipmode IN ('AIR', 'AIR REG')
    AND l_shipinstruct = 'DELIVER IN PERSON'
  )
  OR (
    p_partkey = l_partkey
    AND p_brand = 'Brand#23'
    AND p_container IN ('MED BAG', 'MED BOX', 'MED PKG', 'MED PACK')
    AND l_quantity >= 10 AND l_quantity <= 20
    AND p_size BETWEEN 1 AND 10
    AND l_shipmode IN ('AIR', 'AIR REG')
    AND l_shipinstruct = 'DELIVER IN PERSON'
  )
  OR (
    p_partkey = l_partkey
    AND p_brand = 'Brand#34'
    AND p_container IN ('LG CASE', 'LG BOX', 'LG PACK', 'LG PKG')
    AND l_quantity >= 20 AND l_quantity <= 30
    AND p_size BETWEEN 1 AND 15
    AND l_shipmode IN ('AIR', 'AIR REG')
    AND l_shipinstruct = 'DELIVER IN PERSON'
  )
""",

    # Q20: Potential Part Promotion
    "Q20": """
SELECT s_name, s_address
FROM supplier, nation
WHERE s_suppkey IN (
    SELECT ps_suppkey
    FROM partsupp
    WHERE ps_partkey IN (SELECT p_partkey FROM part WHERE p_name LIKE 'forest%')
      AND ps_availqty > (
        SELECT 0.5 * SUM(l_quantity)
        FROM lineitem
        WHERE l_partkey = ps_partkey
          AND l_suppkey = ps_suppkey
          AND l_shipdate >= '1994-01-01'
          AND l_shipdate < '1995-01-01'
      )
  )
  AND s_nationkey = n_nationkey
  AND n_name = 'CANADA'
ORDER BY s_name
""",

    # Q21: Suppliers Who Kept Orders Waiting
    "Q21": """
SELECT s_name, COUNT(*) AS numwait
FROM supplier, lineitem l1, orders, nation
WHERE s_suppkey = l1.l_suppkey
  AND o_orderkey = l1.l_orderkey
  AND o_orderstatus = 'F'
  AND l1.l_receiptdate > l1.l_commitdate
  AND EXISTS (
    SELECT * FROM lineitem l2
    WHERE l2.l_orderkey = l1.l_orderkey AND l2.l_suppkey <> l1.l_suppkey
  )
  AND NOT EXISTS (
    SELECT * FROM lineitem l3
    WHERE l3.l_orderkey = l1.l_orderkey
      AND l3.l_suppkey <> l1.l_suppkey
      AND l3.l_receiptdate > l3.l_commitdate
  )
  AND s_nationkey = n_nationkey
  AND n_name = 'SAUDI ARABIA'
GROUP BY s_name
ORDER BY numwait DESC, s_name
LIMIT 100
""",

    # Q22: Global Sales Opportunity
    "Q22": """
SELECT cntrycode, COUNT(*) AS numcust, SUM(c_acctbal) AS totacctbal
FROM (
    SELECT SUBSTR(c_phone, 1, 2) AS cntrycode, c_acctbal
    FROM customer
    WHERE SUBSTR(c_phone, 1, 2) IN ('13', '31', '23', '29', '30', '18', '17')
      AND c_acctbal > (
        SELECT AVG(c_acctbal) FROM customer
        WHERE c_acctbal > 0.00
          AND SUBSTR(c_phone, 1, 2) IN ('13', '31', '23', '29', '30', '18', '17')
      )
      AND NOT EXISTS (SELECT * FROM orders WHERE o_custkey = c_custkey)
) AS custsale
GROUP BY cntrycode
ORDER BY cntrycode
""",
}


# New UDF-centric queries adapted for SQLite
# Note: generate_date_range is implemented as a recursive CTE workaround
# New UDF Queries (Q_NEW_01 to Q_NEW_20) - Redesigned for 0.5-2s execution time
# New UDF Queries (Q_NEW_01 to Q_NEW_20) - UDF-centric benchmarking
# Target: 1-4s execution time on SF=0.1, with UDFs as primary bottleneck
NEW_QUERIES = {
    # ============================================================================
    # New UDF Queries (Q_NEW_01 to Q_NEW_20) - Optimized for:
    # - 0.5-2s execution time (process many rows)
    # - ≤10 output rows (small result sets)
    # - UDF called many times (benchmarking UDF overhead)
    # ============================================================================

    # ============ Python UDFs (P1-P4) ============

    # Q_NEW_01: P1 (calc_discount_tier) on all lineitem - GROUP BY 4 tiers
    "Q_NEW_01": """
SELECT calc_discount_tier(l_discount) AS tier, 
       COUNT(*) AS cnt,
       SUM(l_extendedprice) AS total_price,
       AVG(l_quantity) AS avg_qty
FROM lineitem
GROUP BY calc_discount_tier(l_discount)
ORDER BY tier
""",

    # Q_NEW_02: P2 (format_phone) on all lineitem via supplier - TOP 10 by sales
    "Q_NEW_02": """
SELECT * FROM (
    SELECT s.s_suppkey, format_phone(s.s_phone) AS formatted_phone,
           COUNT(*) AS line_count,
           SUM(l.l_extendedprice) AS total_sales
    FROM lineitem l
    JOIN supplier s ON l.l_suppkey = s.s_suppkey
    GROUP BY s.s_suppkey, format_phone(s.s_phone)
) sub
ORDER BY total_sales DESC
LIMIT 10
""",

    # Q_NEW_03: P3 (compute_hash_bucket) on all parts - GROUP BY 10 buckets
    "Q_NEW_03": """
SELECT compute_hash_bucket(p_partkey, p_name, 10) AS bucket,
       COUNT(*) AS cnt,
       SUM(p_retailprice) AS total_price,
       AVG(p_retailprice) AS avg_price
FROM part
GROUP BY compute_hash_bucket(p_partkey, p_name, 10)
ORDER BY bucket
""",

    # Q_NEW_04: P4 (parse_json_score) on all lineitem - GROUP BY 3 quality tiers
    "Q_NEW_04": """
SELECT 
    CASE 
        WHEN parse_json_score(l_metrics, 'quality_score', 50) >= 80 THEN 'high'
        WHEN parse_json_score(l_metrics, 'quality_score', 50) >= 40 THEN 'medium'
        ELSE 'low' 
    END AS quality_tier,
    COUNT(*) AS cnt,
    SUM(l_extendedprice) AS total_price,
    AVG(l_discount) AS avg_discount
FROM lineitem
GROUP BY quality_tier
ORDER BY quality_tier
""",

    # ============ SQL UDFs (S1-S4) ============

    # Q_NEW_05: S1 (get_nation_info) on all orders - TOP 10 nations by value
    "Q_NEW_05": """
SELECT * FROM (
    SELECT get_nation_info(c.c_nationkey, 0) AS customer_nation,
           COUNT(*) AS order_count,
           SUM(o.o_totalprice) AS total_value
    FROM orders o
    JOIN customer c ON o.o_custkey = c.c_custkey
    GROUP BY get_nation_info(c.c_nationkey, 0)
) sub
ORDER BY total_value DESC
LIMIT 10
""",

    # Q_NEW_06: S2 (get_supplier_status) on all lineitem - GROUP BY 3 statuses
    "Q_NEW_06": """
SELECT get_supplier_status(l_suppkey) AS supplier_status,
       COUNT(*) AS line_count,
       SUM(l_extendedprice) AS total_price,
       AVG(l_quantity) AS avg_qty
FROM lineitem
GROUP BY get_supplier_status(l_suppkey)
ORDER BY total_price DESC
""",

    # Q_NEW_07: S4 (get_customer_order_total) on all customers - GROUP BY 5 segments
    "Q_NEW_07": """
SELECT * FROM (
    SELECT c_mktsegment,
           COUNT(*) AS num_customers,
           SUM(get_customer_order_total(c_custkey, 1994)) AS total_1994,
           SUM(get_customer_order_total(c_custkey, 1995)) AS total_1995
    FROM customer
    GROUP BY c_mktsegment
) sub
ORDER BY total_1995 DESC
LIMIT 10
""",

    # Q_NEW_08: S3 (get_supplier_risk_score) on all suppliers - TOP 10 by risk
    "Q_NEW_08": """
SELECT s_suppkey, s_name, get_supplier_risk_score(s_suppkey) AS risk_score,
       s_acctbal
FROM supplier
ORDER BY risk_score DESC
LIMIT 10
""",

    # ============ Combined Python UDFs ============

    # Q_NEW_09: P1 + P4 on filtered lineitem - GROUP BY 2 tiers
    "Q_NEW_09": """
SELECT calc_discount_tier(l_discount) AS tier,
       COUNT(*) AS cnt,
       AVG(parse_json_score(l_metrics, 'quality_score', 0)) AS avg_quality,
       SUM(l_extendedprice) AS total_price
FROM lineitem
WHERE calc_discount_tier(l_discount) >= 2
GROUP BY calc_discount_tier(l_discount)
ORDER BY tier
""",

    # Q_NEW_10: EMA aggregate on all orders - single row output
    "Q_NEW_10": """
SELECT ema(o_totalprice, 0.3) AS price_ema,
       COUNT(*) AS order_count,
       SUM(o_totalprice) AS total_price
FROM (SELECT o_totalprice FROM orders ORDER BY o_orderdate) sub
""",

    # ============ Date/Time Queries ============

    # Q_NEW_11: Orders aggregated by month - TOP 10 months
    "Q_NEW_11": """
SELECT strftime('%Y-%m', o_orderdate) AS month,
       COUNT(*) AS order_count,
       SUM(o_totalprice) AS total_price
FROM orders
WHERE o_orderdate >= '1993-01-01' AND o_orderdate < '1998-01-01'
GROUP BY strftime('%Y-%m', o_orderdate)
ORDER BY total_price DESC
LIMIT 10
""",

    # ============ Multi-UDF Queries ============

    # Q_NEW_12: S1 + S2 on all orders - TOP 10 nation-regions by value
    "Q_NEW_12": """
SELECT * FROM (
    SELECT get_nation_info(c.c_nationkey, 1) AS nation_region,
           COUNT(*) AS order_count,
           SUM(o.o_totalprice) AS total_value
    FROM orders o
    JOIN customer c ON o.o_custkey = c.c_custkey
    GROUP BY get_nation_info(c.c_nationkey, 1)
) sub
ORDER BY total_value DESC
LIMIT 10
""",

    # Q_NEW_13: P1 + P4 on full lineitem - GROUP BY 4 tiers
    "Q_NEW_13": """
SELECT calc_discount_tier(l_discount) AS tier,
       AVG(parse_json_score(l_metrics, 'quality_score', 50)) AS avg_quality,
       COUNT(*) AS cnt,
       SUM(l_extendedprice) AS total_price
FROM lineitem
GROUP BY calc_discount_tier(l_discount)
ORDER BY tier
""",

    # Q_NEW_14: S4 called 4 times per customer - GROUP BY 5 segments
    "Q_NEW_14": """
SELECT c_mktsegment,
       COUNT(*) AS num_customers,
       SUM(get_customer_order_total(c_custkey, 1993)) AS t93,
       SUM(get_customer_order_total(c_custkey, 1994)) AS t94,
       SUM(get_customer_order_total(c_custkey, 1995)) AS t95,
       SUM(get_customer_order_total(c_custkey, 1996)) AS t96
FROM customer
GROUP BY c_mktsegment
ORDER BY t95 DESC
""",

    # Q_NEW_15: P3 on all parts - GROUP BY 10 buckets
    "Q_NEW_15": """
SELECT compute_hash_bucket(p_partkey, p_name, 10) AS bucket, 
       COUNT(*) AS cnt, 
       AVG(p_retailprice) AS avg_price,
       SUM(p_retailprice) AS total_price
FROM part 
GROUP BY compute_hash_bucket(p_partkey, p_name, 10)
ORDER BY bucket
""",

    # Q_NEW_16: S4 per customer aggregated by nation - TOP 10 nations
    "Q_NEW_16": """
SELECT n.n_name AS nation, 
       COUNT(c.c_custkey) AS num_customers,
       SUM(get_customer_order_total(c.c_custkey, 1995)) AS total_1995
FROM nation n 
JOIN customer c ON c.c_nationkey = n.n_nationkey
GROUP BY n.n_name
ORDER BY total_1995 DESC
LIMIT 10
""",

    # Q_NEW_17: S2 + S3 on all suppliers - TOP 10 by risk
    "Q_NEW_17": """
SELECT s_suppkey, s_name,
       get_supplier_status(s_suppkey) AS status,
       get_supplier_risk_score(s_suppkey) AS risk_score
FROM supplier
WHERE get_supplier_status(s_suppkey) IN ('PREMIUM', 'STANDARD')
ORDER BY risk_score DESC
LIMIT 10
""",

    # Q_NEW_18: All Python UDFs (P1, P3, P4) - TOP 10 tier-bucket combos
    "Q_NEW_18": """
SELECT calc_discount_tier(l_discount) AS tier,
       compute_hash_bucket(l_partkey, CAST(l_partkey AS TEXT), 5) AS bucket, 
       COUNT(*) AS cnt,
       AVG(parse_json_score(l_metrics, 'quality_score', 50)) AS avg_quality,
       SUM(l_extendedprice) AS total_price
FROM lineitem 
WHERE parse_json_score(l_metrics, 'quality_score', 0) > 30
GROUP BY calc_discount_tier(l_discount), compute_hash_bucket(l_partkey, CAST(l_partkey AS TEXT), 5)
ORDER BY total_price DESC
LIMIT 10
""",

    # Q_NEW_19: Revenue by year (5 years)
    "Q_NEW_19": """
SELECT strftime('%Y', o_orderdate) AS year,
       COUNT(*) AS order_count,
       SUM(o_totalprice) AS total_price,
       AVG(o_totalprice) AS avg_price
FROM orders
WHERE o_orderdate >= '1993-01-01' AND o_orderdate < '1998-01-01'
GROUP BY strftime('%Y', o_orderdate)
ORDER BY year
""",

    # Q_NEW_20: Mixed Python + SQL UDFs - GROUP BY 3 loyalty tiers
    "Q_NEW_20": """
SELECT 
    CASE 
        WHEN parse_json_score(c_metadata, 'loyalty_score', 0) >= 70 THEN 'high'
        WHEN parse_json_score(c_metadata, 'loyalty_score', 0) >= 30 THEN 'medium'
        ELSE 'low'
    END AS loyalty_tier,
    COUNT(*) AS num_customers,
    SUM(get_customer_order_total(c_custkey, 1995)) AS total_1995
FROM customer
GROUP BY loyalty_tier
ORDER BY total_1995 DESC
""",
}

# Combine all queries
ALL_QUERIES = {**TPCH_QUERIES, **NEW_QUERIES}


# ============================================================================
# Query Execution
# ============================================================================

def run_single_query(conn: sqlite3.Connection, query_id: str, query_sql: str,
                     for_bench: int = 0) -> Dict[str, Any]:
    """
    Run a single query and return results with stats.

    Args:
        conn: Database connection
        query_id: Query identifier
        query_sql: SQL query string
        for_bench: If > 0, run 1 warmup + N benchmark runs

    Returns:
        Dict with 'results', 'stats' keys
    """
    cursor = conn.cursor()

    if for_bench > 0:
        # Benchmark mode: 1 warmup + N runs

        # Warmup run
        start_time = time.perf_counter()
        cursor.execute(query_sql)
        _ = cursor.fetchall()
        warmup_time = time.perf_counter() - start_time

        # Benchmark runs
        run_times = []
        results = None
        for i in range(for_bench):
            start_time = time.perf_counter()
            cursor.execute(query_sql)
            rows = cursor.fetchall()
            run_time = time.perf_counter() - start_time
            run_times.append(run_time)
            if results is None:
                # Convert to list of dicts
                if rows:
                    columns = [desc[0] for desc in cursor.description]
                    results = [dict(zip(columns, row)) for row in rows]
                else:
                    results = []

        avg_time = sum(run_times) / len(run_times)

        return {
            'query_id': query_id,
            'results': results,
            'stats': {
                'warmup_time': warmup_time,
                'avg_time': avg_time,
                'run_times': run_times,
                'num_runs': for_bench
            }
        }
    else:
        # Single run mode
        start_time = time.perf_counter()
        cursor.execute(query_sql)
        rows = cursor.fetchall()
        exec_time = time.perf_counter() - start_time

        # Convert to list of dicts
        if rows:
            columns = [desc[0] for desc in cursor.description]
            results = [dict(zip(columns, row)) for row in rows]
        else:
            results = []

        return {
            'query_id': query_id,
            'results': results,
            'stats': {
                'execution_time': exec_time
            }
        }


def run_all_queries(config: Dict[str, Any], output_file: str, 
                    for_bench: int = 0, query_ids: Optional[List[str]] = None) -> None:
    """Run all queries and save results to output file."""
    conn = create_connection(config)

    queries_to_run = query_ids if query_ids else list(ALL_QUERIES.keys())
    all_results = {}

    total = len(queries_to_run)
    for i, query_id in enumerate(queries_to_run, 1):
        if query_id not in ALL_QUERIES:
            print(f"[{i}/{total}] WARNING: Unknown query {query_id}, skipping")
            continue

        query_sql = ALL_QUERIES[query_id]
        print(f"[{i}/{total}] Running {query_id}...", end=' ', flush=True)

        try:
            result = run_single_query(conn, query_id, query_sql, for_bench)
            all_results[query_id] = result

            if for_bench > 0:
                print(f"OK (warmup: {result['stats']['warmup_time']:.4f}s, "
                      f"avg: {result['stats']['avg_time']:.4f}s)")
            else:
                print(f"OK ({result['stats']['execution_time']:.4f}s, "
                      f"{len(result['results'])} rows)")
        except Exception as e:
            print(f"ERROR: {e}")
            all_results[query_id] = {
                'query_id': query_id,
                'error': str(e),
                'results': None,
                'stats': {}
            }

    conn.close()

    # Save results
    with open(output_file, 'w') as f:
        json.dump(all_results, f, indent=2, default=str)

    print(f"\nResults saved to: {output_file}")


def run_query(config: Dict[str, Any], query_id: Optional[str], raw_query: Optional[str],
              output_file: str, for_bench: int = 0) -> None:
    """Run a single query by ID or raw SQL."""
    conn = create_connection(config)

    if raw_query:
        query_sql = raw_query
        qid = "RAW_QUERY"
    elif query_id:
        if query_id not in ALL_QUERIES:
            raise ValueError(f"Unknown query ID: {query_id}")
        query_sql = ALL_QUERIES[query_id]
        qid = query_id
    else:
        raise ValueError("Must specify either --query_id or --raw_query")

    print(f"Running {qid}...")
    result = run_single_query(conn, qid, query_sql, for_bench)

    conn.close()

    # Save result
    with open(output_file, 'w') as f:
        json.dump(result, f, indent=2, default=str)

    print(f"Result saved to: {output_file}")
    if for_bench > 0:
        print(f"Stats: warmup={result['stats']['warmup_time']:.4f}s, "
              f"avg={result['stats']['avg_time']:.4f}s")
    else:
        print(f"Stats: time={result['stats']['execution_time']:.4f}s, "
              f"rows={len(result['results'])}")


# ============================================================================
# Verification
# ============================================================================

def verify_results(config: Dict[str, Any], output_file: str, expected_file: str) -> None:
    """Verify output results match expected results."""
    with open(output_file, 'r') as f:
        output = json.load(f)

    with open(expected_file, 'r') as f:
        expected = json.load(f)

    mismatches = []

    for query_id in expected:
        if query_id not in output:
            mismatches.append(f"{query_id}: Missing from output")
            continue

        exp_results = expected[query_id].get('results', [])
        out_results = output[query_id].get('results', [])

        if out_results is None:
            mismatches.append(f"{query_id}: Output has error - {output[query_id].get('error', 'unknown')}")
            continue

        if len(exp_results) != len(out_results):
            mismatches.append(f"{query_id}: Row count mismatch (expected {len(exp_results)}, got {len(out_results)})")
            continue

        # Compare row by row (with tolerance for floats)
        for i, (exp_row, out_row) in enumerate(zip(exp_results, out_results)):
            for key in exp_row:
                if key not in out_row:
                    mismatches.append(f"{query_id} row {i}: Missing key '{key}'")
                    break

                exp_val = exp_row[key]
                out_val = out_row[key]

                # Float comparison with tolerance
                if isinstance(exp_val, float) and isinstance(out_val, (int, float)):
                    if abs(exp_val - out_val) > 0.01 * max(abs(exp_val), 1):
                        mismatches.append(f"{query_id} row {i}: Value mismatch for '{key}' "
                                         f"(expected {exp_val}, got {out_val})")
                        break
                elif str(exp_val) != str(out_val):
                    mismatches.append(f"{query_id} row {i}: Value mismatch for '{key}' "
                                     f"(expected {exp_val}, got {out_val})")
                    break

    if mismatches:
        print("VERIFICATION FAILED:")
        for m in mismatches[:20]:  # Show first 20 mismatches
            print(f"  - {m}")
        if len(mismatches) > 20:
            print(f"  ... and {len(mismatches) - 20} more")
        sys.exit(1)
    else:
        print("VERIFICATION PASSED: All results match!")


# ============================================================================
# CLI
# ============================================================================

def main():
    parser = argparse.ArgumentParser(
        description="SQLite Curriculum Workload CLI",
        formatter_class=argparse.RawDescriptionHelpFormatter
    )

    subparsers = parser.add_subparsers(dest='command', help='Available commands')

    # Load command
    load_parser = subparsers.add_parser('load', help='Load data into SQLite database')
    load_parser.add_argument('--config', required=True, help='Path to config JSON file')

    # Run queries command
    run_queries_parser = subparsers.add_parser('run_queries', help='Run all queries')
    run_queries_parser.add_argument('--config', required=True, help='Path to config JSON file')
    run_queries_parser.add_argument('-o', '--output', required=True, help='Output JSON file')
    run_queries_parser.add_argument('--for_bench', type=int, default=0,
                                    help='Benchmark mode: 1 warmup + N runs')
    run_queries_parser.add_argument('--queries', nargs='+', help='Specific query IDs to run')

    # Run query command
    run_query_parser = subparsers.add_parser('run_query', help='Run a single query')
    run_query_parser.add_argument('--config', required=True, help='Path to config JSON file')
    run_query_parser.add_argument('--query_id', help='Query ID (e.g., Q1, Q_NEW_01)')
    run_query_parser.add_argument('--raw_query', help='Raw SQL query string')
    run_query_parser.add_argument('-o', '--output', required=True, help='Output JSON file')
    run_query_parser.add_argument('--for_bench', type=int, default=0,
                                  help='Benchmark mode: 1 warmup + N runs')

    # Verify command
    verify_parser = subparsers.add_parser('verify', help='Verify results match expected')
    verify_parser.add_argument('--config', required=True, help='Path to config JSON file')
    verify_parser.add_argument('--output_file', required=True, help='Output file to verify')
    verify_parser.add_argument('--expected_file', required=True, help='Expected results file')

    args = parser.parse_args()

    if not args.command:
        parser.print_help()
        sys.exit(1)

    # Load config
    config = load_config(args.config)

    if args.command == 'load':
        load_data(config)
    elif args.command == 'run_queries':
        run_all_queries(config, args.output, args.for_bench, args.queries)
    elif args.command == 'run_query':
        run_query(config, args.query_id, args.raw_query, args.output, args.for_bench)
    elif args.command == 'verify':
        verify_results(config, args.output_file, args.expected_file)
    else:
        parser.print_help()
        sys.exit(1)


if __name__ == '__main__':
    main()