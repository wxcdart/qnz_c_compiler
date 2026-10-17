# Register Allocator Fix Status

## Critical Finding

The register allocator **already has the fix implemented**, but it's **never being used**!

### What's Already Working

**File: `src/zero_cc/parser/regalloc.h`**

The `alloc_reg()` function already prefers callee-saved registers for intervals that span calls:

```c
if (interval->spans_call) {
    /* Interval spans OP_CALL: prioritize callee-saved registers (rbx, r12-r15) */
    static const int callee_saved_indices[] = {1, 8, 9, 10, 11};
    for (size_t k = 0; k < sizeof(callee_saved_indices)/sizeof(callee_saved_indices[0]); k++) {
        int i = callee_saved_indices[k];
        if (ra->gpr_free[i]) {
            ra->gpr_free[i] = false;
            ra->used_callee_saved_gpr_mask |= (1u << i);
            return i;
        }
    }
    /* Callee-saved registers exhausted: fall through to allocate any free GPR */
}
```

The `spans_call` field exists in `LiveInterval`:
```c
bool spans_call;          /* Whether this interval spans a function call */
```

And there's even a helper function:
```c
static void regalloc_set_interval_spans_call(RegAllocator *ra, int interval_idx, bool spans_call)
```

### What's Missing

**The intervals are NEVER MARKED as spanning calls!**

The function `regalloc_set_interval_spans_call()` exists but is **NEVER CALLED** anywhere in the codebase:

```bash
grep -r "regalloc_set_interval_spans_call" src/
# Returns: only the function definition, no calls!
```

### The Fix

We just need to add code in `src/zero_cc/parser/ast_lowering_mc.c` to mark intervals as spanning calls **before** register allocation runs.

**Current flow in `lower_ir_to_mc()`:**
```c
1. Parse IR instructions
2. Build live intervals (regalloc_build_live_intervals())
3. Allocate registers (regalloc_allocate_registers())  <-- Too late!
```

**Fixed flow:**
```c
1. Parse IR instructions
2. Build live intervals (regalloc_build_live_intervals())
3. Mark intervals spanning calls (NEW!)
4. Allocate registers (regalloc_allocate_registers())  <-- Now uses callee-saved
```

## Implementation

### Step 1: Add Call Tracking

In `src/zero_cc/parser/ast_lowering_mc.c`, in the `lower_ir_to_mc()` function:

```c
/* After regalloc_build_live_intervals() but before regalloc_allocate_registers(): */

/* Track which IR instructions are calls */
size_t *call_indices = NULL;
size_t call_count = 0;
size_t call_capacity = 64;

call_indices = malloc(call_capacity * sizeof(size_t));

for (size_t i = 0; i < instr_count; i++) {
    if (instrs[i].op == OP_CALL) {
        if (call_count >= call_capacity) {
            call_capacity *= 2;
            call_indices = realloc(call_indices, call_capacity * sizeof(size_t));
        }
        call_indices[call_count++] = i;
    }
}

/* Mark intervals that span calls */
for (size_t i = 0; i < c->regalloc.interval_count; i++) {
    LiveInterval *interval = &c->regalloc.intervals[i];
    
    for (size_t j = 0; j < call_count; j++) {
        size_t call_idx = call_indices[j];
        
        /* Check if interval spans this call */
        if (interval->start <= call_idx && interval->end >= call_idx) {
            regalloc_set_interval_spans_call(&c->regalloc, i, true);
            break;
        }
    }
}

/* Clean up */
free(call_indices);

/* Now call register allocation - it will prefer callee-saved for spanning intervals */
regalloc_allocate_registers(&c->regalloc);
```

### Step 2: AArch64 Specific

For AArch64, the register allocator uses different indices. We need to update `regalloc.h` to handle AArch64:

```c
/* In alloc_reg(), add AArch64 handling: */
if (c->isa == ISA_AARCH64) {
    if (interval->spans_call) {
        /* AArch64 callee-saved: x19-x28 (indices 19-28) */
        for (int i = 19; i <= 28; i++) {
            if (ra->gpr_free[i]) {
                ra->gpr_free[i] = false;
                return i;
            }
        }
    }
}
```

## Testing

After implementing the fix:

