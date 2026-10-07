# Phase 1 Complete: OP_JCOND Added

## Summary

Phase 1 of the IR flag refactoring plan is complete. The `OP_JCOND` opcode has been added to enable explicit flag-based conditional jumps.

## Changes Made

### 1. IR Layer (ir.h, ir.c)
- Added `OP_JCOND` opcode (after `OP_JG`, before `OP_JMP_LABEL`)
- Added `ir_emit_jcond(uint8_t temp_cond, uint16_t label_idx)` helper
- Updated opcode name function to include "JCOND"

### 2. Backend Interface (isa_backend.h)
- Added `emit_jcond(ZeroCCCompiler *c, int cond, const char *label)` to `ISABackend` struct

### 3. x86-64 Backend (x86_64.c)
- Implemented `x86_emit_jcond()` using x86 `jcc` instructions
- Condition code mapping:
  - `CC_LT=0` → `0x8C` (jl)
  - `CC_LE=1` → `0x8D` (jle)
  - `CC_GT=2` → `0x8F` (jg)
  - `CC_GE=3` → `0x8E` (jge)
  - `CC_EQ=4` → `0x84` (je)
  - `CC_NE=5` → `0x85` (jne)

### 4. IR to MC Lowering (ast_lowering_mc.c)
- Added `OP_JCOND` case to handle conditional jumps
- Uses `instr->temp_src1` as condition code

### 5. Optimization (opt_dce.c)
- Added `OP_JCOND` to list of instructions with side effects

### 6. Tests (ir_opcodes_test.c)
- Updated `OP_RET` expected value from 37 to 38

## Current Status

| Item | Status |
|------|--------|
| OP_JCOND opcode | ✓ Defined |
| ir_emit_jcond() | ✓ Implemented |
| emit_jcond() interface | ✓ Added |
| x86_emit_jcond() | ✓ Implemented |
| ast_lowering_mc.c | ✓ Updated |
| DCE optimization | ✓ Updated |
| All 118 JIT tests | ✓ Pass |

## Backward Compatibility

The old `OP_JE`, `OP_JNE`, `OP_JL`, `OP_JLE`, `OP_JG` opcodes still work.
The lowering code currently uses the old pattern:
```c
ir_emit_cmp(a, b);
ir_emit_cond_set(result, cond);
ir_emit_je(result, label);  // Old pattern
```

This is correct but not optimal. The more efficient pattern is:
```c
ir_emit_cmp(a, b);
ir_emit_jcond(cond, label);  // New pattern (not yet used by lowering)
```

## Next Steps: Phase 2 (Flag Register)

Phase 2 will add explicit flag registers to the IR:
1. Define `REG_FLAGS = 255` as special flag register
2. Add `flags_in` and `flags_out` fields to `Instr` structure
3. Update `OP_CMP` to set `flags_out = REG_FLAGS`
4. Update `OP_COND_SET` and `OP_JCOND` to set `flags_in = REG_FLAGS`
5. Implement flag CSE and DCE optimizations

Estimated effort: 3-5 days
Risk level: MEDIUM (Instr size changes from 16 to 18 bytes)

## Files Modified

```
src/zero_cc/parser/ir.h              (+4 lines)
src/zero_cc/parser/ir.c              (+5 lines)
src/zero_cc/isa/isa_backend.h        (+1 line)
src/zero_cc/isa/x86_64.c             (+33 lines)
src/zero_cc/parser/ast_lowering_mc.c (+3 lines)
src/zero_cc/parser/opt_dce.c         (+1 line)
test/unit/ir_opcodes_test.c          (1 line updated)
```

Total: ~48 lines added/modified

## Testing

All 118 JIT tests pass:
```
Results: 118/118 passed, 0 failed in 0.32s
```

## Conclusion

Phase 1 is complete and ready for review. The `OP_JCOND` opcode provides the infrastructure for explicit flag-based conditional jumps. Phase 2 can proceed to add the flag register infrastructure.
