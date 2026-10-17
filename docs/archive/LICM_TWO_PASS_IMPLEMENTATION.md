# LICM Two-Pass Rebuild - Implementation Plan

## Corrected Assessment

**Timeline**: 1-2 days (not 1-2 weeks)  
**Files Modified**: 1 file (opt_loop.c)  
**Risk**: Low (isolated to LICM pass)  
**Impact**: Enables real LICM optimization

---

## Why It's Simpler Than I Thought

### No IRBuilder API Needed

**My Mistake**: I assumed we need a full IRBuilder API

**Reality**: We can use existing `ir_emit()` to build new IR, then replace buffer

### No Backend Changes Needed

**My Mistake**: I thought we need to update register allocator, backends, etc.

**Reality**: LICM only modifies IR before register allocation. Backends see fresh IR.

### No Pass Updates Needed

**My Mistake**: I thought all passes need to use builder API

**Reality**: Only LICM needs Two-Pass. Other passes see final IR.

---

## Implementation Approach

### Current IR Structure

```c
// Global IR buffer (in ir.c)
static Instr *g_ir_buffer;
static size_t g_ir_count;
static size_t g_ir_capacity;

// Access functions
Instr *ir_get(size_t idx);
void ir_emit(Instr instr);
size_t ir_get_count(void);
void ir_reset(void);
```

### Two-Pass Algorithm

```c
void opt_licm(ZeroCCCompiler *c) {
    (void)c;
    
    /* ===== PASS 1: Analysis ===== */
    // Detect loops, find invariants (existing code)
    LoopAnalysisState state;
    loop_analysis_init(&state);
    detect_loops(&state);
    
    // Mark invariants
    bool *should_hoist = calloc(ir_get_count(), sizeof(bool));
    for (int l = 0; l < state.loop_count; l++) {
        LoopInfo *loop = &state.loops[l];
        // ... compute invariants ...
        for (int i = 0; i < loop->body_count; i++) {
            int idx = loop->body_indices[i];
            if (is_invariant(idx) && !has_side_effects(idx)) {
                should_hoist[idx] = true;
                record_hoisted_instruction(loop, idx);
            }
        }
    }
    
    /* ===== PASS 2: Rebuild IR ===== */
    // Save old IR
    Instr *old_buffer = g_ir_buffer;
    size_t old_count = g_ir_count;
    size_t old_capacity = g_ir_capacity;
    
    // Reset IR (frees old buffer, creates new empty buffer)
    ir_reset();
    
    // Build new IR with hoisted code
    size_t old_idx = 0;
    for (int l = 0; l < state.loop_count; l++) {
        LoopInfo *loop = &state.loops[l];
        
        // Emit instructions up to loop header
        while (old_idx < loop->header_idx) {
            ir_emit(old_buffer[old_idx++]);
        }
        
        // Emit hoisted code BEFORE loop header
        for (int h = 0; h < loop->hoisted_count; h++) {
            Instr hoisted = old_buffer[loop->hoisted_indices[h]];
            ir_emit(hoisted);  // Emit copy
        }
        
        // Emit loop body (NOP out hoisted instructions)
        while (old_idx <= loop->exit_idx) {
            if (should_hoist[old_idx]) {
                // Replace with NOP
                ir_emit((Instr){.op = OP_NOP, .temp_dest = 0xFF, ...});
            } else {
                ir_emit(old_buffer[old_idx]);
            }
            old_idx++;
        }
    }
    
    // Emit remaining instructions after all loops
    while (old_idx < old_count) {
        ir_emit(old_buffer[old_idx++]);
    }
    
    // Free old buffer
    free(old_buffer);
    
    // Update global count (new IR is in g_ir_buffer)
    g_ir_count = ir_get_count();
    
    /* ===== Cleanup ===== */
    loop_analysis_free(&state);
    free(should_hoist);
}
```

---

## Key Insights

### 1. `ir_reset()` Frees Old Buffer

```c
void ir_reset(void) {
    if (g_ir_buffer) {
        free(g_ir_buffer);  // Free old buffer
        g_ir_buffer = NULL;
        g_ir_count = 0;
        g_ir_capacity = 0;
    }
    // Next ir_emit() will allocate new buffer
}
```

**Benefit**: No manual buffer management needed!

### 2. Fresh Indices Automatically

```c
// Old IR: indices 0, 1, 2, 3, 4, 5
// New IR: indices 0, 1, 2, 3, 4, 5 (fresh!)

// All jump targets, call args, live ranges use fresh indices
// No index invalidation!
```

**Benefit**: No need to update any indices!

### 3. No Other Passes Affected

```c
// After LICM:
// - IR has fresh indices
// - Jump targets are correct
// - Call args are correct
// - Live ranges are correct

// Next pass (DCE, CSE, etc.) sees clean IR
// No changes needed!
```

**Benefit**: Only LICM needs to change!

---

## Implementation Steps

### Step 1: Save Hoisted Instructions (5 min)

