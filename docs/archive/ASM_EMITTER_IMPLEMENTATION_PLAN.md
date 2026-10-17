# Assembly Emitter (-S Flag) Implementation Plan

## Current Status
The `-S` flag is parsed but **not implemented**. When used, the compiler currently:
- Ignores the flag
- Falls through to default JIT execution or executable creation
- Does NOT emit text assembly

## Required Changes

### 1. Create Assembly Emitter Module (`src/zero_cc/asm_emitter.h/c`)

The assembly emitter needs to:
- Take IR or machine code as input
- Output human-readable x86-64 assembly text
- Support all instruction types (integer, FP, SIMD)
- Handle labels, sections, and symbols

**Key Functions:**
```c
void asm_emit_prologue(AsmContext *ctx, int stack_size);
void asm_emit_epilogue(AsmContext *ctx);
void asm_emit_instruction(AsmContext *ctx, Opcode op, Instr *instr);
void asm_emit_label(AsmContext *ctx, const char *name);
void asm_emit_data(AsmContext *ctx, const char *section, uint8_t *data, size_t len);
bool asm_emit_file(AsmContext *ctx, const char *output_path);
```

### 2. Wire Up `-S` Flag in `src/main.c`

Add assembly emission mode to the compilation flow:

```c
if (opts.emit_asm) {
    return compile_assembly(&opts);
} else if (opts.emit_object) {
    return compile_object(&opts);
} else if (opts.output_file) {
    return compile_and_link(&opts);
} else {
    return compile_jit(&opts);
}
```

### 3. Assembly Output Format

The emitter should produce GNU assembler format:

```assembly
    .text
    .globl main
main:
    push rbp
    mov rbp, rsp
    sub rsp, 16
    # ... instructions ...
    leave
    ret
```

## Python Tools for Debugging & Verification

### 1. `tools/asm_emitter_validator.py`
**Purpose:** Validate that `-S` outputs text assembly (not binary)
**Usage:**
```bash
python3 tools/asm_emitter_validator.py test/jit/simple_test.c
python3 tools/asm_emitter_validator.py test/bench/array_sum.c --gcc
```

**Checks:**
- Output is text, not binary ELF
- Contains valid assembly sections (.text, .data)
- Contains function declarations
- Contains instructions
- Contains labels

### 2. `tools/asm_instruction_checker.py`
**Purpose:** Check which SSE2/SIMD instructions are present in output
**Usage:**
```bash
python3 tools/asm_instruction_checker.py test/bench/vector_add.c
python3 tools/asm_instruction_checker.py test/bench/vector_add.c --full
```

**Reports:**
- Instruction coverage by category (arithmetic, move, conversion, etc.)
- XMM register usage analysis
- Potential issues (uninitialized registers)

### 3. `tools/asm_reference_generator.py`
**Purpose:** Reference for what assembly each IR opcode should produce
**Usage:**
```bash
python3 tools/asm_reference_generator.py VADDSS
python3 tools/asm_reference_generator.py FADD
python3 tools/asm_reference_generator.py  # List all
```

**Provides:**
- Expected x86-64 assembly for each IR opcode
- SIMD instruction patterns
- Control flow patterns

### 4. `tools/asm_diff.py` (existing)
**Purpose:** Compare ZeroCC assembly with GCC assembly)
**Usage:**
```bash
python3 tools/asm_diff.py test/bench/call_he heavy.c
```

## Implementation Priority

### Phase 1: Basic Assembly Emission (Week 1)
1. Create `asm_emitter.c/h` with basic integer instruction emission
2. Wire up `-S` flag to use emitter
3. Test with simple C programs
4. Validate with `asm_emitter_validator.py`

### Phase 2: Floating Point Support (Week 2)
1. Add FP instruction emission (FADD, FSUB, FMUL, FDIV)
2. Add FP load/store emission
3. Test with float/double programs
4. Validate with `asm_instruction_checker.py`

### Phase 3: SIMD/SSE2 Support (Week 3-4, Task 71)
1. Add SIMD instruction emission (VADDSS, VMULSS, etc.)
2. Add XMM register handling
3. Test with vectorizable loops
4. Validate coverage with tools

### Phase 4: Advanced Features (Week 5+)
1. RIP-relative addressing
2. String literals in .rodata
3. Optimized instruction selection
4. Compare with GCC using `asm_diff.py`

## Testing Strategy

### Unit Tests
```bash
# Test each instruction type
./build/bin/zerocc -S -O3 test/jit/arithmetic_test.c -o /tmp/test.s
python3 tools/asm_emitter_validator.py /tmp/test.s

# Test SIMD instructions
./build/bin/zerocc -S -O3 test/bench/vector_add.c -o /tmp/simd.s
python3 tools/asm_instruction_checker.py /tmp/simd.s
```

### Integration Tests
```bash
# Compile and assemble
./build/bin/zerocc -S -O3 test/jit/vla_test.c -o /tmp/vla.s
gcc -c /tmp/vla.s -o /tmp/vla.o
./tmp/vla.o  # Should work if assembly is correct
```

### Regression Tests
```bash
# Compare with GCC
python3 tools/asm_diff.py test/bench/memory_heavy.c --gcc
```

## Success Criteria

1. ✅ `-S` flag produces valid text assembly (not binary)
2. ✅ Assembly can be assembled by `gcc -c`
3. ✅ All 118 existing tests still pass
4. ✅ Assembly output is human-readable
5. ✅ SIMD instructions (Task 71) emit correct SSE2 opcodes
6. ✅ XMM register usage is correct per ABI

## Files to Create/Modify

### New Files
- `src/zero_cc/asm_emitter.h` - Assembly emitter interface
- `src/zero_cc/asm_emitter.c` - Assembly emitter implementation
- `src/zero_cc/asm_context.h` - Assembly context structure
- `docs/ASM_EMITTER_IMPLEMENTATION_PLAN.md` - This document

### Modified Files
- `src/main.c` - Add assembly emission mode
- `src/zero_cc/isa/x86_64.c` - Add assembly emission functions (Task 71)

## Python Tools Summary

| Tool | Purpose | When to Use |
|------|---------|-------------|
| `asm_emitter_validator.py` | Validate text output | After implementing `-S` |
| `asm_instruction_checker.py` | Check SIMD coverage | During SIMD implementation |
| `asm_reference_generator.py` | Reference patterns | While writing emitter code |
| `asm_diff.py` | Compare with GCC | Final validation |

## Next Steps

1. **Implement basic assembly emitter** (integer instructions only)
2. **Wire up `-S` flag** in main.c
3. **Test with `asm_emitter_validator.py`**
4. **Iterate** until text assembly is produced
5. **Add FP support** (Task 71 prep)
6. **Add SIMD support** (Task 71)
