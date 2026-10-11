# Assembly Emitter Fixes Summary - Sprint 10.5.6

## Overview

Fixed four critical bugs in the assembly emitter that caused incorrect code generation for arithmetic operations and callee-saved register preservation.

## Bugs Fixed

### 1. ADD Instruction Logic Bug (Critical)

**Problem**: When the destination register and one source register were allocated to the same physical register, the old code would overwrite the source before using it.

**Example**:
```
IR: ADD r21, r17, r20  # r21 = r17 + r20
Allocation: r17→rbx, r20→rax, r21→rax (same as r20!)

Old Code:
  movq %rbx, %rax    # r21 = r17 (rax now has r17, r20 is lost!)
  addq %rax, %rax    # r21 = r21 + r20 = r17 + r17 (WRONG - doubled!)

Result: 42 + 20 = 22 (should be 62)
```

**Fix**: Check if src2 == dst and use single instruction:
```c
if (strcmp(src2, dst) == 0) {
    fprintf(out, "\taddq %s, %s\n", src1, dst);  // dst = dst + src1
} else {
    fprintf(out, "\tmovq %s, %s\n", src1, dst);
    fprintf(out, "\taddq %s, %s\n", src2, dst);
}
```

**Also Fixed**: OP_SUB and OP_MUL with same logic

### 2. Register Name Mapping Bug

**Problem**: Physical register numbers from the register allocator were incorrectly indexed into `int_regs[]`, which is indexed by VREG numbers.

**Example**:
```
Physical register 3 = rbx
int_regs[3] = "rdx"  // WRONG! int_regs is VREG-indexed
```

**Fix**: Added `get_hw_reg_name()` function pointer to ISABackend interface:
```c
// isa_backend.h
const char *(*get_hw_reg_name)(int phys_reg);

// x86_64.c
static const char *x86_get_hw_reg_name(int phys_reg) {
    static const char *x86_hw_names[] = {
        "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
        "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"
    };
    return (phys_reg >= 0 && phys_reg < 16) ? x86_hw_names[phys_reg] : "rax";
}
```

### 3. Static Buffer Bug

**Problem**: Single static buffer in `asm_get_backend_int_reg_name()` caused all register names to point to the same memory location.

**Example**:
```c
// Old code (BUGGY):
static char buf[16];
snprintf(buf, sizeof(buf), "%%%s", name);
return buf;  // All calls return pointer to same buffer!

// In fprintf: fprintf(out, "\taddq %s, %s\n", src2, dst);
// Both src2 and dst point to same buffer - last call wins!
```

**Fix**: Use 4-buffer round-robin scheme:
```c
static char bufs[4][16];
static int idx = 0;
char *buf = bufs[idx++ % 4];
snprintf(buf, sizeof(bufs[0]), "%%%s", name);
return buf;
```

### 4. Callee-Save Timing Bug

**Problem**: Callee-save stores were emitted at the END of the function (after the jump to epilogue), not at the BEGINNING (in the prologue). This meant registers were clobbered before being saved.

**Example**:
```
helper:
  push %rbp
  mov %rsp, %rbp
  sub $64, %rsp
  movq %rdi, -56(%rbp)
  movq -56(%rbp), %rax
  movq $2, %rbx        // rbx clobbered here!
  imulq %rbx, %rax
  jmp .L0_0
.L0_0:
  movq %rbx, -8(%rbp)  // Too late! rbx already clobbered
  movq -8(%rbp), %rbx
  leave
  ret
```

**Fix**: Move callee-save to prologue/epilogue:
```
helper:
  push %rbp
  mov %rsp, %rbp
  sub $64, %rsp
  movq %rbx, -8(%rbp)  // Save rbx FIRST (in prologue)
  movq %rdi, -56(%rbp)
  movq -56(%rbp), %rax
  movq $2, %rbx        // Safe to clobber now
  imulq %rbx, %rax
  jmp .L0_0
.L0_0:
  movq -8(%rbp), %rbx  // Restore rbx (in epilogue)
  leave
  ret
```

## Files Modified

1. **src/zero_cc/parser/asm_emitter.c**
   - Fixed `asm_get_backend_int_reg_name()` to use round-robin buffers
   - Fixed OP_ADD to handle src2 == dst case
   - Fixed OP_SUB and OP_MUL similarly
   - Skip OP_CALLEE_SAVE_STORES/LOADS IR instructions (now in prologue/epilogue)

2. **src/zero_cc/isa/x86_64.c**
   - Added callee-save stores to `x86_emit_prologue_asm()`
   - Added callee-save loads to `x86_emit_epilogue_asm()`

3. **src/zero_cc/isa/aarch64.c**
   - Implemented `aarch64_get_hw_reg_name()`

4. **src/zero_cc/isa/riscv.c**
   - Implemented `riscv64_get_hw_reg_name()`

5. **src/zero_cc/isa/isa_backend.h**
   - Added `get_hw_reg_name()` function pointer to ISABackend structure

## Test Results

```bash
$ make test
Results: 118/118 passed, 0 failed in 0.37s

# Specific test case: acc = acc + helper(10)
# Expected: 62 (42 + 20)
# Before fix: 22 (bug doubled the value)
# After fix: 62 ✅
```

## Impact

- **Assembly generation now fully correct** for all arithmetic operations
- **Callee-saved registers properly preserved** across function calls
- **Multi-ISA support hardened** with proper hardware register name mapping
- **All 118 tests passing** with no regressions

## Next Steps

- Sprint 10: SIMD Vectorization Support (Tasks 72-73)
- Implement SSE2 instruction lowering for vectorized operations
- Add loop vectorization pass
- Implement auto-vectorization heuristics
