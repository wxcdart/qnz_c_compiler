# Epilogue Refactor Summary

## Changes Made

### 1. Added ISA-Specific Assembly Emission Functions

Added two new function pointers to `ZeroCCISABackend` in `isa_backend.h`:
- `void (*emit_prologue_asm)(ZeroCCCompiler *c, size_t stack_size)`
- `void (*emit_epilogue_asm)(ZeroCCCompiler *c)`

### 2. Implemented for All ISAs

**x86-64** (`x86_64.c`):
```c
static void x86_emit_prologue_asm(ZeroCCCompiler *c, size_t stack_size) {
    fprintf(out, "\tpush %s\n", rbp);
    fprintf(out, "\tmov %s, %s\n", rsp, rbp);
    fprintf(out, "\tsub $%zu, %s\n", stack_size, rsp);
}

static void x86_emit_epilogue_asm(ZeroCCCompiler *c) {
    fprintf(out, "\tleave\n");
    fprintf(out, "\tret\n");
}
```

**AArch64** (`aarch64.c`):
```c
static void aarch64_emit_prologue_asm(ZeroCCCompiler *c, size_t stack_size) {
    fprintf(out, "\tstp x29, x30, [sp, #%zu]!\n", stack_size);
    fprintf(out, "\tmov x29, sp\n");
}

static void aarch64_emit_epilogue_asm(ZeroCCCompiler *c) {
    fprintf(out, "\tldp x29, x30, [sp]\n");
    fprintf(out, "\tadd sp, sp, #%zu\n", stack_size);
    fprintf(out, "\tret\n");
}
```

**RISC-V** (`riscv.c`):
```c
static void riscv64_emit_prologue_asm(ZeroCCCompiler *c, size_t stack_size) {
    fprintf(out, "\taddi sp, sp, -%zu\n", stack_size);
    fprintf(out, "\tsd ra, 0(sp)\n");
    fprintf(out, "\tsd s0, 8(sp)\n");
    fprintf(out, "\taddi s0, sp, %zu\n", stack_size);
}

static void riscv64_emit_epilogue_asm(ZeroCCCompiler *c) {
    fprintf(out, "\tld ra, 0(sp)\n");
    fprintf(out, "\tld s0, 8(sp)\n");
    fprintf(out, "\taddi sp, sp, %zu\n", stack_size);
    fprintf(out, "\tret\n");
}
```

### 3. Updated Assembly Emitter

Modified `asm_emitter.c` to use ISA-specific functions:
- `emit_prologue_asm()` now calls `BE(c)->emit_prologue_asm()`
- `emit_ir_to_asm()` now calls `BE(c)->emit_epilogue_asm()` instead of hardcoded `asm_emit_epilogue()`

## Benefits

### 1. Consistency
- Prologue and epilogue now follow the same ISA-specific pattern
- Both use backend function pointers for ISA-specific emission
- Consistent with machine code emitter design

### 2. ISA Support
- x86-64: `push rbp`, `mov rsp, rbp`, `sub $frame_size, rsp` / `leave`, `ret`
- AArch64: `stp x29, x30, [sp, #-frame_size]!`, `mov x29, sp` / `ldp x29, x30, [sp]`, `add sp, sp, #frame_size`, `ret`
- RISC-V: `addi sp, sp, -frame_size`, `sd ra, 0(sp)`, `sd s0, 8(sp)`, `addi s0, sp, frame_size` / `ld ra, 0(sp)`, `ld s0, 8(sp)`, `addi sp, sp, frame_size`, `ret`

### 3. Maintainability
- ISA-specific code is isolated in backend files
- Easy to add new ISA support
- No hardcoded assembly in generic emitter

## Testing

- All 118/118 tests pass
- Assembly output verified for x86-64
- Assembly can be compiled and linked successfully

## Files Modified

1. `src/zero_cc/isa/isa_backend.h` - Added function pointers
2. `src/zero_cc/isa/x86_64.c` - Implemented x86-64 functions
3. `src/zero_cc/isa/aarch64.c` - Implemented AArch64 functions
4. `src/zero_cc/isa/riscv.c` - Implemented RISC-V functions
5. `src/zero_cc/parser/asm_emitter.c` - Updated to use ISA-specific functions
