# VLA Array Access Bug Fix - Summary

## Problem
The ZeroCC compiler was producing incorrect results when accessing arrays (both fixed-size and VLA) using loop variables as indices. The test `vla_test` was failing with incorrect values.

### Symptoms
- `run_vla(5)` with `for (int i = 0; i < n; i++) { arr[i] = i * 2; }` returned 0, 8, or 56 instead of expected 20
- Even simple cases like `arr[i] = 42; return arr[0];` returned 0 instead of 42

## Root Cause
The bug was in the x86-64 backend's shift instruction emission (`x86_emit_shl`, `x86_emit_shr`, `x86_emit_sar`).

The x86-64 `SHL` instruction format is: `shl dst_reg, cl` where:
- The value to shift must be in `dst_reg`
- The shift count must be in `CL` (low byte of RCX register)

The original code had this logic:
```c
if (dst_reg != src1) {
    x86_mov_rr(c, dst_reg, src1);  // Move value to dst
}
if (src2 != 1) {  // Move shift amount to RCX
    x86_mov_rr(c, 1, src2);
}
// SHL dst_reg, cl
```

**The Bug:** When the register allocator assigned RCX (register 1) as either the source or destination, the code would clobber the value. Specifically:
1. If `dst_reg == RCX`, the value would be moved to RCX
2. Then the shift amount would be moved to RCX, **overwriting the value**
3. The SHL would shift by the wrong amount

## Solution
Modified `x86_emit_shl`, `x86_emit_shr`, and `x86_emit_sar` in `src/zero_cc/isa/x86_64.c` to handle the case where `dst_reg == RCX`:

```c
/* If dst_reg is RCX, use a temp register */
if (dst_reg == 1) {  /* RCX */
    use_temp = 1;
    temp_reg = (src1 == 10) ? 11 : 10;  /* Use r10 or r11 */
}

/* Move value to temp (if needed) or dst */
int dest_for_value = use_temp ? temp_reg : dst_reg;
if (dest_for_value != src1) {
    x86_mov_rr(c, dest_for_value, src1);
}

/* Move shift amount to RCX */
if (src2 != 1) {
    x86_mov_rr(c, 1, src2);
}

/* If using temp, shift the temp, then move result to dst */
if (use_temp) {
    /* SHL temp_reg, cl */
    x86_mov_rr(c, dst_reg, temp_reg);
} else {
    /* SHL dst_reg, cl */
}
```

This ensures that:
1. The value to shift is in a safe register (not RCX if we need RCX for shift count)
2. The shift amount is loaded into RCX
3. The shift is performed
4. The result is moved to the final destination

## Files Modified
1. `src/zero_cc/isa/x86_64.c`:
   - `x86_emit_shl` (lines 256-292): Fixed shift left instruction emission
   - `x86_emit_shr` (lines 786-818): Fixed shift right instruction emission  
   - `x86_emit_sar` (lines 721-753): Fixed arithmetic shift right instruction emission

2. `src/zero_cc/parser/ir.c`:
   - Fixed `OP_MOV_MEM` IR printing (line 79): Changed `[sp-%u]` to `[fp%+d]`
   - Added `OP_STORE_32` IR printing (line 183)
   - Added `OP_DEREF` IR printing (line 186)

## Test Results
- Before fix: 117/118 tests passing (vla_test failing)
- After fix: 118/118 tests passing

## Verification
```bash
cd /home/ubuntu/dev/qnz_c_compiler
make clean && make
make test  # All 118 tests pass
```
