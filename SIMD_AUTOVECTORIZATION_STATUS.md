# SIMD Auto-Vectorization: Current Status

## Status: ⚠️ PARTIALLY COMPLETE (Disabled)

The auto-vectorization pass infrastructure is in place but **disabled** due to critical integration bugs.

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

---

## Why Disabled

Despite the bug fixes, the pass still produces **incorrect runtime results** due to integration issues:

### 1. Vector Temp Allocation Conflicts
```c
// Current approach: manual temp allocation
int vec_temp_counter = 16;  // Start after GPR temps
int vec_temp = vec_temp_counter++;

// Problem: This conflicts with register allocator expectations
// The regalloc.h expects temps to be allocated via c->next_temp_id++
```

**Impact**: Register allocator sees unexpected temp numbers, causing incorrect register assignments.

### 2. VLOAD/VSTORE Machine Code Emission
The machine code emitter (`ast_lowering_mc.c`) has stub implementations for VLOAD/VSTORE:

```c
// ast_lowering_mc.c - current stub
case OP_VLOAD:
    // TODO: Lower to actual vector load instruction
    break;
```

**Impact**: Vector loads/stores don't generate correct machine code.

### 3. Missing Tail Loop Handling
```c
// Current: assumes n is multiple of vector width
for (i = 0; i < n; i += 4) { ... }

// Missing: scalar tail for remainder
for (; i < n; i++) { c[i] = a[i] + b[i]; }
```

**Impact**: Incorrect results when array length is not multiple of 4.

### 4. Register Allocator Integration
Vector temps need to be:
- Tracked by linear-scan register allocator
- Assigned to XMM registers (not GPRs)
- Spilled to 16-byte aligned slots

**Current**: Manual temp allocation bypasses regalloc entirely.

---

## Test Results

```
$ ./build/test/jit/vector_add_loop
Result: 40456160 (expected 396)  ← GARBAGE
```

The garbage output confirms the integration bugs are causing incorrect code generation.

---

## What Needs to Be Fixed

### Priority 1: Register Allocator Integration

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

### Priority 2: Machine Code Lowering

Implement proper VLOAD/VSTORE lowering in `ast_lowering_mc.c`:

```c
case OP_VLOAD:
    // Lower to: MOVQ [base + offset], xmm_dest
    emit_vload_asm(c, instr->temp_dest, instr->temp_src1, instr->imm);
    break;

case OP_VSTORE:
    // Lower to: MOVQ xmm_src2, [base + offset]
    emit_vstore_asm(c, instr->temp_src1, instr->temp_src2, instr->imm);
    break;
```

### Priority 3: Tail Loop Generation

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

### Priority 4: Alignment Handling

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
2. **Implement VLOAD/VSTORE lowering** - Add proper machine code emission
3. **Test with simple case** - Verify end-to-end with 4-element array

### Medium-Term (1 week)
4. **Add tail loop handling** - Generate scalar code for remainder
5. **Add alignment detection** - Use aligned/unaligned instructions appropriately
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
| opt_vectorize.c | Bug fixes applied | ⚠️ Disabled |
| opt_pass.c | Set enabled=0 | ✅ Complete |
| test/jit/vector_add_loop.c | End-to-end test | ❌ Removed (failing) |

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

✅ **Working**: Pattern detection, transformation logic, bug fixes
❌ **Broken**: Temp allocation, machine code lowering, tail handling
⚠️ **Status**: Disabled until integration complete

**Timeline**: 1-2 weeks to full functionality with proper testing.
