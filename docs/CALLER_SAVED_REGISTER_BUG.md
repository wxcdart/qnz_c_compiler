# Caller-Saved Register Clobbering Bug - Complete Analysis

## Executive Summary

The AArch64 backend has a critical bug where **caller-saved registers (x0-x18) are assigned to temporaries that need to survive function calls**, causing register clobbering.

**Root Cause**: The register allocator doesn't mark intervals that span function calls, so it freely assigns caller-saved registers without realizing they need to be preserved.

**Impact**: Accumulation test returns 20 instead of 52 (incorrect result)

**Severity**: CRITICAL - Affects any code with multiple function calls

## Detailed Analysis

### The Bug

Test case: `acc = 42; acc = acc + abs_val(10);`

```c
main():
  0x64: ldur x3, [x29, #-56]   // Load accumulator (42) to x3
  0x7c: bl abs_val              // Call function - x3 gets clobbered!
  0x80: add x0, x3, x0          // BUG: x3 is now 0 instead of 42!
```

### Why It Happens

1. The register allocator assigns temporary variables to physical registers
2. For the accumulator (temp holding 42), it assigns x3
3. x3 is a **caller-saved** register (x0-x18)
4. When `abs_val` executes, it uses x3: `mov x3, #0`
5. The callee is free to use x3 because the caller should have saved it
6. But the compiler never saves/restores x3 around the call

### AArch64 Calling Convention

| Register | Type | Preserved |
|----------|------|-----------|
| x0-x7    | Argument/Return | Caller-saved |
| x8-x15   | Temporary | Caller-saved |
| x16-x17  | Intra-call temp | Caller-saved |
| x18      | Platform | Caller-saved |
| x19-x28  | Callee-saved | YES (callee must preserve) |
| x29      | Frame pointer | YES |
| x30      | Link register | YES |

The key point: **x0-x18 are caller-saved**, so the caller must save them before calling another function.

## Python Tools Created

I created 3 new Python tools to help debug and verify this issue:

### 1. `register_clobbering_analyzer.py`
Analyzes object files to detect caller-saved register clobbering patterns.

```bash
python3 tools/register_clobbering_analyzer.py --object test.o
```

Output:
```
Found 1 potential register clobbering issues:

Function: main
  [SEVERITY: HIGH] Register x3
    Type: load_before_call_use_after
    Load at:  0x64
    Call at:  0x7c
    Use at:   0x80
    Issue: x3 loaded at 0x64, call at 0x7c, used at 0x80 - register may be clobbered!
```

### 2. `register_allocator_analyzer.py`
Analyzes IR and object files together to detect register allocator issues.

```bash
python3 tools/register_allocator_analyzer.py --ir ir_dump.txt --object test.o
```

### 3. `verify_caller_saved_fix.py`
Verifies that save/restore patterns are present around function calls.

```bash
python3 tools/verify_caller_saved_fix.py --object test.o
```

## Root Cause in Code

### File: `src/zero_cc/parser/regalloc.h`

The register allocator has a function to mark intervals as spanning calls:

```c
static void regalloc_set_interval_spans_call(RegAllocator *ra, int interval_idx, bool spans_call)
```

But **this function is NEVER CALLED** during IR lowering!

### File: `src/zero_cc/parser/ast_lowering_mc.c`

The OP_CALL handler doesn't mark intervals as spanning the call:

```c
case OP_CALL: {
    // ... code to extract arguments and emit call ...
    BE(c)->emit_call(c, fn_name);
    // ... but NO call to regalloc_set_interval_spans_call() ...
}
```

## Solution Options

### Option 1: Quick Fix (Band-Aid)
Save/restore all caller-saved registers before/after each call.

**Pros**: Works immediately
**Cons**: Inefficient, generates extra spill code, doesn't fix the root cause

**Status**: Attempted but failed because register allocator doesn't create spill slots for allocated registers

### Option 2: Medium Fix (Proper)
Mark intervals that span calls, modify `alloc_reg()` to prefer callee-saved registers.

```c
// In IR lowering, detect which temps are live across calls
for (size_t i = 0; i < c->regalloc.interval_count; i++) {
    // Check if interval spans any OP_CALL instruction
    if (interval_spans_call(&c->regalloc.intervals[i])) {
        regalloc_set_interval_spans_call(&c->regalloc, i, true);
    }
}
```

