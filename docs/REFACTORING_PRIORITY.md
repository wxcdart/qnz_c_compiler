# ISA-Agnostic Refactoring: Priority Analysis

## Comprehensive Scan Results

Total x86-64 patterns found: **113**

## Categorization: What to Fix vs What's OK

### ✅ OK - Backend-Specific (No Fix Needed)

| Location | Pattern | Why OK |
|----------|---------|--------|
| `src/zero_cc/isa/x86_64.c` | RAX, RBX, RBP, etc. | Backend implementation - should be x86-specific |
| `src/zero_cc/isa/aarch64.c` | x0, x1, x29, etc. | Backend implementation - should be AArch64-specific |
| Comments mentioning "System V AMD64" | Documentation | Just comments explaining ABI |
| `x86_emit_cqo`, `x86_emit_idiv` | Function names | Backend-specific implementation |

### ❌ MUST FIX - IR/Lowering Layer (Breaks Multi-ISA)

| Priority | Location | Pattern | Fix |
|----------|----------|---------|-----|
| **P0** | `ast_lowering.c:95` | `xmm_idx < 8` | `BE(c)->conv->fp_arg_reg_count` |
| **P0** | `ast_lowering.c:98` | `int_idx < 6` | `BE(c)->conv->int_arg_reg_count` |
| **P0** | `ast_lowering_mc.c:386` | `arg_count > 6` | `BE(c)->conv->int_arg_reg_count` |
| **P0** | `ast_lowering_mc.c:390` | `a >= 6` | `BE(c)->conv->int_arg_reg_count` |
| **P0** | `ast_lowering_mc.c:403` | `arg_count < 6` / `: 6` | `BE(c)->conv->int_arg_reg_count` |
| **P1** | `ir.h` | `OP_PUSH_RAX`, `OP_POP_RAX` | Rename to `OP_PUSH`, `OP_POP` |
| **P1** | `ir.h` | `OP_LEA_RAX_MEM` | Rename to `OP_LEA` |
| **P1** | `ir.h` | `OP_MOV_RAX_RBX` | Rename to `OP_MOV` |
| **P1** | `ir.h` | `OP_CQO` | Remove (merge into OP_DIV) |
| **P2** | `ast_lowering_expr.c` | `VREG_0` for struct return | Use backend table |
| **P2** | `ast_lowering_stmt.c` | `xmm0` for float return | Use backend table |

### ⚠️ NEEDS REVIEW - Stack Frame Logic

| Location | Pattern | Issue |
|----------|---------|-------|
| `compile.h:243` | `CALLEE_SAVE_FRAME_RESERVE` | x86-specific (48 bytes for rbx+r12-r15) |
| `ast_lowering.c:31-45` | `[rbp - slot]` | x86 frame pointer assumption |

## Detailed Fix Plan

### Phase 1: Calling Convention (P0) - 1 Day

**Files:**
- `src/zero_cc/parser/ast_lowering.c` (2 changes)
- `src/zero_cc/parser/ast_lowering_mc.c` (3 changes)

**Changes:**
```c
// Before
if (is_fp && xmm_idx < 8) { ... }
else if (!is_fp && int_idx < 6) { ... }

// After
if (is_fp && xmm_idx < BE(c)->conv->fp_arg_reg_count) { ... }
else if (!is_fp && int_idx < BE(c)->conv->int_arg_reg_count) { ... }

// Before
int n_stack = (arg_count > 6) ? arg_count - 6 : 0;
for (int a = arg_count - 1; a >= 6; a--) { ... }
int n_args = (arg_count < 6) ? arg_count : 6;

// After
int int_arg_count = BE(c)->conv->int_arg_reg_count;
int n_stack = (arg_count > int_arg_count) ? arg_count - int_arg_count : 0;
for (int a = arg_count - 1; a >= int_arg_count; a--) { ... }
int n_args = (arg_count < int_arg_count) ? arg_count : int_arg_count;
```

**Impact:** AArch64/RISC-V get 8 register args instead of 6

### Phase 2: IR Opcode Renaming (P1) - 2 Days

**Files:**
- `src/zero_cc/parser/ir.h`
- `src/zero_cc/parser/ast_lowering_mc.c`
- `src/zero_cc/parser/ast_lowering_expr.c`

**Changes:**
```c
// Rename OP_PUSH_RAX -> OP_PUSH
// Rename OP_POP_RAX -> OP_POP
// Rename OP_LEA_RAX_MEM -> OP_LEA
// Remove OP_CQO (merge logic into backend emit_div)
```

**Impact:** IR becomes ISA-neutral

### Phase 3: Struct/Float Return (P2) - 2 Days

**Files:**
- `src/zero_cc/parser/ast_lowering_expr.c`
- `src/zero_cc/parser/ast_lowering_stmt.c`

**Changes:**
```c
// Before
return VREG_0;  // Hardcoded for x86

// After  
return BE(c)->get_int_reg(0);  // Use backend mapping
```

**Impact:** Struct/float returns work on all ISAs

## Risk Assessment

| Change | Risk | Test Coverage | Rollback |
|--------|------|---------------|----------|
| P0: Calling convention | Low | 118 JIT tests | Easy |
| P1: IR opcode rename | Medium | 118 JIT tests | Easy |
| P2: Struct/float return | High | regalloc_test, abi tests | Medium |

## Verification Strategy

```bash
# Before each phase
make test  # 118/118 must pass

# After Phase 1
python3 tools/isa_diff.py test/aot/arithmetic.c --isas x86_64 aarch64

# After Phase 2
make test  # Ensure no regressions

# After Phase 3
python3 tools/isa_diff.py test/abi/struct_return.c --isas x86_64 aarch64
```

## Timeline

| Phase | Duration | Deliverable |
|-------|----------|-------------|
| Phase 0: Tooling | 1 day | ✓ Complete |
| Phase 1: Calling Convention | 1 day | AArch64 gets 8 args |
| Phase 2: IR Opcode Rename | 2 days | ISA-neutral IR |
| Phase 3: Struct/Float Return | 2 days | Multi-ISA ABI |
| Phase 4: AArch64 Backend | 2-3 weeks | Production-ready |

**Total: 4-5 weeks for core ISA-agnostic IR**

## Recommendation

**Start with Phase 1 (Calling Convention):**
1. Low risk
2. High impact (AArch64/RISC-V get correct args)
3. Easy to test and rollback
4. Builds confidence for larger changes

**Do NOT do SIMD yet:**
- Adding SIMD now = more x86 hardcoding
- Would need to refactor again after Phase 2
- Waste of effort

---

*Last updated: 2026-10-06*
