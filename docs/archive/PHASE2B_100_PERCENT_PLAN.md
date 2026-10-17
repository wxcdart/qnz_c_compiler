# Phase 2B: 100% ISA-Neutral IR Plan

## Current Status (After Phase 2A)

**Completed:**
- ✓ Phase 1: Calling convention tables (9 fixes)
- ✓ Phase 2A: Register-specific opcode renames (8 opcodes)

**Remaining: 8 Condition Code Opcodes**

| Opcode | Description | x86 Implementation |
|--------|-------------|-------------------|
| `OP_TEST` | AND for flags | `test src1, src2` |
| `OP_SETL` | Set if less than | `setl dst` (0x9C) |
| `OP_SETLE` | Set if ≤ | `setle dst` (0x9E) |
| `OP_SETG` | Set if greater | `setg dst` (0x9F) |
| `OP_SETGE` | Set if ≥ | `setge dst` (0x9D) |
| `OP_SET_E` | Set if equal | `sete dst` (0x94) |
| `OP_SET_NE` | Set if ≠ | `setne dst` (0x95) |
| `OP_LNOT` | Logical NOT | `test + sete` |

## The Problem

SETcc instructions are x86-specific. AArch64 and RISC-V don't have direct equivalents.

**AArch64 approach:**
```assembly
cmp x0, x1
cset x2, lt    // Conditional select (not SETcc)
```

**RISC-V approach:**
```assembly
slt x2, x0, x1  // Set less than
```

## Three Approaches

### Approach A: Keep SETcc (Current)

**Pros:**
- Simple
- Works for x86

**Cons:**
- Not ISA-neutral
- Requires backend-specific implementations

### Approach B: Replace with CMP + BRANCH + MOV

**Pattern:**
```c
// Before: OP_SETL dst
cmp src1, src2
jl .less_than
mov dst, 0
jmp .done
.less_than:
mov dst, 1
.done:

// After: CMP + conditional branches
```

**Pros:**
- ISA-neutral (all ISAs have compare + branch)
- Clean design

**Cons:**
- More complex (multiple instructions)
- Requires new IR opcodes or multi-instruction sequences
- High risk of breaking tests

### Approach C: Add ISA-Neutral CMP_BRANCH Opcode

**New IR opcode:**
```c
OP_CMP_BRANCH,  // Compare and set dst based on condition
    .temp_dest = dst,
    .temp_src1 = src1,
    .temp_src2 = src2,
    .imm = condition_code,  // LT, LE, GT, GE, EQ, NE
```

**Backend implementations:**
- x86_64: cmp + setcc
- aarch64: cmp + cset
- riscv: slt/slti + branch

**Pros:**
- ISA-neutral IR
- Single opcode per condition
- Clean backend separation

**Cons:**
- Requires new opcode
- Moderate risk

## Recommended Approach: Approach C

### Implementation Plan

#### Step 1: Add New IR Opcode (Day 1)

```c
// ir.h
typedef enum {
    // ... existing opcodes ...
    OP_CMP_BRANCH,  /* compare and set based on condition */
    
    // Condition codes (use imm field)
    CC_LT,  // Signed less than
    CC_LE,  // Signed less or equal
    CC_GT,  // Signed greater
    CC_GE,  // Signed greater or equal
    CC_EQ,  // Equal
    CC_NE,  // Not equal
} ZeroCCOpcode;

// Helper function
static inline void ir_emit_cmp_branch(uint8_t dst, uint8_t src1, uint8_t src2, uint8_t cond) {
    ir_emit((Instr){.op = OP_CMP_BRANCH, .temp_dest = dst, .temp_src1 = src1, 
                    .temp_src2 = src2, .imm = cond, .imm64 = 0, .label_idx = 0});
}
```

#### Step 2: Update Lowering Code (Day 1-2)

```c
// ast_lowering_expr.c - Replace OP_LNOT
// Before:
ir_emit_lnot(result_temp, operand_temp);

// After:
ir_emit_cmp(operand_temp, operand_temp, 0);  // Compare with 0
ir_emit_cmp_branch(result_temp, 0, 0, CC_EQ);  // Set if equal
```

```c
// ast_lowering_stmt.c - Replace SETcc usage
// Find all places where SETcc opcodes are emitted and replace with CMP_BRANCH
```

