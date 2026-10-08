# Assembly Emitter Testing Guide

## Overview

The assembly emitter (`src/zero_cc/parser/asm_emitter.c`) translates IR instructions to GNU assembler syntax. This document describes how to test the assembly emitter.

## Testing the `-S` Flag

### Basic Test

```bash
# Compile to assembly (text output)
./build/bin/zerocc -S -o /tmp/test.s test/jit/vla_test.c

# View the generated assembly
cat /tmp/test.s

# Assemble and link to verify correctness
gcc -o /tmp/test /tmp/test.s
/tmp/test
```

### Test Coverage

The assembly emitter should handle all IR opcodes:

1. **Data Movement**
   - `OP_MOV_IMM` - Load immediate values
   - `OP_MOV_REG` - Register to register moves
   - `OP_MOV_MEM` - Store to stack
   - `OP_LEA` - Load effective address
   - `OP_DEREF` - Load from pointer
   - `OP_STORE_32/64` - Store to memory

2. **Arithmetic**
   - `OP_ADD`, `OP_SUB`, `OP_MUL`, `OP_DIV`, `OP_MOD`
   - `OP_NEG`, `OP_INC`, `OP_DEC`
   - `OP_AND`, `OP_OR`, `OP_XOR`, `OP_NOT`
   - `OP_SHL`, `OP_SHR`, `OP_SAR`

3. **Comparison**
   - `OP_CMP` - Compare values
   - `OP_COND_SET` - Set condition code

4. **Control Flow**
   - `OP_JMP`, `OP_JE`, `OP_JNE`, `OP_JL`, `OP_JLE`, `OP_JG`
   - `OP_JCOND` - Conditional jump
   - `OP_LABEL` - Label definition

5. **Function**
   - `OP_CALL`, `OP_CALL_INDIRECT`
   - `OP_RET`
   - `OP_PARAM_SAVE_INT`, `OP_PARAM_SAVE_FP`
   - `OP_PROLOGUE`, `OP_EPILOGUE`

6. **Memory**
   - `OP_VLA_ALLOC` - Variable length array allocation
   - `OP_CALLEE_SAVE_STORES`, `OP_CALLEE_SAVE_LOADS`

7. **Floating Point**
   - `OP_FADD`, `OP_FSUB`, `OP_FMUL`, `OP_FDIV`
   - `OP_CVTSI2SD`, `OP_CVTTSD2SI`

8. **SIMD**
   - `OP_VADDSS`, `OP_VSUBSS`, `OP_VMULSS`, `OP_VDIVSS`
   - `OP_VMOV`, `OP_VLOAD`, `OP_VSTORE`

## Automated Testing

### Using Python Tools

The following tools are available in `tools/`:

1. **asm_emitter_validator.py** - Validates `-S` outputs text assembly
   ```bash
   python3 tools/asm_emitter_validator.py --input test/jit/vla_test.c
   ```

2. **asm_instruction_checker.py** - Checks SSE2/SIMD instruction coverage
   ```bash
   python3 tools/asm_instruction_checker.py --input /tmp/test.s
   ```

3. **asm_reference_generator.py** - Generates reference patterns
   ```bash
   python3 tools/asm_reference_generator.py --opcode OP_ADD
   ```

## Test Cases

### Test 1: Simple Function

```c
int add(int a, int b) {
    return a + b;
}
```

Expected assembly:
```asm
.globl add
add:
    push %rbp
    mov %rsp, %rbp
    sub $16, %rsp
    mov %edi, -4(%rbp)
    mov %esi, -8(%rbp)
    mov -4(%rbp), %rax
    add -8(%rbp), %rax
    leave
    ret
```

### Test 2: VLA Test

```c
int vla_test(int n) {
    int arr[n];
    arr[0] = 42;
    return arr[0];
}
```

Expected assembly includes:
- Dynamic stack allocation with `sub` and `and`
- Store to VLA pointer
- Load from VLA pointer

### Test 3: Conditional Jumps

```c
int max(int a, int b) {
    return a > b ? a : b;
}
```

Expected assembly includes:
- `cmp` instruction
- `jg` or `jle` conditional jump
- Label definitions (`.L0:`, `.L1:`, etc.)

## ISA-Specific Testing

The assembly emitter uses the backend's register information:

### x86-64

- Integer registers: `%rax`, `%rbx`, `%rcx`, `%rdx`, `%rsi`, `%rdi`, `%r8`-`%r15`
- FP registers: `%xmm0`-`%xmm15`
- Argument registers: `%rdi`, `%rsi`, `%rdx`, `%rcx`, `%r8`, `%r9`

### aarch64 (Future)

- Integer registers: `%x0`-`%x30`
- FP registers: `%v0`-`%v31`
- Argument registers: `%x0`-`%x7`

### riscv64 (Future)

- Integer registers: `%a0`-`%a7`, `%t0`-`%t6`, etc.
- FP registers: `%fa0`-`%fa7`, `%ft0`-`%ft11`, etc.

## Known Issues

1. **Label Naming**: Labels use `.L%zu` format. Future work could use named labels.
2. **SIMD Coverage**: Only basic SIMD instructions are implemented.
3. **Optimization**: Assembly is not optimized for size or speed.

## Future Improvements

1. Add systematic unit tests for each opcode
2. Add ISA-specific test suites
3. Implement assembly optimization passes
4. Add support for more SIMD instructions
5. Add cross-ISA comparison tests
