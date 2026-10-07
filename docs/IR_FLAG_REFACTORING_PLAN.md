# IR Flag Refactoring Plan

## Overview

This plan addresses the two remaining IR design issues:
1. **Issue 1:** Jump operations use implicit flags
2. **Issue 2:** No explicit flag register in IR

## Current State

### Flag-Setting Operations
```c
OP_CMP,        /* compare src1, src2; sets flags */
OP_CMP_ZERO,   /* compare reg with zero; sets flags */
```

### Flag-Reading Operations
```c
OP_COND_SET,   /* set dst based on flags and condition code */
OP_JE,         /* jump if equal - IMPLICIT flag read */
OP_JNE,        /* jump if not equal - IMPLICIT flag read */
OP_JL,         /* jump if less than - IMPLICIT flag read */
OP_JLE,        /* jump if less or equal - IMPLICIT flag read */
OP_JG,         /* jump if greater - IMPLICIT flag read */
```

## Proposed Solution

### Phase 1: Add OP_JCOND (Jump Conditional)

Replace implicit flag-reading jumps with explicit `OP_JCOND`.

**New Opcode:**
```c
OP_JCOND,      /* conditional jump based on flags and condition code */
```

**Migration:**
```c
// Before:
ir_emit_jl(label);  // Implicit flag read

// After:
ir_emit_jcond(CC_LT, label);  // Explicit flag read
```

**Backend Interface:**
```c
// isa_backend.h
size_t (*emit_jcond)(ZeroCCCompiler *c, int cond, const char *label);

// x86_64.c
static size_t x86_emit_jcond(ZeroCCCompiler *c, int cond, const char *label) {
    static const uint8_t jcc_codes[] = {0x8C, 0x8E, 0x8F, 0x8D, 0x84, 0x85};
    return x86_emit_jcc(c, jcc_codes[cond], label);
}
```

### Phase 2: Add Flag Register to IR

Make flags a first-class register for better optimization.

**New Design:**
```c
// Add flag register constant
#define REG_FLAGS 255  // Special flag register

// Flag operations become explicit
OP_CMP,        /* sets REG_FLAGS */
OP_TEST,       /* sets REG_FLAGS */
OP_COND_SET,   /* reads REG_FLAGS */
OP_JCOND,      /* reads REG_FLAGS */
```

**Instruction Format Update:**
```c
typedef struct {
    uint8_t  op;
    uint8_t  temp_dest;    // For COND_SET: destination
    uint8_t  temp_src1;    // For CMP: first operand
    uint8_t  temp_src2;    // For CMP: second operand
    uint8_t  cond;         // For COND_SET/JCOND: condition code
    uint8_t  flags_out;    // NEW: flags output register (REG_FLAGS or 0xFF)
    uint8_t  flags_in;     // NEW: flags input register (REG_FLAGS or 0xFF)
    int16_t  imm;
    int64_t  imm64;
    uint16_t label_idx;    // For JCOND: jump target
} Instr;
```

**Updated Semantics:**
```c
OP_CMP(src1, src2) {
    temp_dest = 0xFF;
    flags_out = REG_FLAGS;  // Explicitly sets flags
}

OP_COND_SET(dst, cond) {
    flags_in = REG_FLAGS;   // Explicitly reads flags
}

OP_JCOND(cond, label) {
    flags_in = REG_FLAGS;   // Explicitly reads flags
}
```

## Implementation Plan

### Phase 1: Add OP_JCOND (2-3 days)

#### Step 1: Add OP_JCOND to ir.h
```c
/* Control Flow (7) */
OP_JMP,
OP_JE,
OP_JNE,
OP_JL,
OP_JLE,
OP_JG,
OP_JCOND,      /* NEW: conditional jump with explicit flags */
OP_JMP_LABEL,
```

#### Step 2: Add emit_jcond to isa_backend.h
```c
size_t (*emit_jcond)(ZeroCCCompiler *c, int cond, const char *label);
```

