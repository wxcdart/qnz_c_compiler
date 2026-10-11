# LICM Trampoline Verification Plan

## Overview

This document outlines the verification strategy for LICM (Loop-Invariant Code Motion) 
trampoline implementation in ZeroCC compiler.

---

## Tools Created

### 1. `tools/licm_verifier.py`

**Purpose**: Automated verification of LICM trampoline correctness

**Features**:
- Parses ZeroCC IR dumps
- Builds basic blocks and control flow graphs
- Detects loops using backedge analysis
- Identifies potential loop invariants
- Verifies hoisting to preheader/trampoline
- Checks jump label integrity (no desynchronization)
- Generates detailed verification reports

**Usage**:
```bash
# Verify LICM transformation
python3 tools/licm_verifier.py \
    --dump-before /tmp/ir_before.txt \
    --dump-after /tmp/ir_after.txt

# Run performance benchmark
python3 tools/licm_verifier.py \
    --source test/bench/licm_invariant_test.c \
    --iterations 100000000
```

**Verification Checks**:
1. **Loop Detection**: Identifies all loops via backedge analysis
2. **Invariant Identification**: Finds instructions that could be hoisted
3. **Hoisting Verification**: Confirms invariants moved to preheader
4. **Jump Integrity**: Ensures no label desynchronization
5. **Functional Correctness**: Compares execution results

---

### 2. `tools/licm_test_suite.py`

**Purpose**: Generate test cases and run verification suite

**Features**:
- Generates test C files with known invariant patterns
- Dumps IR before/after optimization
- Runs verifier on each test case
- Benchmarks performance improvement

**Test Cases**:
1. **simple_invariant**: Constant expression inside loop
   ```c
   int factor = 5;
   for (...) { result += arr[i] * factor; }
   ```

2. **load_invariant**: Value loaded once, used many times
   ```c
   int m = *multiplier;
   for (...) { result += arr[i] * m; }
   ```

3. **complex_invariant**: Multi-step computation
   ```c
   int t1 = base + 10;
   int t2 = t1 * 2;
   int factor = t2 - 5;
   for (...) { result += arr[i] * factor; }
   ```

4. **multiple_invariants**: Several independent invariants
   ```c
   int x = a + b;
   int y = x * c;
   for (...) { result += arr[i] + y; }
   ```

5. **nested_loops**: Invariants in nested loop structure
   ```c
   for (i) {
       int offset = i * cols;  // invariant in outer loop
       for (j) { ... }
   }
   ```

**Usage**:
```bash
# Generate all test cases
python3 tools/licm_test_suite.py --generate

# Verify specific test case
python3 tools/licm_test_suite.py --verify simple_invariant

# Run full verification suite
python3 tools/licm_test_suite.py --all

# Run performance benchmark
python3 tools/licm_test_suite.py --benchmark --iterations 100000000
```

---

## Verification Workflow

### Phase 1: Pre-Implementation Baseline

1. **Generate Test Cases**
   ```bash
   python3 tools/licm_test_suite.py --generate
   ```

2. **Dump Baseline IR** (without LICM)
   ```bash
   ./build/bin/zerocc -O0 --dump-ir -c test/licm/simple_invariant.c -o /tmp/test.o
   ```

3. **Analyze Expected Invariants**
   - Manually identify which instructions should be hoisted
   - Document expected transformation

### Phase 2: Post-Implementation Verification

1. **Dump Optimized IR** (with LICM)
   ```bash
   ./build/bin/zerocc -O1 --dump-ir -c test/licm/simple_invariant.c -o /tmp/test.o
   ```

2. **Run Automated Verification**
   ```bash
   python3 tools/licm_verifier.py \
       --dump-before /tmp/ir_before.txt \
       --dump-after /tmp/ir_after.txt
   ```

3. **Review Verification Report**
   ```
   ============================================================
   LICM TRAMPOLINE VERIFICATION REPORT
   ============================================================

   LOOP ANALYSIS:
     Loops detected: 1
     Loops with hoisting: 1
     Invariants found: 2
     Invariants hoisted: 2

   JUMP INTEGRITY:
     Total jumps: 3
     Valid jumps: 3
     Invalid jumps: 0

   STATUS: ✓ PASS
     Jump integrity verified, no desynchronization detected
   ============================================================
   ```

