# Assembly Emitter Refactoring Summary

## Overview

This document summarizes the refactoring work to separate the assembly emitter into its own module, improving code organization and testability.

## Changes Made

### 1. New Files Created

#### `src/zero_cc/parser/asm_emitter.h`
- Public header for assembly emission
- Clean interface with documented functions
- ISA-agnostic design using backend register info

#### `src/zero_cc/parser/asm_emitter.c`
- Complete assembly emission implementation
- Translates IR to GNU assembler syntax
- Uses `BE(c)->regs` for ISA-specific register names
- Handles all IR opcodes (data movement, arithmetic, control flow, etc.)

### 2. Modified Files

#### `src/zero_cc/parser/ast_lowering.c`
- Updated include order to include `asm_emitter.c` early
- Changed comment to reflect new module structure

#### `src/zero_cc/parser/ast_lowering_internal.h`
- Removed `emit_ir_to_asm` forward declaration (now in `asm_emitter.h`)

#### `src/zero_cc/parser/ast_lowering_mc.c`
- Removed duplicate assembly emission code
- Kept only machine code emission

#### `Makefile`
- Removed `asm_emitter.o` from `EXTRA_OBJS` (included via `#include`)

### 3. Documentation Created

#### `docs/ASM_EMITTER_TESTING.md`
- Testing guide for assembly emitter
- Test cases for different opcode categories
- ISA-specific testing information
- Known issues and future improvements

## Design Decisions

### Include Pattern

The assembly emitter uses the same `#include` pattern as other lowering modules:
```c
#include "asm_emitter.c"
```

This keeps all lowering code in a single compilation unit, avoiding:
- Multiple definition errors
- Link-time overhead
- Complex dependency management

### ISA-Agnostic Design

The assembly emitter uses the backend's register information:
```c
const char *reg_name = BE(c)->regs->int_arg_regs[seq_idx];
```

This ensures:
- Correct register names for each ISA
- Easy addition of new ISAs
- Single source of truth for register mappings

### Label Naming

Labels use `.L%zu` format (e.g., `.L0:`, `.L1:`):
```c
fprintf(out, ".L%zu:\n", instr->label_idx);
```

This matches the machine code emitter's approach and is compatible with GNU assembler.

## Benefits

### 1. Separation of Concerns

- **Machine code emission** (`ast_lowering_mc.c`): Binary ELF generation
- **Assembly emission** (`asm_emitter.c`): Text assembly generation
- Clear separation makes each module easier to understand and test

### 2. Improved Testability

- Assembly emitter can be tested independently
- Python tools (`asm_emitter_validator.py`, etc.) can validate output
- Easy to add unit tests for individual opcodes

### 3. Better Code Organization

- Each module has a single responsibility
- Clear interfaces between modules
- Easier to navigate and maintain

### 4. ISA Support

- Backend provides register information
- Easy to add new ISAs (aarch64, riscv64)
- No ISA-specific code in emitter

## Testing

### All Tests Pass

```
Results: 118/118 passed, 0 failed
```

### VLA Test Works

The VLA test (which previously had issues) now works correctly:
```
[PASS] [jit]            jit/vla_test                        (   4.7ms)
```

### Assembly Output Verified

```bash
./build/bin/zerocc -S -o /tmp/test.s test/jit/vla_test.c
# Generates valid GNU assembler syntax
```

## Known Issues

1. **Label Naming**: Uses `.L%zu` format. Could use named labels for better readability.
2. **SIMD Coverage**: Only basic SIMD instructions implemented.
3. **Optimization**: Assembly is not optimized for size or speed.

## Future Work

### 1. Systematic Testing

Add unit tests for each opcode:
```c
TEST(asm_emit_mov_imm) {
    // Test OP_MOV_IMM emission
}
```

### 2. ISA-Specific Tests

Add tests for each supported ISA:
```c
TEST(asm_emit_x86_64) {
    // Test x86-64 specific emission
}
```

### 3. Assembly Optimization

Implement optimization passes:
- Remove redundant instructions
- Optimize for size or speed
- Improve label naming

### 4. More SIMD Instructions

Implement remaining SIMD opcodes:
- `VADDPD`, `VMULPS`, `VSUBPS`, etc.
- `VSHUF`, `VANDN`, `VORPS`, etc.

## Conclusion

The assembly emitter refactoring successfully separates text assembly emission from binary machine code emission. All 118 tests pass, and the `-S` flag now works correctly. The new design is more maintainable, testable, and extensible.