#### Step 3: Implement x86_64_emit_jcond
```c
static size_t x86_emit_jcond(ZeroCCCompiler *c, int cond, const char *label) {
    static const uint8_t jcc_codes[] = {0x84, 0x85, 0x8C, 0x8D, 0x8E, 0x8F};
    // jcc label (reads flags set by previous cmp)
    return x86_emit_jcc_internal(c, jcc_codes[cond], label);
}
```

#### Step 4: Update ast_lowering_expr.c
Replace all conditional jumps with OP_JCOND:
```c
// In if statement lowering:
// Before:
ir_emit_cmp(left, right);
ir_emit_jl(false_label);

// After:
ir_emit_cmp(left, right);
ir_emit_jcond(CC_LT, false_label);
```

#### Step 5: Update ast_lowering_mc.c
```c
case OP_JCOND:
    if (BE(c)->emit_jcond) {
        BE(c)->emit_jcond(c, (int)instr->cond, get_label_name(instr->label_idx));
    }
    break;
```

#### Step 6: Update opt_dce.c
```c
case OP_JCOND:
    return 1;  // Has side effect (control flow)
```

#### Step 7: Test
- All 118 tests must pass
- Verify control flow is correct

### Phase 2: Add Flag Register (3-5 days)

#### Step 1: Define REG_FLAGS constant
```c
#define REG_FLAGS 255  // Special flag register
```

#### Step 2: Update Instr structure
```c
typedef struct {
    uint8_t  op;
    uint8_t  temp_dest;
    uint8_t  temp_src1;
    uint8_t  temp_src2;
    uint8_t  cond;
    uint8_t  flags_out;  // NEW
    uint8_t  flags_in;   // NEW
    int16_t  imm;
    int64_t  imm64;
    uint16_t label_idx;
} Instr;
```

**Note:** This changes Instr size from 16 to 18 bytes. May need to pad to maintain alignment.

#### Step 3: Update OP_CMP semantics
```c
static inline void ir_emit_cmp(int temp_src1, int temp_src2) {
    ir_emit((Instr){
        .op = OP_CMP,
        .temp_dest = 0xFF,
        .temp_src1 = (uint8_t)temp_src1,
        .temp_src2 = (uint8_t)temp_src2,
        .cond = 0,
        .flags_out = REG_FLAGS,  // Explicitly sets flags
        .flags_in = 0xFF,         // Doesn't read flags
        .imm = 0,
        .imm64 = 0,
        .label_idx = 0
    });
}
```

#### Step 4: Update OP_COND_SET semantics
```c
static inline void ir_emit_cond_set(int temp_dest, int cond) {
    ir_emit((Instr){
        .op = OP_COND_SET,
        .temp_dest = (uint8_t)temp_dest,
        .temp_src1 = 0xFF,
        .temp_src2 = 0xFF,
        .cond = (uint8_t)cond,
        .flags_out = 0xFF,        // Doesn't set flags
        .flags_in = REG_FLAGS,     // Explicitly reads flags
        .imm = 0,
        .imm64 = 0,
        .label_idx = 0
    });
}
```

#### Step 5: Update OP_JCOND semantics
```c
static inline void ir_emit_jcond(int cond, int label_idx) {
    ir_emit((Instr){
        .op = OP_JCOND,
        .temp_dest = 0xFF,
        .temp_src1 = 0xFF,
        .temp_src2 = 0xFF,
        .cond = (uint8_t)cond,
        .flags_out = 0xFF,        // Doesn't set flags
        .flags_in = REG_FLAGS,     // Explicitly reads flags
        .imm = 0,
        .imm64 = 0,
        .label_idx = (uint16_t)label_idx
    });
}
```

#### Step 6: Update backend implementations
Backends now see explicit flag dependencies:
```c
// x86_64.c
static void x86_emit_instr(ZeroCCCompiler *c, Instr *instr) {
    switch (instr->op) {
        case OP_CMP:
            // instr->flags_out = REG_FLAGS
            x86_emit_cmp(c, instr->temp_src1, instr->temp_src2);
            break;
        case OP_COND_SET:
            // instr->flags_in = REG_FLAGS
            x86_emit_cond_set(c, instr->temp_dest, instr->cond);
            break;
        case OP_JCOND:
            // instr->flags_in = REG_FLAGS
            x86_emit_jcond(c, instr->cond, get_label(instr->label_idx));
            break;
    }
}
```

