# Prologue Assembly Fix Summary

## Issue
The function prologue was missing from assembly output when using `-S` flag.

## Root Cause
The `emit_prologue_asm` function was using the wrong register names:
- It was using `BE(c)->regs->int_regs[BE(c)->sp_reg]` and `BE(c)->regs->int_regs[BE(c)->fp_reg]`
- The `int_regs` array is indexed by VREG number (0-15 for x86-64)
- But `sp_reg` and `fp_reg` are hardware register numbers (4=rsp, 5=rbp for x86-64)
- This caused the wrong register names to be used (rsi/rdi instead of rsp/rbp)

## Fix
Changed `emit_prologue_asm` to use `asm_get_reg_name` instead of directly indexing `int_regs`:
```c
const char *rsp = asm_get_reg_name(BE(c)->sp_reg, 0);
const char *rbp = asm_get_reg_name(BE(c)->fp_reg, 0);
```

The `asm_get_reg_name` function uses a hardcoded array that correctly maps hardware register numbers to register names:
- `gpr_regs[4] = "%rsp"`
- `gpr_regs[5] = "%rbp"`

## Result
The assembly output now correctly shows:
```asm
.globl main
main:
    push %rbp
    mov %rsp, %rbp
    sub $64, %rsp
    ...
```

## Testing
- All 118/118 tests pass
- Assembly output can be compiled and linked successfully
- Verified with multiple test cases

## Files Modified
- `src/zero_cc/parser/asm_emitter.c`: Fixed `emit_prologue_asm` to use `asm_get_reg_name`
