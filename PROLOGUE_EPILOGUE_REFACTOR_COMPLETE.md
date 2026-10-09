# Prologue & Epilogue Refactor - Complete

## Summary

Successfully refactored both prologue and epilogue assembly emission to be ISA-specific, eliminating hardcoded assembly and ensuring consistency across all ISAs.

## Key Changes

### 1. ISA-Specific Function Pointers

Added to `ZeroCCISABackend` in `isa_backend.h`:
```c
void (*emit_prologue_asm)(ZeroCCCompiler *c, size_t stack_size);
void (*emit_epilogue_asm)(ZeroCCCompiler *c);
```

### 2. Implementation for All ISAs

| ISA | Prologue | Epilogue |
|-----|----------|----------|
| **x86-64** | `push rbp`, `mov rsp, rbp`, `sub $frame_size, rsp` | `leave`, `ret` |
| **AArch64** | `stp x29, x30, [sp, #-frame_size]!`, `mov x29, sp` | `ldp x29, x30, [sp]`, `add sp, sp, #frame_size`, `ret` |
| **RISC-V** | `addi sp, sp, -frame_size`, `sd ra, 0(sp)`, `sd s0, 8(sp)`, `addi s0, sp, frame_size` | `ld ra, 0(sp)`, `ld s0, 8(sp)`, `addi sp, sp, frame_size`, `ret` |

### 3. Design Pattern

Both prologue and epilogue now follow the same pattern:
```
IR → Register Allocation → Emit Prologue → Emit IR → Emit Epilogue
```

No two-phase patching needed - both are emitted directly after register allocation.

## Benefits

1. **Consistency**: Prologue and epilogue use the same ISA-specific design
2. **Maintainability**: ISA-specific code isolated in backend files
3. **Extensibility**: Easy to add new ISA support
4. **Correctness**: All 118/118 tests pass

## Testing

```bash
$ make test
Results: 118/118 passed, 0 failed in 0.47s

$ ./build/bin/zerocc -S -o test.s test.c
$ gcc test.s -o test
$ ./test
Exit code: 42  # Correct!
```

## Files Modified

1. `src/zero_cc/isa/isa_backend.h` - Added function pointers
2. `src/zero_cc/isa/x86_64.c` - Implemented x86-64 functions
3. `src/zero_cc/isa/aarch64.c` - Implemented AArch64 functions
4. `src/zero_cc/isa/riscv.c` - Implemented RISC-V functions
5. `src/zero_cc/parser/asm_emitter.c` - Updated to use ISA-specific functions

## Next Steps

The prologue/epilogue refactor is complete. Ready to proceed with:
- Task 71: SSE2 Instruction Lowering
- Task 72: Loop Vectorization Pass
- Task 73: Auto-Vectorization Heuristics