#### Step 7: Implement Flag CSE Optimization
```c
// opt_flag_cse.c
// If multiple COND_SET/JCOND follow one CMP, share the cmp

void opt_flag_cse(ZeroCCCompiler *c) {
    for (int i = 0; i < instr_count - 1; i++) {
        Instr *cmp = &instrs[i];
        if (cmp->op != OP_CMP) continue;
        if (cmp->flags_out != REG_FLAGS) continue;
        
        // Look for consecutive COND_SET/JCOND that read flags
        int j = i + 1;
        while (j < instr_count && instrs[j].flags_in == REG_FLAGS) {
            // This instruction uses the same flags
            // Can potentially optimize
            j++;
        }
        
        if (j > i + 1) {
            // Multiple flag users found - can optimize
            // TODO: Implement CSE logic
        }
    }
}
```

#### Step 8: Implement Flag DCE Optimization
```c
// opt_flag_dce.c
// Remove CMP if its flags are never used

void opt_flag_dce(ZeroCCCompiler *c) {
    for (int i = 0; i < instr_count; i++) {
        Instr *instr = &instrs[i];
        if (instr->op != OP_CMP) continue;
        if (instr->flags_out != REG_FLAGS) continue;
        
        // Check if any subsequent instruction reads flags
        bool flags_used = false;
        for (int j = i + 1; j < instr_count && !flags_used; j++) {
            if (instrs[j].flags_in == REG_FLAGS) {
                flags_used = true;
            }
            // Stop at control flow boundary
            if (instrs[j].op == OP_JMP || instrs[j].op == OP_JCOND) {
                break;
            }
        }
        
        if (!flags_used) {
            // Mark as dead
            instr->op = OP_DEAD;
        }
    }
}
```

#### Step 9: Test
- All 118 tests must pass
- Verify flag CSE and DCE optimizations work

## Risk Assessment

### Phase 1: Add OP_JCOND
| Risk | Level | Mitigation |
|------|-------|------------|
| Breaking changes | LOW | Backward compatible (old jumps still work) |
| Test failures | LOW | Incremental changes, test after each step |
| Backend complexity | LOW | Simple mapping to existing jcc |

**Estimated effort:** 2-3 days
**Risk level:** LOW

### Phase 2: Add Flag Register
| Risk | Level | Mitigation |
|------|-------|------------|
| Instr size change | MEDIUM | Careful alignment, test thoroughly |
| Backend changes | MEDIUM | Update all backends consistently |
| Optimization bugs | MEDIUM | Extensive testing, incremental rollout |

**Estimated effort:** 3-5 days
**Risk level:** MEDIUM

## Rollout Strategy

### Week 1: Phase 1 (OP_JCOND)
- Day 1: Add OP_JCOND to ir.h, isa_backend.h
- Day 2: Implement x86_64_emit_jcond, update lowering
- Day 3: Test, fix, document

### Week 2: Phase 2 (Flag Register)
- Day 1-2: Update Instr structure, update all opcodes
- Day 3-4: Update backends, implement optimizations
- Day 5: Test, fix, document

## Success Criteria

### Phase 1
- ✓ OP_JCOND added and working
- ✓ All 118 tests pass
- ✓ Control flow is correct

### Phase 2
- ✓ Flag register added to IR
- ✓ All flag operations explicit
- ✓ Flag CSE optimization works
- ✓ Flag DCE optimization works
- ✓ All 118 tests pass

## Rollback Plan

If issues arise:
1. Phase 1: Revert OP_JCOND changes, keep implicit jumps
2. Phase 2: Revert flag register, keep OP_JCOND

Both phases are backward compatible and can be rolled back independently.

---

*Plan created: 2026-10-06*
*Ready for implementation*
