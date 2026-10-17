# IR Dump Display Bug Fix

## Problem

The IR dump was showing stack loads incorrectly:

```
3: MOV r17, [flags] r255  ; WRONG - looks like saving flags
```

Instead of:

```
3: LOAD r17, [fp-56]      ; CORRECT - loading from stack
```

## Root Cause

The `ir_emit_load()` function in `src/zero_cc/parser/ir_emit_ops.h` encodes stack loads using:
- `.op = OP_MOV` 
- `.temp_src1 = 0xFF` (frame pointer marker)
- `.imm = offset` (stack offset)

This encoding uses `OP_MOV` with `temp_src1 = 0xFF` as a marker to indicate "load from stack frame pointer at offset X".

However, the IR dump function in `src/zero_cc/parser/ir.c` didn't recognize this pattern. It was treating all `OP_MOV` instructions the same way, checking if `flags_in == REG_FLAGS` to display `[flags]`, which caused the misleading output.

## Solution

Updated the IR dump function to check for the frame pointer marker:

```c
case OP_MOV: 
    if (instr->temp_src1 == 0xFF) {
        /* This is actually a LOAD from stack (temp_src1=0xFF is frame pointer marker) */
        fprintf(f, "LOAD r%u, [fp%+d]", instr->temp_dest, (int)instr->imm);
    } else {
        fprintf(f, "MOV r%u, ", instr->temp_dest);
        if (instr->flags_in == REG_FLAGS) fprintf(f, "[flags] ");
        fprintf(f, "r%u", instr->temp_src1);
    }
    break;
```

## Impact

- **No functional change** - the IR generation was correct all along
- Only the **display** was fixed
- All 118 JIT tests still pass
- IR dump now correctly shows stack loads as `LOAD rX, [fp+offset]`

## Files Changed

- `src/zero_cc/parser/ir.c` - Fixed IR dump to recognize frame pointer marker

## Testing

Test case:
```c
int abs_val(int x) {
    return x < 0 ? -x : x;
}

int main() {
    int acc = 42;
    int c = acc + abs_val(10);
    return c;  // Returns 52 (correct)
}
```

IR dump now correctly shows:
```
=== IR for function 'main' ===
   0: PROLOGUE 0 [flags_out=r0] [flags_in=r0]
   1: MOV_IMM r16, 42 [flags_out=r0] [flags_in=r0]
   2: MOV_MEM [sp-...], r16 [flags_out=r0] [flags_in=r0]
   3: LOAD r17, [fp-56] [flags_out=r0] [flags_in=r0]  ; CORRECT!
   4: MOV_IMM r18, 10 [flags_out=r0] [flags_in=r0]
   5: MOV_MEM [sp-...], r18 [flags_out=r0] [flags_in=r0]
   6: LOAD r19, [fp-72] [flags_out=r0] [flags_in=r0]
   7: CALL fn_0 [flags_out=r0] [flags_in=r0]
   8: MOV_REG r20, r0 [flags_out=r0] [flags_in=r0]
   9: ADD r21, r17, r20 [flags_out=r0] [flags_in=r0]  ; Correctly adds acc + abs_val(10)
  10: MOV_MEM [sp-...], r21 [flags_out=r0] [flags_in=r0]
  11: LOAD r22, [fp-64] [flags_out=r0] [flags_in=r0]
  12: MOV_REG r0, r22 [flags_out=r0] [flags_in=r0]
  13: JMP L0 [flags_out=r0] [flags_in=r0]
  14: L0: [flags_out=r0] [flags_in=r0]
```

## Lessons Learned

1. **Debug output can be misleading** - The IR dump was showing incorrect information, which led to a false diagnosis of an IR generation bug
2. **Always verify the actual execution** - The test returned the correct result (52), which should have been a clue that the IR generation was correct
3. **Check encoding conventions** - The `temp_src1 = 0xFF` marker is a clever way to reuse `OP_MOV` for stack loads, but it requires special handling in dump/debug functions
