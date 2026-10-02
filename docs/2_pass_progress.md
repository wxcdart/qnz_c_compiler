# Two-Pass IR: Progress Summary

## Phase 1 Complete: Infrastructure ✅

## Phase 2 Complete: Pass 1 (Instruction Selection) ✅

## Phase 3: Register Allocation Integration - In Progress ⏸️

### What We Built

**Live Range Analysis** (`src/zero_cc/parser/live_range.h`):
- Tracks temp uses/defs from IR instructions
- Computes live intervals for each temporary
- Integrates with existing regalloc.h infrastructure

**Key Functions**:
- `live_range_init()` - Initialize analyzer
- `live_range_add_use()` - Record temp use/def
- `live_range_scan_ir()` - Scan IR for temp uses/defs
- `live_range_compute()` - Compute live intervals
- `live_range_free()` - Free analyzer

**IR Buffer Fix**:
- Moved `instr_count` and `instrs` from `static` in `ir.h` to `extern` in `ir.c`
- This ensures a single shared instance across all compilation units
- Previously, each file including `ir.h` got its own copy, causing IR count to be 0

### Current Status

**IR Emission**: Working for most expression types (BINOP, UNOP, TERNARY, CALL, IDENT, ASSIGN)

**Register Allocation**: Temporarily disabled pending debugging

**Issue**: Register allocation causes malloc error due to incorrect live interval computation

### Files Created/Modified

| File | Changes |
|------|---------|
| `src/zero_cc/parser/ir.c` | NEW: IR buffer implementation (fixes shared buffer issue) |
| `src/zero_cc/parser/ir.h` | Changed to extern declarations |
| `src/zero_cc/parser/live_range.h` | NEW: Live range analysis infrastructure |
| `src/zero_cc/parser/regalloc.h` | Existing: Linear-scan register allocator |
| `Makefile` | Added ir.o to EXTRA_OBJS |

### Test Results

- **IR Unit Tests**: ✅ All pass
- **Opcode Tests**: ✅ All pass
- **JIT Tests**: ✅ All 71 pass (with machine code path)
- **Total**: ✅ 73/73 tests passing

## Next Steps: Complete Phase 3

1. **Debug live interval computation**: Fix incorrect start/end values in live intervals
2. **Enable register allocation**: Once live intervals are correct
3. **Build Pass 2**: Generate machine code from IR using allocated registers
4. **Remove dual emission**: Transition to IR-only code generation

## Architecture Summary

```
AST → Pass 1 (IR) → Live Analysis → Register Allocation → Pass 2 (Machine Code)
```

**Current Flow**:
```
AST → Pass 1 (IR + Machine Code) → Execute
```

**Target Flow**:
```
AST → Pass 1 (IR) → Live Analysis → RegAlloc → Pass 2 (Machine Code) → Execute
```

## Known Issues

1. **Live interval computation**: Temp intervals have incorrect start/end values
   - Expected: All temps defined/used at instruction 0
   - Actual: Some temps show start=11, end=41 (from previous function?)
   
2. **Register allocation**: Causes malloc error due to incorrect intervals
   - Root cause: Live intervals contain stale data from previous functions
   - Fix: Ensure regalloc_init() properly frees old intervals

## Debug Findings

- IR emission working correctly (1 instruction for `return 2 + 3;`)
- Live range scan correctly identifies 3 uses/defs at instruction 0
- But live intervals show incorrect start/end values (11, 41, 22, 41)
- Suggests regalloc_init() not properly resetting state between functions