### Phase 3: Performance Benchmarking

1. **Run Benchmark**
   ```bash
   python3 tools/licm_verifier.py \
       --source test/bench/licm_invariant_test.c \
       --iterations 100000000
   ```

2. **Expected Results**
   - With LICM: O(1) invariant computations
   - Without LICM: O(N) invariant computations
   - Speedup: ~N× for large N

---

## Verification Criteria

### Correctness Checks

| Check | Description | Status |
|-------|-------------|--------|
| **Loop Detection** | All loops correctly identified | Required |
| **Invariant Detection** | All invariants found | Required |
| **Hoisting** | Invariants moved to preheader | Required |
| **Jump Integrity** | No label desynchronization | Required |
| **Functional Correctness** | Same execution result | Required |
| **No Infinite Loops** | Trampoline doesn't create cycles | Required |

### Performance Checks

| Check | Description | Target |
|-------|-------------|--------|
| **Invariant Reduction** | O(N) → O(1) | 100% reduction |
| **Instruction Count** | Fewer instructions in loop body | ≥1 per invariant |
| **Wall-Clock Time** | Speedup on large loops | ≥2× for N=10^8 |

---

## Integration with Existing Tools

### IR Analysis Tools

- **`tools/ir_analyzer.py`**: CFG analysis, data flow tracking
- **`tools/differential_verify.py`**: Functional correctness against GCC
- **`tools/compare_opt_levels.py`**: Compare -O0 vs -O1

### LICM-Specific Tools

- **`tools/licm_verifier.py`**: LICM correctness verification
- **`tools/licm_test_suite.py`**: Test generation and suite runner

### Workflow Integration

```
1. Generate IR dumps
   └── ./build/bin/zerocc --dump-ir -O0/-O1

2. Run LICM verifier
   └── python3 tools/licm_verifier.py --dump-before X --dump-after Y

3. Verify functional correctness
   └── python3 tools/differential_verify.py --source test.c

4. Benchmark performance
   └── python3 tools/licm_verifier.py --benchmark
```

---

## Testing Strategy

### Unit Tests

Each test case verifies a specific pattern:

1. **simple_invariant.c**: Basic constant invariant
2. **load_invariant.c**: Memory load invariant
3. **complex_invariant.c**: Multi-step computation
4. **multiple_invariants.c**: Several invariants
5. **nested_loops.c**: Nested loop structure

### Integration Tests

Full compilation and execution:

1. Compile test program
2. Run with ZeroCC
3. Compare result against GCC
4. Verify correctness

### Performance Tests

Large-scale benchmarking:

1. Compile with -O1 (LICM enabled)
2. Run with 10^8 iterations
3. Measure wall-clock time
4. Compare against -O0 baseline

---

## Success Criteria

### Minimum Requirements

- ✅ All 119 existing tests still pass
- ✅ No jump label desynchronization
- ✅ Functional correctness (matches GCC)
- ✅ Invariants correctly identified and hoisted

### Ideal Outcomes

- ✅ 100% invariant hoisting rate
- ✅ 20-30% performance improvement on loop-heavy code
- ✅ Zero false positives/negatives in verification
- ✅ Automated verification in CI/CD pipeline

---

## Future Enhancements

### Extended Verification

1. **Cross-Block GVN**: Verify value numbering across basic blocks
2. **Partial Redundancy Elimination**: Track PRE alongside LICM
3. **Profile-Guided Optimization**: Use runtime profiles for better hoisting

### Enhanced Benchmarking

1. **Microbenchmarks**: Isolate specific optimization effects
2. **Real-World Workloads**: Test on actual applications
3. **ISA Comparison**: Compare x86-64 vs AArch64 vs RISC-V

---

## Conclusion

The LICM verification toolchain provides:

1. **Automated correctness checking** via `licm_verifier.py`
2. **Comprehensive test coverage** via `licm_test_suite.py`
3. **Performance validation** via benchmarking
4. **Integration with existing tools** for full verification

**Ready to implement LICM trampolines with confidence!** 🎯