Then modify `alloc_reg()` to prefer callee-saved registers for spanning intervals.

**Pros**: Correct solution, minimal code change
**Cons**: Requires understanding register allocator internals

### Option 3: Full Fix (Best)
Implement proper caller-saved register save/restore in `ast_lowering_mc.c`.

```c
case OP_CALL: {
    // Save caller-saved registers that have live values
    save_caller_saved_regs(c, live_regs);
    
    // Emit the call
    BE(c)->emit_call(c, fn_name);
    
    // Restore caller-saved registers
    restore_caller_saved_regs(c, live_regs);
}
```

**Pros**: Guaranteed to work, handles all cases
**Cons**: More code, may generate extra spills

## Implementation Path

### Step 1: Detect Spans-Call Intervals
Add code in `ast_lowering_mc.c` to mark intervals spanning calls:

```c
static bool interval_spans_call(LiveInterval *interval, size_t call_instr_idx) {
    return interval->start <= call_instr_idx && interval->end > call_instr_idx;
}

// In IR lowering loop...
for (size_t i = 0; i < instr_count; i++) {
    if (instrs[i].op == OP_CALL) {
        for (size_t j = 0; j < c->regalloc.interval_count; j++) {
            if (interval_spans_call(&c->regalloc.intervals[j], i)) {
                regalloc_set_interval_spans_call(&c->regalloc, j, true);
            }
        }
    }
}
```

### Step 2: Modify Register Allocation
Update `alloc_reg()` in `regalloc.h` to prefer callee-saved registers:

```c
static int alloc_reg(RegAllocator *ra, LiveInterval *interval) {
    // Prefer callee-saved registers (x19-x28) if interval spans calls
    if (interval->spans_call) {
        // Allocate from callee-saved registers first
        for (int reg : CALLEE_SAVED_REGS) {
            if (is_free(ra, reg)) return reg;
        }
    }
    
    // Otherwise use caller-saved registers
    for (int reg : CALLER_SAVED_REGS) {
        if (is_free(ra, reg)) return reg;
    }
    
    // Spill if no free registers
    return SPILL;
}
```

### Step 3: Test
```bash
make clean && make
make test
./build/bin/zerocc -c /tmp/test_accum2.c -o /tmp/test_accum2.o --target aarch64
aarch64-linux-gnu-gcc /tmp/test_accum2.o -static -o /tmp/test_accum2_static
qemu-aarch64-static /tmp/test_accum2_static  # Should return 52
```

## Verification

Use the created Python tools to verify the fix:

```bash
# Before fix
python3 tools/register_clobbering_analyzer.py --object /tmp/test_accum2.o
# Output: Found 1 potential register clobbering issues

# After fix
python3 tools/register_clobbering_analyzer.py --object /tmp/test_accum2.o
# Output: ✓ No register clobbering issues detected!
```

## Files That Need Changes

1. **src/zero_cc/parser/regalloc.h**
   - Modify `alloc_reg()` to handle `spans_call` intervals

2. **src/zero_cc/parser/ast_lowering_mc.c**
   - Add interval marking logic before IR lowering
   - Alternatively, add save/restore code around OP_CALL

3. **tests**
   - Add regression test for multiple function calls
   - Add accumulation test case

## Current Status

- ✅ Bug identified and characterized
- ✅ Python analysis tools created to detect the bug
- ❌ Fix implemented (attempted quick fix, didn't work)
- ⏳ Medium/Full fix implementation needed

## Next Steps

1. **Implement Step 1**: Detect spans-call intervals in IR lowering
2. **Implement Step 2**: Modify `alloc_reg()` to prefer callee-saved registers
3. **Test**: Verify with test suite and tool validation
4. **Commit**: Create clean commit with proper description
5. **Document**: Update IR design documentation

## References

- AArch64 AAPCS64 Calling Convention
- ARM Architecture Reference Manual
- `src/zero_cc/parser/regalloc.h` - Register allocator implementation
- `src/zero_cc/parser/ast_lowering_mc.c` - IR to machine code lowering
