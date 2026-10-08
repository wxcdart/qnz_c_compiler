# Parser Module - AST Lowering & Code Emission

## Overview

This module handles the transformation from AST to executable code. It includes:
- AST statement lowering (`ast_lowering_stmt.c`)
- AST expression lowering (`ast_lowering_expr.c`)
- Assembly emission (`asm_emitter.c`)
- Machine code emission (`ast_lowering_mc.c`)
- Optimization passes (`opt_pass.c`, `opt_constant_fold.c`, etc.)

## Architecture

### Two-Path Code Generation

The module supports two output paths from the same IR:

```
AST → Semantic Analysis → IR → ──┬──► asm_emitter.c → GAS text (.s) → as → ELF
                                 │
                                 └──► ast_lowering_mc.c → Binary ELF (direct)
```

**Assembly Path** (`-S` flag):
- Human-readable text output
- Useful for debugging code generation
- Requires external assembler (`as`)

**Machine Code Path** (default):
- Binary output for direct JIT/linking
- Faster (no external tool needed)
- Production use

### File Organization

| File | Purpose | Lines |
|------|---------|-------|
| `ast_lowering.c` | Coordinator (includes modules below) | 150 |
| `ast_lowering_internal.h` | Internal declarations | 50 |
| `ast_lowering_stmt.c` | Statement lowering (if, while, for, etc.) | 350 |
| `ast_lowering_expr.c` | Expression lowering (arithmetic, calls, etc.) | 1350 |
| `asm_emitter.c` | IR → GAS assembly emission | 430 |
| `asm_emitter.h` | Assembly emitter interface | 80 |
| `ast_lowering_mc.c` | IR → Machine code emission | 1100 |
| `ir.h` | IR instruction definitions | 700 |
| `regalloc.h` | Register allocation | 200 |
| `live_range.h` | Live range analysis | 150 |

### Build Pattern

Files use `#include` pattern instead of separate compilation:

```c
// In ast_lowering.c
#include "ast_lowering_internal.h"
#include "asm_emitter.c"        // Assembly emission
#include "ast_lowering_stmt.c"  // Statement lowering
#include "ast_lowering_expr.c"  // Expression lowering
#include "ast_lowering_mc.c"    // Machine code emission
#include "opt_pass.c"           // Optimization passes
```

**Why?** Keeps all lowering code in one compilation unit:
- Simpler dependency management
- No multiple definition errors
- Faster incremental builds

## Assembly Emitter (`asm_emitter.c`)

### Purpose

Translates IR instructions to GNU Assembler (GAS) syntax text output.

### Interface

```c
// Main entry point
void emit_ir_to_asm(ZeroCCCompiler *c, int32_t spill_base_offset);

// Individual instruction emission
void asm_emit_instruction(ZeroCCCompiler *c, const Instr *instr, size_t idx);

// Helper functions
void asm_emit_prologue(FILE *out, size_t frame_size);
void asm_emit_epilogue(FILE *out);
void asm_emit_label(FILE *out, const char *label);
```

### ISA-Agnostic Design

The emitter uses the backend's register information:

```c
// Get register name from backend
const char *reg_name = BE(c)->regs->int_arg_regs[seq_idx];
fprintf(out, "\tmovq %%%s, %d(%%rbp)\n", reg_name, offset);
```

This ensures:
- Correct register names for each ISA
- Easy addition of new ISAs
- No hardcoded register names

### Example Output

```asm
# 0: OP_PROLOGUE
.globl main
main:
    push %rbp
    mov %rsp, %rbp
    sub $16, %rsp
    
# 1: OP_PARAM_SAVE_INT
    movq %rdi, -8(%rbp)
    
# 2: OP_MOV_IMM
    movq $42, %rax
    
# 3: OP_MOV_MEM
    movq %rax, -16(%rbp)
    
# 4: OP_LABEL
.L0:
    # loop body...
```

### Supported Opcodes

