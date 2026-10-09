# Assembly Emitter ADD Instruction Fix

## Problem

The assembly emitter was generating incorrect code for the `ADD` instruction when the destination register and one of the source registers were allocated to the same physical register.

### Example Bug

Given IR:
```
ADD r21, r17, r20  # r21 = r17 + r20
```

Where:
- `r17` is allocated to `rbx` (physical register 3)
- `r20` is allocated to `rax` (physical register 0)
- `r21` is allocated to `rax` (physical register 0) - **same as r20!**

The old code generated:
```asm
movq %rbx, %rax    # r21 = r17 (rax now contains r17)
addq %rax, %rax    # r21 = r21 + r20 = r17 + r17 (WRONG! r20 was overwritten)
```

This doubled `r17` instead of adding `r17 + r20`.

## Root Cause

The issue had two parts:

### 1. Register Name Mapping Bug

The `asm_get_backend_int_reg_name()` function was using a single static buffer:
```c
static char buf[16];
snprintf(buf, sizeof(buf), "%%%s", name);
return buf;
```

When multiple register names were used in the same `fprintf` statement (e.g., `fprintf(out, "\taddq %s, %s\n", src2, dst)`), all pointers pointed to the same buffer, and the last call overwrote the previous values.

**Fix**: Use round-robin buffers:
```c
static char bufs[4][16];
static int idx = 0;
char *buf = bufs[idx++ % 4];
snprintf(buf, sizeof(bufs[0]), "%%%s", name);
return buf;
```

### 2. Physical Register to Register Name Mismatch

The register allocator returns **physical register numbers**, but the assembly emitter was using these as indices into `int_regs[]`, which is indexed by **VREG numbers**.

For x86-64:
- Physical register 3 = `rbx`
- `int_regs[3]` = `"rdx"` (WRONG!)

**Fix**: Use the new `get_hw_reg_name()` function pointer in the backend interface, which correctly maps physical register numbers to register names.

### 3. ADD Instruction Logic Bug

When `src2 == dst` (both allocated to the same register), the old code:
```asm
movq src1, dst    # dst = src1 (overwrites src2!)
addq src2, dst    # dst = dst + src2 (but src2 was just overwritten!)
```

**Fix**: Check if `src2 == dst` and handle it specially:
```c
if (strcmp(src2, dst) == 0) {
    /* dst already contains src2, just add src1 */
    fprintf(out, "\taddq %s, %s\n", src1, dst);
} else {
    fprintf(out, "\tmovq %s, %s\n", src1, dst);
    fprintf(out, "\taddq %s, %s\n", src2, dst);
}
```

### 4. Callee-Save Timing Bug

The callee-save stores were being emitted at the END of the function (after the jump to epilogue), not at the BEGINNING (in the prologue). This meant registers were clobbered before being saved.

**Fix**: Move callee-save stores to the prologue and loads to the epilogue:
- `x86_emit_prologue_asm()`: Added callee-save store emission
- `x86_emit_epilogue_asm()`: Added callee-save load emission
- `asm_emitter.c`: Skip `OP_CALLEE_SAVE_STORES/LOADS` IR instructions

## Files Modified

1. **src/zero_cc/parser/asm_emitter.c**
   - Fixed `asm_get_backend_int_reg_name()` to use round-robin buffers
   - Fixed `OP_ADD` to handle `src2 == dst` case
   - Fixed `OP_SUB` and `OP_MUL` similarly
   - Skip `OP_CALLEE_SAVE_STORES/LOADS` (now handled in prologue/epilogue)

2. **src/zero_cc/isa/x86_64.c**
   - Added callee-save stores to `x86_emit_prologue_asm()`
   - Added callee-save loads to `x86_emit_epilogue_asm()`

3. **src/zero_cc/isa/isa_backend.h**
   - Added `get_hw_reg_name()` function pointer

4. **src/zero_cc/isa/aarch64.c** and **src/zero_cc/isa/riscv.c**
   - Implemented `get_hw_reg_name()` for each ISA

## Testing

```bash
$ make test
Results: 118/118 passed, 0 failed in 0.37s

$ ./build/bin/zerocc -S -o test.s test.c
$ cat test.s | grep -A 2 "# 8: ADD"
    addq %rbx, %rax    # Correct!
```

## Result

The test case `acc = acc + helper(10)` now correctly returns 62 (42 + 20) instead of 22.