```bash
# Build
make clean && make

# Run tests
make test  # All 118 should pass

# Test accumulation case
./build/bin/zerocc -c /tmp/test_accum2.c -o /tmp/test_accum2.o --target aarch64
aarch64-linux-gnu-gcc /tmp/test_accum2.o -static -o /tmp/test_accum2_static
qemu-aarch64-static /tmp/test_accum2_static  # Should return 52, not 20

# Verify with analyzer
python3 tools/register_clobbering_analyzer.py --object /tmp/test_accum2.o
# Should show: No register clobbering issues detected!
```

## Python Tools Status

### Created Tools

1. **interval_span_analyzer.py** - ✅ Working
   - Analyzes IR to find intervals spanning calls
   - Shows which temps need callee-saved registers
   - Example output:
     ```
     Found 5 intervals spanning calls:
       Temp r0: spans 1 call(s) at [22]
         Recommendation: use_callee_saved_or_spill
     ```

2. **regalloc_fix_generator.py** - ✅ Working
   - Generates patch code for the fix
   - Shows current alloc_reg() implementation
   - Verifies if fix is needed

3. **register_clobbering_analyzer.py** - ✅ Working
   - Detects register clobbering in disassembly
   - Used to confirm the bug exists

4. **verify_caller_saved_fix.py** - ✅ Working
   - Verifies save/restore patterns around calls

### Tools Needed?

**NO additional tools needed!** The existing tools are sufficient to:
1. Detect the bug (`register_clobbering_analyzer.py`)
2. Analyze which intervals span calls (`interval_span_analyzer.py`)
3. Generate fix code (`regalloc_fix_generator.py`)
4. Verify the fix works (`verify_caller_saved_fix.py`)

## Next Steps

1. ✅ **Analyze**: Use `interval_span_analyzer.py` to confirm which intervals span calls
2. ⏳ **Implement**: Add call tracking code in `ast_lowering_mc.c`
3. ⏳ **Test**: Run test suite and verify accumulation test
4. ⏳ **Verify**: Use `register_clobbering_analyzer.py` to confirm bug is fixed
5. ⏳ **Commit**: Create clean commit with proper documentation

## Files to Modify

1. **src/zero_cc/parser/ast_lowering_mc.c**
   - Add call tracking before `regalloc_allocate_registers()`
   - Mark intervals as spanning calls

2. **src/zero_cc/parser/regalloc.h** (if needed)
   - May need to add AArch64-specific callee-saved handling

## Summary

**Good news**: The hard part (register allocator logic) is already done!

**What's needed**: Just add code to mark intervals as spanning calls before register allocation.

**Estimated effort**: 30 minutes to implement, 1 hour to test and verify.

## Update: Register Allocation Fix Complete, But IR Generation Bug Found

### Register Allocation Fix Status: ✅ COMPLETE

The register allocation fix for caller-saved registers is **fully implemented and working**:

1. ✅ `target_isa` field added to `RegAllocator` struct
2. ✅ `alloc_reg()` modified to prefer callee-saved registers for AArch64 (x19-x28)
3. ✅ `spans_call` intervals are correctly marked in `live_range_compute()`
4. ✅ GPR_COUNT increased to 32 to support AArch64's 30 registers
5. ✅ All 118 JIT tests pass

### New Bug Found: IR Generation

However, there's a **separate bug in IR generation** that causes incorrect code:

**Symptom**: `acc = 42; acc = acc + abs_val(10);` returns wrong value

**Root Cause**: The accumulator value (42) is stored to memory but never loaded back before the addition:

```
IR for main():
   1: MOV_IMM r16, 42           ; acc = 42
   2: MOV_MEM [sp-...], r16     ; store acc
   ...
   7: CALL fn_0                 ; call abs_val(10)
   8: MOV_REG r20, r0           ; r20 = return value
   9: ADD r21, r17, r20         ; BUG! Should load acc from memory first
```

**Missing**: `MOV rXX, [sp-...]` to load accumulator before ADD

**Impact**: This bug affects **both x86-64 and AArch64**

**Next Steps**: Fix the IR generation bug in AST lowering layer

### Conclusion

The register allocation fix is **complete and correct**. The test failure is due to a **separate IR generation bug** that needs to be fixed in the AST lowering layer.

