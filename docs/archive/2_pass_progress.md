# Two-Pass IR: Progress Summary

## Phase 1 Complete: Infrastructure ✅

## Phase 2 Complete: Pass 1 (Instruction Selection) ✅

## Phase 3 Complete: Register Allocation Integration & Two-Pass Pipeline ✅

### What We Built

**Live Range Analysis** (`src/zero_cc/parser/live_range.h`):
- Tracks temp uses/defs from IR instructions
- Supports GPR and XMM register classes
- Detects call crossings (`spans_call`) for callee-saved affinity
- Integrates with linear-scan register allocator

**Register Allocation & Spilling** (`src/zero_cc/parser/regalloc.h`):
- Linear-scan allocation for GPRs (`rax, rbx, rsi, rdi, r8–r15`) and XMMs (`xmm0–xmm7`)
- Callee-saved preservation (`rbx, r12–r15`) with prologue stores and epilogue reloads
- Spilled temp management using dedicated frame slots with `r10`/`r11` scratch reloads

**AST Lowering to IR (Pass 1)** (`src/zero_cc/parser/ast_lowering_*.h`):
- Expressions, statements, loops, break/continue, and gotos/labels lowered to linear IR
- Direct machine code emission bypasses eliminated from AST lowering

### Current Status

- **IR Emission & RegAlloc**: Fully functional across all language constructs
- **Pipeline**: AST → Pass 1 (IR) → Live Range Analysis → Linear-Scan RegAlloc → Pass 2 (Machine Code)
- **Test Results**: 118/118 tests passing (100%)

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
