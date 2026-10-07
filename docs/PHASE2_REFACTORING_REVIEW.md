# Phase 2: IR Opcode Refactoring - Review Document

## Current State

After Phase 1, we have 17 remaining x86-specific opcode issues:

| Category | Count | Files |
|----------|-------|-------|
| Register-specific opcodes | 6 | ir.h, ast_lowering_mc.c |
| Condition code opcodes | 8 | ir.h, ast_lowering_mc.c, x86_64.c |
| Division helper | 1 | ir.h (OP_CQO not used) |

## Detailed Analysis

### 1. Register-Specific Opcodes (6 opcodes)

These opcodes hardcode RAX/RBX register names:

| Opcode | Current Usage | Proposed Change |
|--------|---------------|-----------------|
| `OP_MOV_RAX_MEM` | Load from stack to "RAX" | → `OP_MOV` (generic) |
| `OP_MOV_MEM_RAX` | Store "RAX" to stack | → `OP_STORE` (generic) |
| `OP_MOV_RAX_RBX` | Move register to register | → `OP_MOV` (generic) |
| `OP_LEA_RAX_MEM` | Load effective address | → `OP_LEA` (generic) |
| `OP_PUSH_RAX` | Push RAX to stack | → `OP_PUSH` (generic) |
| `OP_POP_RAX` | Pop stack to RAX | → `OP_POP` (generic) |
| `OP_PUSH_RBX` | Push RBX to stack | → `OP_PUSH` (generic) |
| `OP_POP_RBX` | Pop stack to RBX | → `OP_POP` (generic) |

**Note:** `OP_LEA_RIP` is already ISA-neutral (position-independent addressing)

### 2. Condition Code Opcodes (8 opcodes)

These are x86-specific SETcc instructions:

| Opcode | Current Usage | Proposed Change |
|--------|---------------|-----------------|
| `OP_TEST` | AND for flags | → `OP_CMP` (or keep as-is) |
| `OP_SETL` | Set if less than | → `OP_CMP` + `OP_JL` + `OP_MOV` |
| `OP_SETLE` | Set if ≤ | → `OP_CMP` + `OP_JLE` + `OP_MOV` |
| `OP_SETG` | Set if greater | → `OP_CMP` + `OP_JG` + `OP_MOV` |
| `OP_SETGE` | Set if ≥ | → `OP_CMP` + `OP_JGE` + `OP_MOV` |
| `OP_SET_E` | Set if equal | → `OP_CMP` + `OP_JE` + `OP_MOV` |
| `OP_SET_NE` | Set if ≠ | → `OP_CMP` + `OP_JNE` + `OP_MOV` |
| `OP_LNOT` | Logical NOT | → Keep (uses SETcc internally) |

### 3. Division Helper (Already OK)

| Opcode | Status | Notes |
|--------|--------|-------|
| `OP_DIV` | ✓ OK | Already ISA-neutral, backend handles cqo+idiv |
| `OP_MOD` | ✓ OK | Already ISA-neutral |
| `OP_CQO` | Not used | No IR opcode exists (handled in backend) |

## Files to Modify

### Core Changes (Low Risk)

| File | Lines | Change |
|------|-------|--------|
| `src/zero_cc/parser/ir.h` | ~20 | Rename 8 opcodes |
| `src/zero_cc/parser/ast_lowering_mc.c` | ~20 | Update switch cases |

### Backend Changes (Medium Risk)

| File | Lines | Change |
|------|-------|--------|
| `src/zero_cc/isa/x86_64.c` | ~30 | Keep existing functions, just rename |
| `src/zero_cc/isa/aarch64.c` | ~0 | Already uses generic names |
| `src/zero_cc/isa/riscv.c` | ~0 | Already uses generic names |

### IR Lowering Changes (Medium Risk)

| File | Lines | Change |
|------|-------|--------|
| `src/zero_cc/parser/ast_lowering_expr.c` | ~5 | Replace SETcc usage with CMP+JMP |
| `src/zero_cc/parser/ast_lowering_stmt.c` | ~0 | No changes needed |

## Proposed Implementation

### Step 1: Rename Register-Specific Opcodes

