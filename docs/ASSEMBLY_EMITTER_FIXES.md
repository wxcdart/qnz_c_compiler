# Assembly Emitter Bug Fixes - Sprint 10.5.5

## Overview

This document summarizes the critical bug fixes applied to the assembly emitter in Sprint 10.5.5, which brought the ZeroCC compiler to 100% test pass rate (118/118 tests).

## Timeline

- **Sprint 10.5**: Initial assembly emitter implementation (117/118 tests passing)
- **Sprint 10.5.5**: Bug fixes and completion (118/118 tests passing)

## Issues Fixed

### 1. OP_MOV Segfault (Critical)

**Location**: `src/zero_cc/parser/asm_emitter.c:105-108`

**Problem**:
```c
// BUG: Casting integer to const char* causes segfault
fprintf(out, "\tmovq %s(%s), %s\n", 
        instr->imm < 0 ? "-" : "", 
        instr->imm < 0 ? (const char*)(-(int)instr->imm) : (const char*)instr->imm,
        dst);
```

**Fix**:
```c
// Correct instruction emission
fprintf(out, "\tmovq %d(%%rbp), %s\n", (int)instr->imm, dst);
```

**Impact**: Compiler crashed when compiling any function with stack variable access.

---

### 2. FP/SIMD Wrong Register Numbers (High)

**Location**: `src/zero_cc/parser/asm_emitter.c:311-364`

**Problem**: FP and SIMD operations used virtual temp numbers instead of physical registers:
```c
// BUG: Uses virtual temp numbers
case OP_FADD:
    fprintf(out, "\taddsd %%xmm%u, %%xmm%u\n", instr->temp_src1, instr->temp_dest);
```

**Fix**:
```c
// Correct: Map to physical registers
case OP_FADD:
    int phys_src1 = asm_get_phys_reg(&c->regalloc, instr->temp_src1);
    int phys_dest = asm_get_phys_reg(&c->regalloc, instr->temp_dest);
    fprintf(out, "\taddsd %%xmm%u, %%xmm%u\n", phys_src1, phys_dest);
```

**Impact**: Generated incorrect assembly for FP and SIMD operations.

---

### 3. Thread Safety - Static Buffer (Medium)

**Location**: `src/zero_cc/parser/asm_emitter.c:44-62`

**Problem**: Single static buffer returned by register name function:
```c
// BUG: Single static buffer overwritten on each call
static char buf[32];
snprintf(buf, sizeof(buf), "%%%s", ...);
return buf;
```

**Fix**:
```c
// 4-buffer round-robin scheme
static char bufs[4][16];
static int idx = 0;
char *buf = bufs[idx++ % 4];
snprintf(buf, sizeof(bufs[0]), "%%%s", ...);
return buf;
```

**Impact**: Potential register name corruption when multiple registers used in same instruction.

---

### 4. Function Call Arguments (High)

**Location**: `src/zero_cc/parser/asm_emitter.c:224-265`

**Problem**: Assembly emitter didn't move arguments to correct registers before call.

**Fix**: Implemented System V AMD64 ABI argument passing:
```c
case OP_CALL:
    // Get call arguments from IR call args table
    const IRCallArgs *call_args = ir_call_args_find(idx);
    int arg_count = call_args ? call_args->count : 0;
    
    // First 6 args: rdi, rsi, rdx, rcx, r8, r9
    static const char *int_arg_regs[] = {"rdi", "rsi", "rdx", "rcx", "r8", "r9"};
    
    // Move args to registers
    for (int a = 0; a < n_reg_args; a++) {
        int temp = call_args->temps[a];
        int phys = asm_get_phys_reg(&c->regalloc, temp);
        const char *reg = asm_get_backend_int_reg_name(c, phys);
        fprintf(out, "\tmovq %s, %%%s\n", reg, int_arg_regs[a]);
    }
    
    // Stack args (beyond 6): push in reverse order
    // ...
```

**Impact**: Function calls with arguments produced incorrect results.

---

### 5. Stack Alignment (Fixed)

**Location**: `src/zero_cc/parser/ast_lowering.c:127-130`

**Problem**: I initially introduced conditional frame size calculation thinking JIT and assembly needed different alignment.

**Incorrect Fix**:
```c
if (c->emit_asm) {
    // WRONG: Assembly needs 8 mod 16 after push rbp for proper call alignment
    size_t base_size = (size_t)((c->current_stack_frame + 7) & ~7);
    total_frame_size = base_size + 8;
} else {
    // JIT uses original 16-byte alignment
    total_frame_size = (size_t)((c->current_stack_frame + 15) & ~15);
}
```

**Correct Fix**:
```c
// Both JIT and assembly need 16-byte alignment for proper call alignment
// After push rbp (8 bytes), RSP is 0 mod 16
// After sub rsp, frame_size, RSP should remain 0 mod 16
// So frame_size must be 0 mod 16 (16-byte aligned)
size_t total_frame_size = (size_t)((c->current_stack_frame + 15) & ~15);
```

**Impact**: Assembly mode had stack misalignment causing segfaults when calling functions like printf.

**Root Cause**: Misunderstanding of System V ABI stack alignment requirements. Both modes use the same prologue pattern and need identical alignment.

---

### 6. Use-After-Free Warning (Low)

**Location**: `src/main.c:256,405`

**Problem**: Duplicate `free(source)` calls in error paths after preprocessing.

**Fix**: Removed duplicate free() calls since source already freed after preprocessing.

**Impact**: Compiler warning, no runtime impact.

---

## Files Modified

| File | Changes | Lines |
|------|---------|-------|
| `src/zero_cc/parser/asm_emitter.c` | OP_MOV fix, FP/SIMD register mapping, function call arguments | ~50 |
| `src/zero_cc/parser/ast_lowering.c` | Conditional frame size calculation | ~10 |
| `src/main.c` | Removed duplicate free() calls | ~2 |

---

## Test Results

### Before Fixes
- JIT tests: 117/118 PASS (99.15%)
- Assembly generation: Segfaults, incorrect results

### After Fixes
- JIT tests: 118/118 PASS (100%)
- Assembly generation: Valid, runnable assembly

### Verification Tests

```bash
# Simple function call
./build/bin/zerocc -S -o test.s test.c
as -o test.o test.s
gcc -o test test.o
./test  # Exit code: 7 (3 + 4)

# Multiple arguments
./build/bin/zerocc -S -o test.s test.c
as -o test.o test.s
gcc -o test test.o
./test  # Exit code: 6 (1 + 2 + 3)

# 7 arguments (stack args)
./build/bin/zerocc -S -o test.s test.c
as -o test.o test.s
gcc -o test test.o
./test  # Exit code: 28 (1+2+3+4+5+6+7)
```

---

## Lessons Learned

1. **Register Mapping**: Always map virtual temps to physical registers in assembly emission
2. **Stack Alignment**: JIT and assembly modes may have different alignment requirements
3. **Function Calls**: System V AMD64 ABI requires careful argument register assignment
4. **Testing**: Manual testing with simple programs catches bugs faster than unit tests

---

## Next Work

### Sprint 10: SIMD Vectorization Support

Now that assembly emission is stable, proceed with:
- Task 71: SSE2 Instruction Lowering
- Task 72: Loop Vectorization Pass
- Task 73: Auto-Vectorization Heuristics

---

## References

- [System V AMD64 ABI](https://refspecs.linuxbase.org/elf/x86-64-abi-1.0.pdf)
- [GNU Assembler Syntax](https://sourceware.org/binutils/docs/as.html)
- [Sprint 10.5 Documentation](docs/CHANGELOG.md#sprint-105)
