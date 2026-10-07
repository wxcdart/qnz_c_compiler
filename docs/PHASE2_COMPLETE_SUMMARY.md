# Phase 2 Complete: ISA-Agnostic IR Refactoring

## Summary

**Status:** ✓ Phase 2A Complete | ⏳ Phase 2B Planned

**Test Results:** 118/118 JIT tests passing

## What Was Done

### Phase 1: Calling Convention (Commit: 3d8c0d4)

**Problem:** Hardcoded "6" for register args (x86-64 only)

**Fix:** Use backend calling convention tables

| File | Changes | Impact |
|------|---------|--------|
| `ast_lowering.c` | 2 lines | FP/int param save |
| `ast_lowering_mc.c` | 4 lines | Stack args |
| `ast_lowering_expr.c` | 2 lines | FP arg spill |

**Result:** AArch64/RISC-V now get 8 register args (was 6)

### Phase 2A: Opcode Renaming (Commit: dfb8e0e)

**Problem:** x86-specific register names in opcodes

**Fix:** Rename to ISA-neutral names

| Old Opcode | New Opcode | Purpose |
|------------|------------|---------|
| `OP_MOV_RAX_MEM` | `OP_MOV` | Load from stack |
| `OP_MOV_MEM_RAX` | `OP_MOV_MEM` | Store to stack |
| `OP_MOV_RAX_RBX` | `OP_MOV_REG` | Register move |
| `OP_LEA_RAX_MEM` | `OP_LEA` | Load effective address |
| `OP_PUSH_RAX` | `OP_PUSH` | Push register |
| `OP_POP_RAX` | `OP_POP` | Pop register |
| `OP_PUSH_RBX` | (merged) | Push register |
| `OP_POP_RBX` | (merged) | Pop register |

**Files Changed:**
- `ir.h` - Opcode definitions
- `ast_lowering_mc.c` - Lowering switch cases
- `ir_emit_ops.h` - Helper macros
- `opt_cse.c` - CSE optimization
- `opt_dce.c` - DCE optimization
- `ir_opcodes_test.c` - Test assertions

**Result:** 8 x86-specific opcodes → ISA-neutral

## What Remains (Phase 2B)

### 8 Condition Code Opcodes

| Opcode | x86 | AArch64 | RISC-V |
|--------|-----|---------|--------|
| `OP_TEST` | `test` | N/A | N/A |
| `OP_SETL` | `setl` | `cset lt` | `slt` |
| `OP_SETLE` | `setle` | `cset le` | `slt` |
| `OP_SETG` | `setg` | `cset gt` | `slt` |
| `OP_SETGE` | `setge` | `cset ge` | `slt` |
| `OP_SET_E` | `sete` | `cset eq` | `beq` |
| `OP_SET_NE` | `setne` | `cset ne` | `bne` |
| `OP_LNOT` | `test+sete` | `cset eq` | `seqz` |

### Recommended Approach: CMP_BRANCH

**New opcode:** `OP_CMP_BRANCH` with condition code in `imm` field

**Benefits:**
- ISA-neutral IR
- Clean backend separation
- Achieves 100% goal

**Timeline:** 4-5 days

**Risk:** Medium (new opcode, lowering changes)

**Fallback:** Keep SETcc but genericize backend functions

## Current State

```
IR Layer:
  ✓ Register-specific opcodes: REMOVED
  ⏳ Condition code opcodes: Still x86-specific (8 opcodes)
  
Backend Layer:
  ✓ Calling convention tables: ISA-agnostic
  ✓ Backend functions: Generic names
  
Tests:
  ✓ 118/118 JIT tests passing
```

## Progress Timeline

| Phase | Duration | Status | Commit |
|-------|----------|--------|--------|
| Phase 0: Tooling | 1 day | ✓ Complete | - |
| Phase 1: Calling Convention | 1 day | ✓ Complete | 3d8c0d4 |
| Phase 2A: Opcode Renaming | 1 day | ✓ Complete | dfb8e0e |
| Phase 2B: Condition Codes | 4-5 days | ⏳ Planned | - |
| Phase 3: AArch64 Backend | 2-3 weeks | Pending | - |
| Phase 4: SIMD | 3-4 weeks | Pending | - |

**Total to 100% ISA-neutral:** ~6 weeks

## Next Steps

1. **Review Phase 2B plan** (see `docs/PHASE2B_100_PERCENT_PLAN.md`)
2. **Approve Approach C** (CMP_BRANCH) or choose alternative
3. **Implement Phase 2B** (4-5 days)
4. **Test thoroughly** (118/118 must pass)
5. **Proceed to Phase 3** (AArch64 backend)

## Decision Needed

**Should we proceed with Phase 2B?**

Options:
- **Approach C (Recommended):** Add CMP_BRANCH opcode, 4-5 days, clean design
- **Minimal Approach:** Keep SETcc, genericize backend, 2-3 days, less clean
- **Defer:** Move to AArch64 backend work first

---

*Last updated: 2026-10-06*
*See also: docs/PHASE2B_100_PERCENT_PLAN.md, docs/REFACTORING_PRIORITY.md*
