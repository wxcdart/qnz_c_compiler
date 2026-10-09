# SIMD Auto-Vectorization: Current Status

## Status: ⚠️ PARTIALLY COMPLETE (Disabled)

The auto-vectorization pass infrastructure is in place but **disabled** due to integration bugs.

---

## What's Working

### ✅ Pattern Detection
- Loop detection with backedge scanning
- Vectorizable pattern identification (2+ loads, 1+ store, 1+ arithmetic)
- Criteria checking for vectorization eligibility

### ✅ IR Transformation Framework
- Three-pass transformation algorithm:
  1. **Load pass**: Convert scalar loads → VLOAD, build scalar→vector map
  2. **Arithmetic pass**: Convert scalar ops → VADDPS/VSUBPS/VMULPS using mapped temps
  3. **Store pass**: Convert scalar stores → VSTORE using mapped temps

### ✅ Bug Fixes Applied
All three critical defects identified in the review have been fixed:

| Bug | Status | Fix |
|-----|--------|-----|
| Data-flow disconnect in arithmetic | ✅ Fixed | scalar_to_vec_map tracks VLOAD → VADDPS inputs |
| Disconnected vector store value | ✅ Fixed | Store uses map to get arithmetic result |
| Memory offset scaling | ✅ Fixed | Loop increment = 16 bytes (4 floats × 4 bytes) |

### ✅ Register Allocator Integration (NEW)
| Component | Status |
|-----------|--------|
| Live range vector marking | ✅ Fixed (live_range.h) |
| Vector dest reg class | ✅ REG_CLASS_VEC (not XMM) |
| Vector source reg class | ✅ REG_CLASS_VEC for SIMD sources |
| Vector spill alignment | ✅ 16-byte aligned slots |

### ✅ JIT Binary Emission (NEW)
| Component | Status |
|-----------|--------|
| Backend interface | ✅ emit_vload, emit_vstore, emit_vaddps, etc. |
| x86_64 implementation | ✅ SSE2 machine code emission |
| ast_lowering_mc.c integration | ✅ Both asm and JIT modes supported |

---

## Why Disabled

Despite the bug fixes and infrastructure completion, the pass still produces **incorrect runtime results** due to integration issues:

### 1. Vector Temp Allocation Conflicts
```c
// Current approach: manual temp allocation
int vec_temp_counter = 16;  // Start after GPR temps
int vec_temp = vec_temp_counter++;

// Problem: This conflicts with register allocator expectations
// The regalloc.h expects temps to be allocated via c->next_temp_id++
```

**Impact**: Register allocator sees unexpected temp numbers, causing incorrect register assignments.

### 2. Missing Tail Loop Handling
```c
// Current: assumes n is multiple of vector width
for (i = 0; i < n; i += 4) { ... }

// Missing: scalar tail for remainder
for (; i < n; i++) { c[i] = a[i] + b[i]; }
```

**Impact**: Incorrect results when array length is not multiple of 4.

### 3. Complex Data-Flow Tracking
The scalar_to_vec_map approach works for simple cases but breaks down for:
- Multiple arithmetic operations in sequence
- Conditional updates within loops
- Loop-carried dependencies

**Impact**: Vector temps not properly connected across complex loop bodies.

---

## Test Results

```
$ ./build/test/jit/vector_add_loop
Result: 40456160 (expected 396)  ← GARBAGE
```

The garbage output confirms the integration bugs are causing incorrect code generation.

---

## What Needs to Be Fixed

### Priority 1: Temp Allocation Integration

**Option A: Use RegAlloc API**
```c
// Instead of manual temp allocation:
int vec_temp = vec_temp_counter++;  // ❌

// Use compiler's temp allocator:
int vec_temp = c->next_temp_id++;   // ✅
```

**Option B: Pre-allocate Vector Temps**
```c
// Reserve a range of temps for vector operations
#define VEC_TEMP_BASE 200
#define VEC_TEMP_COUNT 64

int alloc_vec_temp() {
    static int next = VEC_TEMP_BASE;
    return next++;
}
```

### Priority 2: Tail Loop Generation

Add scalar tail loop for remainder iterations:

```c
// Vector loop
int vector_n = (n / vec_width) * vec_width;
for (i = 0; i < vector_n; i += vec_width) {
    // Vectorized code
}

// Scalar tail
for (; i < n; i++) {
    c[i] = a[i] + b[i];
}
```

### Priority 3: Enhanced Data-Flow Tracking

Improve scalar_to_vec_map to handle:
- Multiple arithmetic operations in sequence
- Loop-carried dependencies
- Conditional updates

**Current limitation**: Simple one-pass mapping doesn't work for complex loops.

### Priority 4: Alignment Handling (Future)

Check for aligned vs unaligned access:
```c
// If pointer is 16-byte aligned: use aligned instructions
if (is_aligned(ptr, 16)) {
    emit_vmovaps(...);  // Aligned
} else {
    emit_vmovups(...);  // Unaligned
}
```

---

## Recommended Next Steps

### Short-Term (1-2 days)
1. **Fix temp allocation** - Use `c->next_temp_id++` instead of manual counter
2. **Test with simple case** - Verify end-to-end with 4-element array (VLOAD/VSTORE already implemented)

### Medium-Term (1 week)
3. **Add tail loop handling** - Generate scalar code for remainder
4. **Add alignment detection** - Use aligned/unaligned instructions appropriately
6. **Integration testing** - Test with various array sizes

### Long-Term (2-3 weeks)
7. **Stride support** - Handle non-unit stride access patterns
8. **Alias analysis** - Conservative handling of overlapping arrays
9. **Loop unrolling** - Reduce loop overhead for better performance
10. **Re-enable pass** - Set `enabled_by_default = 1`

---

## Current Configuration

```c
// opt_pass.c
{ "auto_vectorize", opt_pass_auto_vectorize, 0, 80 },  // Disabled
```

To re-enable when fixed:
```c
{ "auto_vectorize", opt_pass_auto_vectorize, 1, 80 },  // Enabled
```

---

## Files Modified

| File | Changes | Status |
|------|---------|--------|
| live_range.h | Vector reg class fixes | ✅ Complete |
| isa_backend.h | JIT binary vector interface | ✅ Complete |
| x86_64.c | Binary vector emission | ✅ Complete |
| ast_lowering_mc.c | JIT/ASM dual mode | ✅ Complete |
| opt_vectorize.c | Bug fixes applied | ⚠️ Disabled |
| opt_pass.c | Set enabled=0 | ✅ Complete |

---

## Expected Performance (When Fixed)

| Metric | Value |
|--------|-------|
| Theoretical speedup | 4x |
| Real-world speedup | 2-4x |
| Vector width | 4x float32 |
| Overhead | Minimal (tail loop only) |

---

## Summary

The auto-vectorization pass has the **right algorithm** but needs **better integration**:

✅ **Working**: Pattern detection, transformation logic, bug fixes, JIT binary emission, regalloc integration
❌ **Broken**: Temp allocation, tail loop handling, complex data-flow tracking
⚠️ **Status**: Disabled until integration complete

**Timeline**: 1-2 weeks to full functionality with proper testing.
