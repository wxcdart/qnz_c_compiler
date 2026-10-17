# Phase 2 Complete: Flag Registers Added

## Summary

Phase 2 of the IR flag refactoring plan is complete. Explicit flag registers (`flags_in` and `flags_out`) have been added to the IR instruction structure.

## Changes Made

### 1. IR Layer (ir.h)
- Added `REG_FLAGS` constant (255) as special flag register
- Updated `Instr` structure from 16 to 18 bytes:
  - Added `flags_out` field (flags output register)
  - Added `flags_in` field (flags input register)

### 2. IR Emission Functions (ir_emit_ops.h, ir.c)
Updated all IR emission functions to set `flags_in`/`flags_out`:

**Flag-setting operations:**
- `ir_emit_cmp()`: `flags_out = REG_FLAGS`
- `ir_emit_cmp_zero()`: `flags_out = REG_FLAGS`

**Flag-reading operations:**
- `ir_emit_cond_set()`: `flags_in = REG_FLAGS`
- `ir_emit_jcond()`: `flags_in = REG_FLAGS`

**Non-flag operations:**
- All other ops: `flags_out = 0xFF`, `flags_in = 0xFF`

### 3. Verification Tools
- Added `tools/flag_register_migration.py` - migration planning tool
- Added `tools/verify_flag_registers.py` - verification tool

## Current Status

| Item | Status |
|------|--------|
| REG_FLAGS constant | ✓ Defined (255) |
| Instr structure updated | ✓ 16 -> 18 bytes |
| Flag setters updated | ✓ OP_CMP, OP_CMP_ZERO |
| Flag readers updated | ✓ OP_COND_SET, OP_JCOND |
| All other ops updated | ✓ flags_in/flags_out = 0xFF |
| All 118 JIT tests | ✓ Pass |

## Impact Analysis

### Memory Impact
- Instr size: 16 -> 18 bytes (+12.5%)
- IR buffer memory: +12.5%
- Cache impact: Minimal (still cache-line friendly)

### Correctness
- All 118 JIT tests pass
- Backward compatible (old code still works)
- Explicit flag dependency tracking enabled

## Next Steps: Flag Optimizations

The infrastructure for flag optimizations is now in place. Future work can add:

### Flag CSE (Common Subexpression Elimination)
```c
// Before:
CMP a, b
COND_SET x, EQ
COND_SET y, NE
CMP a, b  // Redundant!
COND_SET z, GT

// After (with flag CSE):
CMP a, b
COND_SET x, EQ
COND_SET y, NE
COND_SET z, GT  // Reuses flags from first CMP
```

### Flag DCE (Dead Code Elimination)
```c
// Before:
CMP a, b  // Flags never used!
ADD x, y

// After (with flag DCE):
ADD x, y
```

## Files Modified

```
src/zero_cc/parser/ir.h              (+3 lines: REG_FLAGS, flags_in, flags_out)
src/zero_cc/parser/ir.c              (+6 lines: updated Instr initializations)
src/zero_cc/parser/ir_emit_ops.h     (+28 lines: updated all emission functions)
tools/flag_register_migration.py     (new: migration planning tool)
tools/verify_flag_registers.py       (new: verification tool)
```

Total: ~37 lines added/modified in core code

## Testing

All 118 JIT tests pass:
```
Results: 118/118 passed, 0 failed in 0.30s
```

Verification tool confirms all changes:
```
[✓] REG_FLAGS constant: REG_FLAGS = 255
[✓] Instr structure: flags_in and flags_out fields present
[✓] Helper functions: All helper functions updated correctly
[✓] Backend updates: Backend check complete
```

## Conclusion

Phase 2 is complete. The IR now has explicit flag dependency tracking through `flags_in` and `flags_out` fields. This enables future flag optimizations (CSE, DCE) while maintaining backward compatibility and passing all tests.

The flag refactoring plan is now **COMPLETE**:
- ✓ Phase 1: OP_JCOND added
- ✓ Phase 2: Flag registers added

The IR is now fully ISA-agnostic with explicit flag dependency tracking.
