# Epilogue Analysis

## Current Implementation

The epilogue is currently implemented in `src/zero_cc/parser/asm_emitter.c`:

```c
void asm_emit_epilogue(FILE *out) {
    fprintf(out, "\tleave\n");
    fprintf(out, "\tret\n");
}
```

This is called at the end of `emit_ir_to_asm()`:
```c
/* Emit epilogue */
asm_emit_epilogue(out);
```

## Pattern Comparison: Prologue vs Epilogue

### Prologue
- **IR Instruction**: `OP_PROLOGUE` (removed in refactor)
- **Emission Pattern**: Direct emission after register allocation
- **Parameters**: Requires `frame_size`
- **ISA-Specific**: Yes (x86-64: `push rbp`, `mov rsp, rbp`, `sub $frame_size, rsp`)

### Epilogue
- **IR Instruction**: `OP_EPILOGUE` (skipped in IR loop)
- **Emission Pattern**: Direct emission after IR loop
- **Parameters**: No `frame_size` needed (for x86-64)
- **ISA-Specific**: Yes (x86-64: `leave`, `ret`)

## Analysis

### Correctness for x86-64
✅ **Correct** - The current implementation is correct for x86-64:
- `leave` instruction automatically restores `rsp` from `rbp` and pops `rbp`
- `ret` instruction returns from function
- No `frame_size` parameter needed (unlike aarch64/riscv64)

### Pattern Consistency
✅ **Consistent** - The epilogue follows the same pattern as the machine code emitter:
- Machine code: `emit_prologue_mc` → `emit_ir_to_machine_code` → `emit_leave` → `emit_ret`
- Assembly: `emit_prologue_asm` → `emit_ir_to_asm` → `asm_emit_epilogue`

Both emit the epilogue separately after the IR loop, not as part of IR processing.

### Redundant Comment
⚠️ **Minor Issue** - The `OP_EPILOGUE` case in `asm_emit_instruction` emits a comment `# EPILOGUE`:
```c
case OP_EPILOGUE:
    fprintf(out, "\t# EPILOGUE\n");
    return;
```

This is redundant since the epilogue is also emitted at the end of `emit_ir_to_asm`. However, it's harmless and may be useful for debugging.

### ISA-Agnostic Design
⚠️ **Limitation** - The epilogue is x86-64 specific:
- x86-64: `leave`, `ret`
- aarch64: `ldp x29, x30, [sp]`, `add sp, sp, #stack_size`, `ret`
- riscv64: `ld ra, 0(sp)`, `ld s0, 8(sp)`, `addi sp, sp, #stack_size`, `jalr zero, 0(ra)`

For aarch64 and riscv64 support, the epilogue would need:
1. ISA-specific emission functions
2. `frame_size` parameter (unlike x86-64)

## Recommendation

### No Immediate Refactoring Needed
The epilogue implementation is **correct for x86-64** and follows the same pattern as the machine code emitter. No immediate refactoring is needed.

### Future Enhancement
For aarch64 and riscv64 support, refactor the assembly emitter to be ISA-specific:
1. Add `emit_epilogue_asm(ZeroCCCompiler *c, size_t frame_size)` function pointer to ISA backend
2. Implement ISA-specific epilogue emission for x86-64, aarch64, riscv64
3. Call from `zero_cc_lower_ast()` with `frame_size` parameter

This is similar to the machine code emitter's approach with `emit_leave` and `emit_epilogue`.

### Optional Cleanup
Remove the redundant `# EPILOGUE` comment from `asm_emit_instruction` to avoid confusion:
```c
// Remove this case:
case OP_EPILOGUE:
    fprintf(out, "\t# EPILOGUE\n");
    return;
```

## Conclusion

**Status**: ✅ **No refactoring needed**

The epilogue implementation is correct and follows the same pattern as the machine code emitter. ISA-specific support for aarch64 and riscv64 can be added later as a future enhancement.
