# ISA-Agnostic IR Refactoring Summary

## Current State Assessment

### Tooling Created

| Tool | Purpose | Location |
|------|---------|----------|
| `ir_analyzer.py` | Detect x86-64 assumptions in IR | `tools/ir_analyzer.py` |
| `backend_coverage.py` | Track opcode implementation | `tools/backend_coverage.py` |
| `isa_diff.py` | Compare codegen across ISAs | `tools/isa_diff.py` |
| `isa_readiness.py` | Quick readiness summary | `tools/isa_readiness.py` |

### Analysis Results

```
IR Layer (ir.h):
  - 81 total opcodes
  - 8 x86-specific opcodes detected:
    * OP_PUSH_RAX, OP_POP_RAX, OP_PUSH_RBX, OP_POP_RBX
    * OP_LEA_RAX_MEM, OP_MOV_RAX_MEM, OP_MOV_RAX_RBX, OP_MOV_MEM_RAX
    * OP_TEST, OP_SETL, OP_SETLE, OP_SETG, OP_SETGE, OP_SET_E, OP_SET_NE

Backend Functions:
  - x86_64: 79 emit_* functions
  - aarch64: 75 emit_* functions
  - riscv64: 75 emit_* functions

Hardcoded Values:
  - "6" for register args (x86-64 System V) - should use backend table
  - RAX/RBX/RBP/RSP references in lowering code
```

## Why ISA-Agnostic IR First?

| Reason | Impact |
|--------|--------|
| **Avoid rework** | Adding SIMD now = more x86 hardcoding = refactor later anyway |
| **Cleaner SIMD** | ISA-neutral IR allows SIMD opcodes that work across x86/AArch64/RISC-V |
| **Lower risk** | Quick wins fix critical issues with minimal changes |
| **Core goal** | Multi-ISA is a primary goal, not an afterthought |

## Refactoring Plan

### Phase 0: Tooling (DONE) ✓

- [x] IR analyzer (`ir_analyzer.py`)
- [x] Backend coverage tracker (`backend_coverage.py`)
- [x] Cross-ISA diff tool (`isa_diff.py`)
- [x] Readiness summary (`isa_readiness.py`)

### Phase 1: Quick Wins (1 week)

| Day | Task | Files | Risk |
|-----|------|-------|------|
| 1 | Fix hardcoded "6" for args | `ast_lowering_mc.c` | Low |
| 2 | Replace OP_PUSH_RAX/POP_RAX | `ir.h`, `ast_lowering_mc.c` | Low |
| 3 | Fix OP_DIV (remove OP_CQO) | `ir.h`, `ast_lowering_mc.c` | Low |
| 4 | Replace OP_LEA_RAX_MEM | `ir.h`, `ast_lowering_expr.c` | Medium |
| 5 | Test & fix regressions | All | - |

### Phase 2: Core Refactoring (2 weeks)

| Week | Task | Files | Risk |
|------|------|-------|------|
| 2 | Add IRMemMode abstraction | `ir.h`, `ast_lowering_expr.c` | Medium |
| 2 | Replace SETCC with CMP_BRANCH | `ir.h`, `ast_lowering_stmt.c` | Medium |
| 3 | Add OP_MOV_LARGE_IMM | `ir.h`, all backends | Medium |
| 3 | Update x86_64.c to new IR | `x86_64.c` | High |

### Phase 3: AArch64 Backend (2-3 weeks)

- Fix prologue/epilogue encoding
- Complete missing functions
- Test with QEMU

### Phase 4: SIMD (3-4 weeks)

- Add SIMD IR opcodes (ISA-neutral)
- x86-64 SSE2/AVX implementation
- AArch64 NEON (future)

## Quick Wins Implementation

### 1. Fix Hardcoded "6" for Register Args

```c
/* Before */
int n_stack = (arg_count > 6) ? arg_count - 6 : 0;

/* After */
int int_arg_count = BE(c)->conv->int_arg_reg_count;
int n_stack = (arg_count > int_arg_count) ? arg_count - int_arg_count : 0;
```

**Impact:** AArch64/RISC-V get correct 8 register args instead of 6

### 2. Replace OP_PUSH_RAX/POP_RAX

```c
/* Before */
case OP_PUSH_RAX: BE(c)->emit_push_reg(c, 0); break;
case OP_POP_RAX:  BE(c)->emit_pop_reg(c, 0); break;

/* After */
case OP_PUSH: BE(c)->emit_push_reg(c, phys_src1); break;
case OP_POP:  BE(c)->emit_pop_reg(c, phys_dest); break;
```

**Impact:** Works with any register, not just RAX

### 3. Fix OP_DIV (Remove OP_CQO)

```c
/* Before */
case OP_DIV:
    BE(c)->emit_cqo(c);
    BE(c)->emit_idiv(c, src);
    break;

/* After */
case OP_DIV:
    BE(c)->emit_div(c, phys_dst, phys_src1, phys_src2);
    break;
```

**Impact:** x86_64.c handles cqo+idiv internally; AArch64/RISC-V use single sdiv

## Verification Strategy

```bash
# Before each change
make test  # Ensure 118/118 pass

# After each change
make test  # Ensure no regressions

# Cross-ISA testing
python3 tools/isa_diff.py test/aot/arithmetic.c --isas x86_64 aarch64
```

## Timeline

| Phase | Duration | Status |
|-------|----------|--------|
| Phase 0: Tooling | 1 day | ✓ Complete |
| Phase 1: Quick Wins | 1 week | Ready to start |
| Phase 2: Core Refactoring | 2 weeks | Pending |
| Phase 3: AArch64 Backend | 2-3 weeks | Pending |
| Phase 4: SIMD | 3-4 weeks | Pending |

**Total: 8-10 weeks for full ISA-agnostic IR + AArch64**

## Recommendation

**Start Phase 1 (Quick Wins) now:**
1. Run `python3 tools/ir_analyzer.py` to verify current state
2. Implement Day 1 fix (hardcoded "6")
3. Run tests
4. Commit and review
5. Continue with Days 2-5

This approach:
- Fixes critical issues first
- Low risk (small changes)
- Easy to revert if needed
- Builds confidence for larger refactoring

---

*Last updated: 2026-10-06*
*See also: docs/IR_ISA_AGNOSTIC_REFACTORING.md, docs/AArch64_BACKEND_ANALYSIS.md*
