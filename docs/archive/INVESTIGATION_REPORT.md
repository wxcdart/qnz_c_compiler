# ZeroCC vs GCC Investigation Report

## Overview

This document summarizes the investigation into differences between ZeroCC and GCC output.

## Test Results Summary

### Passing Tests

| Test | Exit Code | Status |
|------|-----------|--------|
| arith_heavy.c | 88 | ✅ All -O0 to -O3 match |
| loop_heavy.c | 86 | ✅ All -O0 to -O3 match |

### Failing Tests

| Test | ZeroCC | GCC | Issue |
|------|--------|-----|-------|
| call_heavy.c | 1 | 17 | Function call/inlining |
| memory_heavy.c | -1 (error) | 80 | Memory allocation |

## Root Cause Analysis

### call_heavy.c

**Symptom**: Exit code mismatch (ZeroCC: 1, GCC: 17)

**GCC Assembly Analysis**:
```
- SIMD vectorization: 124 xmm instructions
- Function inlining: 4 calls → 0 calls (all inlined)
- CET protection: endbr64 detected
```

**Likely Causes**:
1. **No SIMD support**: ZeroCC generates scalar code, GCC uses vectorized code
2. **No inlining**: ZeroCC may not inline `square()` and `cube()` functions
3. **Different loop unrolling**: GCC may unroll loops differently

**Test Code**:
```c
static inline int square(int x) { return x * x; }
static inline int cube(int x) { return x * x * x; }

int main() {
    int sum = 0;
    for (int i = 0; i < 100; i++) {
        sum += square(i);
        sum += cube(i % 10);
    }
    return sum;
}
```

**Expected Result**: `sum = Σ(i²) + Σ((i%10)³)` for i=0 to 99

**Investigation Steps**:
1. Check if ZeroCC handles `static inline` correctly
2. Verify function call semantics
3. Check loop bounds and iteration count

### memory_heavy.c

**Symptom**: ZeroCC fails (-1), GCC succeeds (80)

**GCC Assembly Analysis**:
```
- Stack allocation: 40960 bytes (large array)
- Stack canary: %fs:40 detected
- SIMD vectorization: xmm instructions for array ops
```

**Likely Causes**:
1. **Large stack allocation**: ZeroCC may not handle 40KB stack allocation
2. **VLA handling**: 100x100 array may be treated as VLA
3. **Stack overflow**: ZeroCC may have stack size limits

**Test Code**:
```c
int main() {
    int arr[1000];        // 4KB
    int matrix[100][100]; // 40KB
    // ... array operations
}
```

**Investigation Steps**:
1. Check ZeroCC stack allocation limits
2. Verify VLA handling
3. Test with smaller arrays

## GCC Optimization Differences

### -O0 vs -O2 Impact

| Metric | -O0 | -O2 | Change |
|--------|-----|-----|--------|
| Lines of assembly | 124 | 140 | +16 |
| Function calls | 4 | 0 | -4 (all inlined) |
| SIMD instructions | 0 | 124 | +124 (vectorized) |

### Key GCC Optimizations Not in ZeroCC

1. **SIMD Vectorization**: GCC uses SSE/AVX for loops
2. **Function Inlining**: `static inline` functions are inlined
3. **Loop Unrolling**: GCC unrolls loops for performance
4. **Stack Canaries**: GCC adds stack protection
5. **CET Support**: Control-flow Enforcement Technology

## Recommendations

### Immediate Fixes

1. **call_heavy.c**:
   - Fix `static inline` function handling
   - Verify function call return values
   - Check loop iteration count

2. **memory_heavy.c**:
   - Increase stack allocation limits
   - Fix VLA handling
   - Test with smaller arrays first

### Long-term Improvements

1. **Add SIMD Support**: Implement vectorization
2. **Add Inlining**: Implement function inlining
3. **Add Stack Protection**: Optional security feature
4. **Better Error Messages**: Report stack allocation failures

## Investigation Tools

### Available Tools

| Tool | Purpose | Usage |
|------|---------|-------|
| `tools/gcc_compare.py` | Direct comparison | `python3 tools/gcc_compare.py` |
| `tools/investigate_diff.py` | Deep analysis | `python3 tools/investigate_diff.py <file>` |
| `tools/asm_diff.py` | Assembly comparison | `python3 tools/asm_diff.py <file>` |
| `tools/compare_opt_levels.py` | Optimization comparison | `python3 tools/compare_opt_levels.py <file>` |

### Quick Diagnostics

```bash
# Check all optimization levels
python3 tools/compare_opt_levels.py test/bench/call_heavy.c

# Analyze assembly
python3 tools/asm_diff.py test/bench/call_heavy.c

# Full investigation
python3 tools/investigate_diff.py test/bench/memory_heavy.c
```

## Next Steps

1. **Priority 1**: Fix call_heavy.c (inline function handling)
2. **Priority 2**: Fix memory_heavy.c (stack allocation)
3. **Priority 3**: Add SIMD support for better performance
4. **Priority 4**: Add function inlining

## Conclusion

The differences between ZeroCC and GCC are primarily due to:
1. **Missing SIMD vectorization** - Performance difference, not correctness
2. **Missing function inlining** - May affect correctness for inline functions
3. **Stack allocation limits** - May cause failures for large arrays

Focus on fixing correctness issues first (inline functions, stack allocation), then optimize performance.

---

*Last updated: 2024-10-05*
*Status: Investigation in progress*