```c
typedef struct {
    int loop_idx;
    int *hoisted_indices;  // Indices of instructions to hoist
    int hoisted_count;
} HoistedInfo;

HoistedInfo *hoisted_list;
int hoisted_list_count;
```

### Step 2: Modify opt_licm() (30 min)

```c
void opt_licm(ZeroCCCompiler *c) {
    // Pass 1: Analysis (existing code, save hoisted info)
    // Pass 2: Rebuild (new code)
}
```

### Step 3: Test (1 hour)

```bash
make test
# Should pass all 119 tests
```

### Step 4: Verify Hoisting (30 min)

```bash
./build/bin/zerocc -dump-ir -O1 test/licm/simple_invariant.c -o /tmp/test.o
# Check that invariants are before loop header
```

### Step 5: Benchmark (30 min)

```bash
python3 tools/licm_verifier.py --benchmark
# Should see O(1) vs O(N) improvement
```

---

## Total Time Estimate

| Task | Time |
|------|------|
| Save hoisted info | 5 min |
| Modify opt_licm() | 30 min |
| Test | 1 hour |
| Verify hoisting | 30 min |
| Benchmark | 30 min |
| **Total** | **2.5 hours** |

**Buffer for debugging**: 1 day  
**Total**: **1-2 days** ✅

---

## Code Changes

### File: `src/zero_cc/parser/opt_loop.c`

**Lines Changed**: ~100 lines (mostly new code for Pass 2)

**Changes**:
1. Add `HoistedInfo` struct (10 lines)
2. Modify `opt_licm()` to save hoisted info (20 lines)
3. Add Pass 2 rebuild logic (70 lines)

**No changes to**:
- `ir.h` ✅
- `ir.c` ✅
- `regalloc.h` ✅
- Backend files ✅
- Other passes ✅

---

## Why This Works

### Before LICM (with splicing)

```
[0] PROLOGUE
[1] LABEL header     ← loop->header_idx = 1
[2] ADD r1, r1, 1    ← loop->body_indices[0] = 2
[3] JMP header       ← loop->body_indices[1] = 3

Insert at 1:
[0] PROLOGUE
[1] NEW_INSTR        ← Inserted
[2] LABEL header     ← Was at 1, now at 2! BROKEN!
[3] ADD r1, r1, 1    ← Was at 2, now at 3! BROKEN!
[4] JMP header       ← Was at 3, now at 4! BROKEN!
```

### After LICM (Two-Pass Rebuild)

```
Old IR:
[0] PROLOGUE
[1] LABEL header
[2] ADD r1, r1, 1
[3] JMP header

Pass 2: Build NEW IR
[0] PROLOGUE       ← Fresh index 0
[1] HOISTED_CODE   ← Fresh index 1
[2] LABEL header   ← Fresh index 2 (was 1)
[3] NOP            ← Fresh index 3 (was 2)
[4] JMP header     ← Fresh index 4 (was 3)

All indices are fresh! No broken references!
```

---

## Testing Strategy

### Test 1: Simple Invariant

```c
int test(int *arr, int n, int factor) {
    int result = 0;
    for (int i = 0; i < n; i++) {
        result += arr[i] * factor;  // factor is invariant
    }
    return result;
}
```

**Expected**: `factor` computation hoisted before loop

### Test 2: Multiple Invariants

```c
int test(int *arr, int n, int a, int b) {
    int result = 0;
    int x = a + b;  // Invariant
    for (int i = 0; i < n; i++) {
        result += arr[i] * x;
    }
    return result;
}
```

**Expected**: `x = a + b` hoisted before loop

### Test 3: Nested Loops

```c
int test(int **mat, int rows, int cols, int factor) {
    int result = 0;
    for (int i = 0; i < rows; i++) {
        for (int j = 0; j < cols; j++) {
            result += mat[i][j] * factor;  // factor invariant in inner loop
        }
    }
    return result;
}
```

**Expected**: `factor` hoisted from inner loop

---

## Risk Assessment

### Low Risk

| Risk | Mitigation |
|------|------------|
| **Index corruption** | Fresh IR buffer, no index updates needed |
| **Other passes break** | They see clean IR, no changes needed |
| **Backend breaks** | Backend sees final IR, no changes needed |
| **Tests fail** | Easy to debug, isolated to opt_loop.c |

### Rollback Plan

If something goes wrong:
```bash
git revert <commit>
# Back to detection-only mode
```

---

## Success Criteria

### Functional

- ✅ All 119 tests pass
- ✅ Invariants hoisted before loop headers
- ✅ No index corruption
- ✅ No backend/regalloc issues

### Performance

- ✅ O(1) invariant computations (vs O(N))
- ✅ 20-30% improvement on loop-heavy code
- ✅ Benchmarks show speedup

---

## Conclusion

**Two-Pass Rebuild is the right solution**:

1. ✅ **Fast**: 1-2 days (not 1-2 weeks)
2. ✅ **Simple**: Only opt_loop.c changes
3. ✅ **Safe**: Fresh IR, no index updates
4. ✅ **Effective**: Real LICM optimization

**Ready to implement!** 🎯