**Data Movement:**
- `OP_MOV_IMM`, `OP_MOV_REG`, `OP_MOV_MEM`, `OP_LEA`
- `OP_DEREF`, `OP_LOAD_32`, `OP_STORE_32`, `OP_STORE_64`

**Arithmetic:**
- `OP_ADD`, `OP_SUB`, `OP_MUL`, `OP_DIV`, `OP_MOD`
- `OP_NEG`, `OP_AND`, `OP_OR`, `OP_XOR`, `OP_NOT`
- `OP_SHL`, `OP_SHR`, `OP_SAR`

**Control Flow:**
- `OP_JMP`, `OP_JE`, `OP_JNE`, `OP_JL`, `OP_JLE`, `OP_JG`
- `OP_JCOND`, `OP_LABEL`

**Function:**
- `OP_CALL`, `OP_CALL_INDIRECT`, `OP_RET`
- `OP_PARAM_SAVE_INT`, `OP_PARAM_SAVE_FP`
- `OP_PROLOGUE`, `OP_EPILOGUE`

**Memory:**
- `OP_VLA_ALLOC`, `OP_CALLEE_SAVE_STORES`, `OP_CALLEE_SAVE_LOADS`

**Floating Point:**
- `OP_FADD`, `OP_FSUB`, `OP_FMUL`, `OP_FDIV`
- `OP_CVTSI2SD`, `OP_CVTTSD2SI`

**SIMD:**
- `OP_VADDSS`, `OP_VSUBSS`, `OP_VMULSS`, `OP_VDIVSS`
- `OP_VMOV`, `OP_VLOAD`, `OP_VSTORE`

## Machine Code Emitter (`ast_lowering_mc.c`)

### Purpose

Translates IR instructions to binary machine code and writes ELF object format.

### Key Features

- Direct binary emission (no external tools)
- ELF object file generation
- JIT compilation support
- AOT compilation support

### ISA Backend Integration

Uses backend function pointers for ISA-specific emission:

```c
// Generic IR opcode
case OP_ADD:
    BE(c)->emit_add(c, phys_dst, phys_src1);
    break;

// Backend implements ISA-specific encoding
// x86_64.c: x86_emit_add()
// aarch64.c: aarch64_emit_add()
// riscv.c: riscv64_emit_add()
```

## Testing

### Assembly Emitter Tests

```bash
# Generate assembly
./build/bin/zerocc -S -o test.s input.c

# Validate output
python3 tools/asm_emitter_validator.py --input test.s

# Check instruction coverage
python3 tools/asm_instruction_checker.py --input test.s
```

### All Tests Pass

```
Results: 118/118 passed, 0 failed in 0.29s
```

## Debugging

### Common Issues

**1. Assembly output is binary instead of text:**
- Ensure `-S` flag is set
- Check `c->emit_asm` is true

**2. Wrong register names:**
- Check backend initialization (`x86_64_init()`, etc.)
- Verify `BE(c)->regs->int_regs[]` is populated

**3. Label naming conflicts:**
- Labels use `.L%zu` format (`.L0:`, `.L1:`, etc.)
- Ensure unique label indices

### IR Dumps

```bash
# See IR before emission
./build/bin/zerocc -dump-ir input.c

# See IR with register allocation
./build/bin/zerocc -dump-regalloc input.c
```

## Future Work

1. **More SIMD Instructions**: Implement remaining SIMD opcodes
2. **Assembly Optimization**: Add optimization passes for size/speed
3. **Named Labels**: Use function-scoped label names instead of `.L%zu`
4. **Cross-ISA Testing**: Compare assembly output across ISAs

## References

- [ASM_EMITTER_TESTING.md](../../docs/ASM_EMITTER_TESTING.md) - Testing guide
- [ASM_EMITTER_REFACTORING.md](../../ASM_EMITTER_REFACTORING.md) - Refactoring summary
- [CHANGELOG.md](../../docs/CHANGELOG.md) - Version history
