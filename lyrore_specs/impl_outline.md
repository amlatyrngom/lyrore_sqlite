# Lyrore SQLite Implementation Plan: Rough Outline

## Executive Summary

This implementation plan follows a **bottom-up, infrastructure-first** approach. We start with the foundational model framework and state management that ALL subsequent features depend on, then progressively add optimization layers: plan-level → expression-level → fused operators → columnar storage.

**Key Principles:**
1. Each step is independently E2E testable using the curriculum workload
2. APIs are designed upfront to be stable (later steps don't modify earlier APIs)
3. Foundation first: model interface, feature extraction, state persistence
4. Testing with curriculum workload at SF=0.01 for fast iteration, SF=0.1 for validation

**Total Estimated LOC:** ~20-30k (including tests)

---

## Step 1: Core Model Framework & State Infrastructure
**Estimated LOC:** 2.5k-3.5k (+ 500-800 tests)

### Description
Implement the `LyroreModelOps` pluggable model interface, `LyroreFeatures` extraction, state persistence with dedicated connection, and basic PRAGMA configuration. This is THE foundational infrastructure that every other optimization depends on.

**LyroreFeatures Two-Part Design (Critical):**

1. **Pre-defined feature fields** (static struct members for interpretability):
   - Query structure: `nTables`, `nJoins`, `nPredicates`, `nAggregations`, `queryDepth`, `nProjections`
   - Cost estimates: `estTotalRows`, `estOutputRows`, `estCost`, `avgSelectivity`  
   - Access patterns: `nIndexScans`, `nTableScans`, `hasEquality`, `hasRange`, `hasLike`
   - Historical: `lastExecTime`, `avgExecTime`, `execCount`

2. **Dynamic extension vector** (for additional features):
   - `int nExtended` - count of extended feature values
   - `double *aExtended` - dynamically allocated array for ADDITIONAL FEATURES (NOT weights)
   - Examples: feature embeddings, table-specific statistics, workload-derived features, custom computed features

3. **Helper function**: `lyroreFeaturesToArray()` combines static fields + dynamic `aExtended` into a single `double[]` for model input

This two-part design provides:
- Stable, interpretable features for simple heuristics (Steps 2-4)
- Extensibility for future ML models (custom features, embeddings, etc.)
- Note: Model weights are INTERNAL to models (stored in model state), not in LyroreFeatures

### Expected Changes
- **New Files:**
  - `src/lyrore_model.c/h` - Model interface, registration, lifecycle
  - `src/lyrore_features.c/h` - Feature extraction from plans, loops, expressions
  - `src/lyrore_stats.c/h` - State persistence (column stats, model state, exec history)
- **Modified Files:**
  - `src/sqliteInt.h` - Add `LyroreContext` to `sqlite3` struct, flags, feature types
  - `src/main.c` - Initialize Lyrore context, dedicated state connection
  - `src/pragma.c` - Add `lyrore_enabled`, `lyrore_persist`, `lyrore_reset` pragmas
  - `Makefile.in` / `Makefile` - Add new source files

### Why This Order
This MUST come first because:
- All optimizations use `LyroreModelOps` for decisions
- All optimizations need feature extraction for context
- State persistence is required for learning across queries
- PRAGMAs are needed to enable/disable features during testing

### Success Criteria
1. `PRAGMA lyrore_enabled = ON/OFF` works
2. Model registration API works (`lyroreRegisterModel`)
3. Feature extraction produces valid features from dummy plans
4. State tables created and persisted across connections
5. Dedicated state connection doesn't conflict with user transactions
6. Curriculum workload queries run (passthrough, no optimization yet)

### Dependencies
- None (this is the foundation)

---

## Step 2: Plan-Level Cost Correction Hook
**Estimated LOC:** 1.5k-2k (+ 500 lines of tests)

### Description
Hook into `whereLoopAddBtree()` and related functions to apply ML-ready cost correction using the model framework. Implement the initial linear cost correction model as the default. Add execution time tracking and reward collection.

### Expected Changes
- **New Files:**
  - `src/lyrore_cost.c` - Cost estimation integration, correction logic
- **Modified Files:**
  - `src/where.c` - Hook `whereLoopAddBtree()`, `whereLoopInsert()` for cost adjustment
  - `src/vdbe.c` or `src/vdbeaux.c` - Track actual execution time
  - `src/lyrore_model.c` - Add `LinearCostState` implementation
  - `src/lyrore_features.c` - Add `lyroreExtractLoopFeatures()`
  - `src/pragma.c` - Add `PRAGMA lyrore_cost = ON/OFF`

### Why This Order
- Depends on Step 1 (model framework, features, state)
- Cost correction is simpler than plan selection (single model output vs multi-armed bandit)
- Provides immediate measurable impact on query planning

### Success Criteria
1. `PRAGMA lyrore_cost = ON` activates cost correction
2. Cost corrections logged and observable in EXPLAIN output
3. Linear correction model updates based on actual vs predicted
4. Curriculum workload shows cost estimates changing over repeated runs
5. No regression in query correctness

### Dependencies
- Step 1 (model framework, state persistence)

---

## Step 3: Plan-Level Join Order Selection (RL)
**Estimated LOC:** 2k-2.5k (+ 600 lines of tests)

### Description
Implement Thompson Sampling-based join order selection by saving top-N candidate paths during `wherePathSolver()` and applying RL-based selection. Add reward collection based on execution time.

### Expected Changes
- **New Files:**
  - `src/lyrore_plan.c` - Plan selection integration, candidate management
- **Modified Files:**
  - `src/where.c` - Hook `wherePathSolver()` to save candidates, apply selection
  - `src/sqliteInt.h` - Add `aLyroreCandidates`, `nLyroreCandidates`, `iLyrorePlan` to `WhereInfo`
  - `src/lyrore_model.c` - Add `ThompsonState` implementation
  - `src/lyrore_features.c` - Add `lyroreExtractPlanFeatures()`
  - `src/vdbe.c` - Call `lyroreRecordOutcome()` after execution
  - `src/pragma.c` - Add `PRAGMA lyrore_plan_rl = ON/OFF`

### Why This Order
- Depends on Steps 1-2 (framework, execution time tracking)
- Plan-level optimization before expression-level (coarser granularity first)
- Shared infrastructure for execution time tracking from Step 2

### Success Criteria
1. `PRAGMA lyrore_plan_rl = ON` enables RL-based plan selection
2. Multiple candidate plans saved during planning (visible in debug output)
3. Thompson Sampling selects among candidates
4. Rewards collected and bandit converges over repeated runs
5. Multi-table curriculum queries show plan adaptation
6. No correctness regressions

### Dependencies
- Step 1 (model framework)
- Step 2 (execution time tracking infrastructure)

---

## Step 4: Expression Flavoring Framework
**Estimated LOC:** 2.5k-3k (+ 800 lines of tests)

### Description
Implement compile-time expression flavor selection. Create the variant registration system, statistics-based selector, and expression codegen integration. Focus on safe, compile-time-only selection (no per-tuple branching).

### Expected Changes
- **New Files:**
  - `src/lyrore_expr.c/h` - Flavor registration, selection, code generation hooks
- **Modified Files:**
  - `src/expr.c` - Hook `sqlite3ExprCode()` for flavor selection
  - `src/sqliteInt.h` - Add flavor metadata to `Expr` or related structures
  - `src/lyrore_model.c` - Add `StaticFlavorState` implementation
  - `src/lyrore_features.c` - Add `lyroreExtractExprFeatures()`
  - `src/pragma.c` - Add `PRAGMA lyrore_flavor = ON/OFF`

### Why This Order
- Depends on Step 1 (model framework)
- Independent of Steps 2-3 (can be parallel, but sequentially for simplicity)
- Foundation for fused operators (Step 5)

### Success Criteria
1. `PRAGMA lyrore_flavor = ON` enables flavor selection
2. At least 2 flavors implemented (e.g., branching vs branchless comparison)
3. Flavor selected at compile time based on statistics
4. EXPLAIN shows different VDBE code for different flavors
5. Curriculum queries with selective predicates benefit from correct flavor
6. No correctness regressions

### Dependencies
- Step 1 (model framework, feature extraction)

---

## Step 5: Fused Operators & Micro-Adaptivity
**Estimated LOC:** 3.5k-4.5k (+ 1k lines of tests)

### Description
Implement fused VDBE opcodes with batch-level micro-adaptivity (εw-greedy). Add schema validation, fixed-format ephemeral records, and at least 2-3 fused operator types (e.g., fused filter-aggregate, array join for small tables).

### Expected Changes
- **New Files:**
  - `src/lyrore_opcodes.c` - Fused opcode implementations
  - `src/lyrore_fusion.c` - Fusion pattern detection and codegen
- **Modified Files:**
  - `src/vdbe.c` - Add new OP_Lyrore* opcodes to switch statement
  - `src/opcodes.h` / `mkopcodeh.tcl` - Define new opcode numbers
  - `src/wherecode.c` - Generate fused opcodes instead of standard sequences
  - `src/sqliteInt.h` - Add `LyroreFusedPlan`, `LyroreMicroAdaptState` structs
  - `src/pragma.c` - Add `PRAGMA lyrore_fusion = ON/OFF`

### Why This Order
- Depends on Step 1 (model framework for micro-adaptivity)
- Depends on Step 4 (flavor selection concepts apply to fused variants)
- Complex but high-impact for analytical queries

### Success Criteria
1. `PRAGMA lyrore_fusion = ON` enables fused operators
2. Schema validation prevents stale plan usage (SQLITE_SCHEMA on change)
3. Micro-adaptive selector converges to best flavor within ~10 batches
4. At least 2 fused operators working (e.g., OP_LyroreFusedSumWhere, OP_LyroreArrayBuild)
5. Curriculum aggregation queries show speedup (>10% on selective aggregates)
6. Fixed-format ephemeral records work for numeric columns
7. Error handling consistent with SQLite patterns

### Dependencies
- Step 1 (model framework)
- Step 4 (flavoring concepts)

---

## Step 6: UDF Optimization Framework
**Estimated LOC:** 2k-3k (+ 700 lines of tests)

### Description
Implement UDF strategy classification (INLINE/OUTLINE/HYBRID) and C transpilation framework. Focus on simple computational UDFs first. Integrate with expression handling.

### Expected Changes
- **New Files:**
  - `src/lyrore_udf.c/h` - UDF analysis, transpilation, registration
- **Modified Files:**
  - `src/func.c` - Hook UDF registration for Lyrore analysis
  - `src/expr.c` - Use transpiled variants when available
  - `src/sqliteInt.h` - Add UDF metadata (strategy, transpiled function pointer)

### Why This Order
- Depends on Step 1 (framework)
- Can be parallel with Steps 3-5, but sequentially for simplicity
- Curriculum workload UDFs (P1-P4, S1-S4) are the test cases

### Success Criteria
1. UDF strategy classification working (log strategy decisions)
2. At least 2 curriculum UDFs transpiled to optimized C (e.g., P1, P4)
3. Transpiled UDFs produce correct results
4. Performance improvement on UDF-heavy queries (Q_NEW_01, etc.)
5. Fallback to original UDF on error

### Dependencies
- Step 1 (model framework)
- Step 4 (optional, for expression integration)

---

## Step 7: Columnar Storage Foundation
**Estimated LOC:** 3k-4k (+ 1k lines of tests)

### Description
Implement shadow table schema, `lyrore_vectorize()` API, AFTER trigger sync, and basic columnar scan. This is the most complex feature and comes last.

### Expected Changes
- **New Files:**
  - `src/lyrore_colstore.c/h` - Columnar storage implementation
- **Modified Files:**
  - `src/main.c` - Register columnar SQL functions
  - `src/where.c` - Add columnar scan as WhereLoop option
  - `src/wherecode.c` - Generate OP_LyroreColstoreScan
  - `src/vdbe.c` - Implement OP_LyroreColstoreScan
  - `src/build.c` or `src/trigger.c` - AFTER trigger creation for sync

### Why This Order
- MUST come last (most complex, builds on all previous infrastructure)
- Depends on Step 5 (fused operators for columnar scan efficiency)
- Optional advanced feature (system works without it)

### Success Criteria
1. `SELECT lyrore_vectorize('lineitem', 'l_quantity,l_discount')` works
2. Shadow tables created with correct schema
3. INSERT/UPDATE/DELETE sync via AFTER triggers (test with small table)
4. Schema change detection invalidates columnar data
5. Columnar scan selected for appropriate queries (wide scan, few columns)
6. Curriculum lineitem queries benefit from columnar storage
7. No data corruption (stress test with concurrent modifications)

### Dependencies
- Steps 1-5 (full infrastructure)

---

## Optional Step 8: Integration & Polish
**Estimated LOC:** 1k-1.5k (+ 500 lines of tests)

### Description
Integration testing, performance tuning, documentation, and polish. Cross-feature interactions, edge cases, and comprehensive testing with full curriculum workload.

### Expected Changes
- Test suite expansion
- Documentation
- Performance profiling and tuning
- Edge case handling

### Why This Order
- Final step after all features implemented
- Integration testing requires all components

### Success Criteria
1. All curriculum queries pass correctness tests
2. Performance benchmarks documented
3. No regressions from vanilla SQLite
4. Clean PRAGMA interface documented

### Dependencies
- All previous steps

---

## Summary Table

| Step | Name | Est. LOC | Key Files | Depends On |
|------|------|----------|-----------|------------|
| 1 | Core Model Framework & State | 2.5k-3.5k | lyrore_model.c/h, lyrore_features.c/h, lyrore_stats.c/h | None |
| 2 | Cost Correction Hook | 1.5k-2k | lyrore_cost.c, where.c | Step 1 |
| 3 | Join Order Selection (RL) | 2k-2.5k | lyrore_plan.c, where.c | Steps 1-2 |
| 4 | Expression Flavoring | 2.5k-3k | lyrore_expr.c/h, expr.c | Step 1 |
| 5 | Fused Operators | 3.5k-4.5k | lyrore_opcodes.c, lyrore_fusion.c, vdbe.c | Steps 1, 4 |
| 6 | UDF Optimization | 2k-3k | lyrore_udf.c/h, func.c | Step 1 |
| 7 | Columnar Storage | 3k-4k | lyrore_colstore.c/h, where.c, vdbe.c | Steps 1-5 |
| 8 | Integration & Polish | 1k-1.5k | Tests, docs | All |

**Total: ~18k-24k LOC (implementation) + ~5k-6k LOC (tests)**

---

## Testing Strategy

### Per-Step Testing
1. **Unit tests**: For each new function/module
2. **Integration tests**: With curriculum workload at SF=0.01
3. **Correctness tests**: Compare results with vanilla SQLite
4. **Performance tests**: Measure impact on curriculum queries

### Curriculum Workload Test Cases
- **Q1-Q22**: Standard TPC-H (tests general correctness)
- **Q_NEW_01-Q_NEW_20**: UDF-centric (tests UDF optimization)
- **Selective predicates**: Tests flavoring (Steps 4-5)
- **Multi-table joins**: Tests plan selection (Step 3)
- **Aggregations on large tables**: Tests fused operators (Step 5)
- **Column subset queries**: Tests columnar storage (Step 7)

### Fast Feedback Approach
- SF=0.01 for development iteration (~1M rows in lineitem)
- SF=0.1 for validation before step completion
- Automated test suite runs on each commit