#### Step 3: Update Backend Interface (Day 2)

```c
// isa_backend.h
typedef struct {
    // ... existing functions ...
    void (*emit_cmp_branch)(ZeroCCCompiler *c, int dst, int src1, int src2, int cond);
} ISABackendFuncs;

// Condition codes
#define CC_LT  0
#define CC_LE  1
#define CC_GT  2
#define CC_GE  3
#define CC_EQ  4
#define CC_NE  5
```

#### Step 4: Implement Backend Functions (Day 2-3)

**x86_64.c:**
```c
static void x86_emit_cmp_branch(ZeroCCCompiler *c, int dst, int src1, int src2, int cond) {
    static const uint8_t setcc_codes[] = {0x9C, 0x9E, 0x9F, 0x9D, 0x94, 0x95};
    x86_alu(c, 0x85, src1, src2);  // cmp src1, src2
    x86_setcc(c, dst, setcc_codes[cond]);
}
```

**aarch64.c:**
```c
static void aarch64_emit_cmp_branch(ZeroCCCompiler *c, int dst, int src1, int src2, int cond) {
    static const uint8_t conditions[] = {0x80, 0x81, 0x82, 0x83, 0x84, 0x85};  // LT, LE, GT, GE, EQ, NE
    arm64_cmp(c, src1, src2);
    arm64_cset(c, dst, conditions[cond]);
}
```

**riscv.c:**
```c
static void riscv_emit_cmp_branch(ZeroCCCompiler *c, int dst, int src1, int src2, int cond) {
    // Use slt/slti and branch
    static const int slt_ops[] = {0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    riscv_slt(c, dst, src1, src2, slt_ops[cond]);
}
```

#### Step 5: Remove Old SETcc Opcodes (Day 3)

```c
// ir.h - Remove these opcodes
OP_TEST,
OP_SETL,
OP_SETLE,
OP_SETG,
OP_SETGE,
OP_SET_E,
OP_SET_NE,

// ast_lowering_mc.c - Remove switch cases
// x86_64.c - Remove x86_emit_setl, x86_emit_setle, etc.
```

#### Step 6: Test and Fix (Day 3-4)

```bash
make test  # 118/118 must pass
```

## Risk Assessment

| Change | Risk | Mitigation |
|--------|------|------------|
| New opcode | Low | Add, don't remove initially |
| Lowering changes | Medium | Test incrementally |
| Backend implementations | Medium | Start with x86 only |
| Remove old opcodes | High | After tests pass |

## Timeline

| Step | Duration | Deliverable |
|------|----------|-------------|
| Step 1: Add opcode | 0.5 day | New IR opcode |
| Step 2: Update lowering | 1 day | CMP_BRANCH usage |
| Step 3: Backend interface | 0.5 day | New function pointer |
| Step 4: Backend impl | 1 day | x86/aarch64/riscv |
| Step 5: Remove old | 0.5 day | Clean IR |
| Step 6: Test/fix | 1 day | 118/118 passing |

**Total: 4-5 days**

## Verification Strategy

```bash
# After each step
make test  # Ensure no regressions

# After completion
python3 tools/ir_analyzer.py  # Should show 0 errors for SETcc
python3 tools/isa_diff.py test/abi/comparison.c --isas x86_64 aarch64
```

## Alternative: Minimal Approach

If Approach C is too risky, here's a minimal approach:

**Keep SETcc opcodes but make them ISA-neutral:**

```c
// Keep OP_SETL, OP_SETLE, etc. in IR
// But rename backend functions to be generic
// x86_64.c: x86_emit_setl -> emit_setl (in table)
// aarch64.c: aarch64_emit_cset_lt -> emit_setl (in table)
```

**Pros:**
- Minimal changes
- Low risk

**Cons:**
- Still x86-named opcodes
- Not 100% clean

## Recommendation

**Proceed with Approach C (CMP_BRANCH)**

**Why:**
1. Clean ISA-neutral design
2. Reasonable risk (4-5 days)
3. Achieves 100% goal
4. Better long-term maintainability

**Fallback:**
If Approach C fails tests, fall back to "Minimal Approach" (keep SETcc but genericize backend).

---

*Last updated: 2026-10-06*
*See also: docs/PHASE2_REFACTORING_REVIEW.md, docs/REFACTORING_PRIORITY.md*
