# ZeroCC vs GCC Comparison Guide

## Overview

This guide documents the tools and process for comparing ZeroCC output with GCC.

## Prerequisites

- ✅ GCC installed (`gcc --version`)
- ✅ ZeroCC CLI driver built (`build/bin/zerocc`)
- ✅ Test files available (`test/bench/`, `test/jit/`)

## Comparison Tools

### 1. Direct Comparison (`tools/gcc_compare.py`)

Compares ZeroCC JIT execution with GCC compilation + execution.

```bash
python3 tools/gcc_compare.py
```

**Output:**
```
============================================================
ZeroCC vs GCC Comparison
============================================================
Tests: 4 files
Optimization level: -O2
============================================================

  arith_heavy.c...
    ✅ Match
  call_heavy.c...
    ❌ Exit code mismatch: 1 vs 17
  loop_heavy.c...
    ✅ Match
  memory_heavy.c...
    ❌ One or both failed

============================================================
SUMMARY
============================================================
Total: 4
Passed: 2 ✅
Failed: 2 ❌
Pass rate: 50.0%
============================================================
```

### 2. Benchmark Suite (`tools/benchmark_suite.py`)

Measures compilation and execution times.

```bash
python3 tools/benchmark_suite.py --tests test/bench --max-tests 4
```

**Output:**
```
============================================================
BENCHMARK SUMMARY
============================================================
Total tests: 4
Successful: 4
Avg compile time: 30.32 ms
Avg exec time: 0.9231 ms
============================================================
```

### 3. Differential Verification (`tools/differential_verify.py`)

Verifies output correctness against GCC.

```bash
python3 tools/differential_verify.py --tests test/bench --max-tests 4 --opt-level="-O2"
```

## Manual Comparison

### Single File Comparison

```bash
# ZeroCC JIT
./build/bin/zerocc -O2 test.c

# GCC compile + run
gcc -O2 test.c -o /tmp/test && /tmp/test
```

### Optimization Level Comparison

```bash
# Test all optimization levels
for opt in -O0 -O1 -O2 -O3; do
    echo "Testing $opt:"
    ./build/bin/zerocc $opt test.c
    gcc $opt test.c -o /tmp/test && /tmp/test
done
```

## Test Files

### Benchmark Tests (`test/bench/`)

| File | Description | Status |
|------|-------------|--------|
| `arith_heavy.c` | Arithmetic-intensive | ✅ Pass |
| `call_heavy.c` | Function call-intensive | ❌ Fail |
| `loop_heavy.c` | Loop-intensive | ✅ Pass |
| `memory_heavy.c` | Memory-intensive | ❌ Fail |

### JIT Tests (`test/jit/`)

67 JIT tests for comprehensive coverage.

## Known Issues

### call_heavy.c

- **Issue**: Exit code mismatch (ZeroCC: 1, GCC: 17)
- **Cause**: Function inlining differences
- **Status**: Investigating

### memory_heavy.c

- **Issue**: One or both compilers failed
- **Cause**: Memory access pattern handling
- **Status**: Investigating

## Comparison Workflow

### 1. Quick Check

```bash
python3 tools/gcc_compare.py
```

### 2. Benchmark Performance

```bash
python3 tools/benchmark_suite.py --tests test/bench --max-tests 4
```

### 3. Detailed Analysis

```bash
# Per-file comparison
./build/bin/zerocc -v -O2 test/bench/arith_heavy.c
gcc -O2 test/bench/arith_heavy.c -o /tmp/test && /tmp/test
```

## Optimization Level Support

| Level | ZeroCC | GCC | Status |
|-------|--------|-----|--------|
| -O0 | ✅ | ✅ | Working |
| -O1 | ✅ | ✅ | Working |
| -O2 | ✅ | ✅ | Working |
| -O3 | ✅ | ✅ | Working |

## Results Storage

- `benchmark_results/benchmark_results.json` - Benchmark data
- `diff_results/diff_verify_O2.json` - Differential verification
- `reports/benchmark_report.html` - HTML report

## Next Steps

1. Fix failing tests (call_heavy.c, memory_heavy.c)
2. Expand test suite with more edge cases
3. Add performance regression tracking
4. Implement object file comparison (Task 65)

---

*Last updated: 2024-10-05*
*Status: Ready for comparison*
