# Task 59: Optimization Benchmarking & Differential Verification Checklist

## Overview
**Task 59**: Benchmark compilation speed and execution throughput against GCC across optimization levels (-O0 to -O3)
**Timeline**: ~3 days
**Status**: 📅 Ready to start

## Pre-Work Complete ✅

### Infrastructure Verification
- [x] deps_graph.json updated (103 files, 1479 functions)
- [x] No circular dependencies found
- [x] All optimization passes implemented (constant fold, DCE, CSE, LICM, strength reduction, branch simplify)
- [x] IR infrastructure complete (89 IR functions)
- [x] Test suite: 118 tests passing

### Tools Created
- [x] `tools/task59_precheck.py` - Readiness check
- [x] `tools/task59_benchmark.py` - Benchmarking framework (GCC only)
- [x] `tools/check_circular_deps.py` - Circular dependency checker

## Implementation Phases

### Phase 1: Benchmark Infrastructure (0.5 days)

#### Files to Create
- [ ] `tools/benchmark_suite.py` - Comprehensive benchmark runner
- [ ] `test/bench/` - Benchmark test cases
  - `test/bench/loop_heavy.c` - Loop-heavy code
  - `test/bench/arith_heavy.c` - Arithmetic-heavy code
  - `test/bench/memory_heavy.c` - Memory-intensive code
  - `test/bench/call_heavy.c` - Function call intensive

#### Key Features
- [ ] Compilation time measurement
- [ ] Execution time measurement
- [ ] Memory usage tracking
- [ ] Multiple optimization levels (-O0 to -O3)

### Phase 2: Differential Verification against GCC (1 day)

#### Implementation
- [ ] Create test harness that runs same code through:
  - ZeroCC (all optimization levels)
  - GCC (-O0, -O1, -O2, -O3)
- [ ] Compare outputs byte-for-byte
- [ ] Report discrepancies
- [ ] Track false positives (known differences)

#### Test Cases
- [ ] Integer arithmetic
- [ ] Floating-point arithmetic
- [ ] Control flow (loops, conditionals)
- [ ] Function calls
- [ ] Memory operations
- [ ] Struct operations
- [ ] Pointer arithmetic

### Phase 3: Performance Analysis (1 day)

#### Metrics to Collect
- [ ] Compilation time (ms)
- [ ] Execution time (ns per operation)
- [ ] Code size (bytes)
- [ ] Stack usage
- [ ] Register pressure

#### Analysis Tools
- [ ] `tools/analyze_benchmarks.py` - Parse and analyze results
- [ ] `tools/generate_report.py` - Generate HTML/PDF reports
- [ ] `tools/compare_opt_levels.py` - Compare optimization levels

#### Visualizations
- [ ] Bar charts: Compilation time by optimization level
- [ ] Line graphs: Execution time vs. input size
- [ ] Heat maps: Optimization effectiveness

### Phase 4: Optimization Recommendations (0.5 days)

#### Based on Results
- [ ] Identify bottlenecks in current implementation
- [ ] Recommend optimization targets
- [ ] Suggest algorithmic improvements
- [ ] Document trade-offs

#### Documentation
- [ ] `docs/BENCHMARK_RESULTS.md` - Full benchmark results
- [ ] `docs/OPTIMIZATION_GUIDE.md` - How to optimize ZeroCC
- [ ] `docs/PERFORMANCE_NOTES.md` - Known performance characteristics

## Success Criteria

- [ ] Benchmark suite runs on all 118 tests
- [ ] Differential verification passes for 95%+ of tests
- [ ] Compilation speed documented for all optimization levels
- [ ] Execution throughput documented for all optimization levels
- [ ] Performance report generated
- [ ] Optimization recommendations documented

## Dependencies

### Required from Previous Tasks
- [x] Task 56: Pass 2 machine code emission
- [x] Task 57: Optimization passes (constant fold, DCE, CSE)
- [x] Task 58: Loop optimizations (LICM, strength reduction)

### External Dependencies
- [x] GCC installed (for differential verification)
- [ ] perf/valgrind (optional, for detailed profiling)

## Risks & Mitigations

| Risk | Impact | Mitigation |
|------|--------|------------|
| Benchmark overhead skews results | Medium | Use many iterations, average results |
| GCC produces different but correct code | High | Verify semantics, not just bytes |
| Performance regressions from optimizations | Medium | Compare -O0 baseline |
| Insufficient test coverage | Medium | Expand test suite |

## Benchmark Templates

### Compilation Speed Test
```python
def benchmark_compilation(source_file, opt_level):
    start = time.perf_counter()
    result = run_zerocc(source_file, opt_level)
    end = time.perf_counter()
    return (end - start) * 1000  # ms
```

### Execution Speed Test
```python
def benchmark_execution(compiled_test, input_data, iterations=1000):
    times = []
    for _ in range(iterations):
        start = time.perf_counter()
        result = run_jit(compiled_test, input_data)
        end = time.perf_counter()
        times.append(end - start)
    return {
        'avg': sum(times) / len(times),
        'min': min(times),
        'max': max(times),
    }
```

### Differential Verification against GCC
```python
def differential_verify(source_file, opt_level):
    zerocc_result = run_zerocc(source_file, opt_level)
    gcc_result = run_gcc(source_file, opt_level)
    
    # Compare outputs
    if zerocc_result.output == gcc_result.output:
        return True, None
    else:
        return False, {
            'zerocc': zerocc_result.output,
            'gcc': gcc_result.output,
        }
```

## Notes

- Benchmark suite should be idempotent (safe to run multiple times)
- Results should be reproducible (document system specs)
- Consider warm-up runs to avoid cold-start effects
- Use statistical analysis for meaningful comparisons

---

*Last updated: 2024-10-04*
*Ready to start: Yes*