```c
// ir.h - Before
OP_MOV_RAX_MEM,    /* load from stack slot to register */
OP_MOV_RAX_RBX,    /* move register to register */
OP_LEA_RAX_MEM,    /* load effective address from stack slot */
OP_PUSH_RAX,       /* push register to stack */
OP_POP_RAX,        /* pop register from stack */
OP_PUSH_RBX,       /* push register to stack */
OP_POP_RBX,        /* pop register from stack */

// ir.h - After
OP_MOV,            /* load from stack slot to register */
OP_MOV_REG,        /* move register to register */
OP_LEA,            /* load effective address from stack slot */
OP_PUSH,           /* push register to stack */
OP_POP,            /* pop register from stack */
```

**Impact:** All backends use generic names, no functional change

### Step 2: Keep Condition Code Opcodes (Defer)

**Decision:** Keep `OP_TEST`, `OP_SETL`, `OP_SETLE`, etc. for now.

**Reason:** These are used by the IR layer and replacing them requires:
- Adding new IR opcodes (CMP_BRANCH pattern)
- Rewriting lowering code in ast_lowering_expr.c
- High risk of breaking tests

**Alternative:** Keep them as-is. They're x86-specific but the backend interface is clean.

### Step 3: Update Backend Interface

```c
// isa_backend.h - Keep existing
typedef struct {
    void (*emit_push_reg)(ZeroCCCompiler *c, int reg);
    void (*emit_pop_reg)(ZeroCCCompiler *c, int reg);
    void (*emit_mov)(ZeroCCCompiler *c, int dst, int src);
    void (*emit_lea)(ZeroCCCompiler *c, int dst, int base, int32_t offset);
    // ... keep existing functions
} ISABackendFuncs;
```

## Risk Assessment

| Change | Risk | Test Coverage | Rollback |
|--------|------|---------------|----------|
| Opcode rename | Low | 118 JIT tests | Easy (git revert) |
| Backend rename | Low | 118 JIT tests | Easy |
| SETcc replacement | High | 118 JIT tests | Medium |

## Recommended Approach

### Option A: Conservative (Recommended)

1. **Rename only register-specific opcodes** (Step 1)
2. **Keep condition code opcodes** as-is (defer to Phase 3)
3. **Update backend function names** to match

**Pros:**
- Low risk
- Easy to test
- Easy to rollback
- Achieves 80% of goal

**Cons:**
- Still has x86-specific SETcc opcodes
- Not 100% ISA-neutral

### Option B: Aggressive

1. Rename register-specific opcodes
2. Replace SETcc with CMP_BRANCH pattern
3. Update all backends

**Pros:**
- 100% ISA-neutral
- Cleaner design

**Cons:**
- High risk
- Complex changes
- Harder to test
- Longer timeline

## Implementation Plan (Option A - Conservative)

### Day 1: Rename Register-Specific Opcodes

```bash
# ir.h changes
OP_MOV_RAX_MEM → OP_MOV
OP_MOV_MEM_RAX → OP_STORE (or keep as OP_MOV_MEM)
OP_MOV_RAX_RBX → OP_MOV_REG
OP_LEA_RAX_MEM → OP_LEA
OP_PUSH_RAX/OP_PUSH_RBX → OP_PUSH
OP_POP_RAX/OP_POP_RBX → OP_POP

# ast_lowering_mc.c changes
case OP_MOV_RAX_MEM: → case OP_MOV:
case OP_PUSH_RAX: → case OP_PUSH:
...
```

### Day 2: Update Backend Functions

```bash
# x86_64.c - Keep existing functions, just rename
x86_emit_push_reg → keep (already generic)
x86_emit_pop_reg → keep (already generic)
```

### Day 3: Test and Fix

```bash
make test  # 118/118 must pass
```

## Timeline

| Phase | Duration | Status |
|-------|----------|--------|
| Phase 1: Calling Convention | 1 day | ✓ Complete |
| Phase 2A: Opcode Rename | 2-3 days | Ready |
| Phase 2B: SETcc Refactor | 2 days | Deferred |
| Phase 3: AArch64 Backend | 2-3 weeks | Pending |

## Recommendation

**Proceed with Option A (Conservative):**

1. Rename register-specific opcodes only
2. Keep condition code opcodes as-is
3. Test thoroughly
4. Defer SETcc refactoring to Phase 3

**Why?**
- Low risk
- Fast implementation (2-3 days)
- Achieves most of the goal
- Can always do Option B later

**Do NOT do SIMD yet:**
- Still has x86 hardcoding
- Would need to refactor again
- Wastes effort

---

*Last updated: 2026-10-06*
*See also: docs/REFACTORING_PRIORITY.md, docs/FINAL_FIX_LIST.md*
