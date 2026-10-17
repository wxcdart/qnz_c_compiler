# IR Design Improvements Proposal

## Current Issues

### 1. Implicit Flag Dependency in OP_CMP_BRANCH

**Current:**
```c
// IR instruction combines cmp + conditional set
ir_emit_cmp_branch(dst, src1, src2, cond);

// x86_64 backend implementation:
static void x86_emit_cmp_branch(...) {
    if (src1 != 0xFF || src2 != 0xFF) {
        x86_emit_cmp(c, src1, src2);  // Sets flags
    }
    // setcc reads flags - ASSUMES flags are still set!
    x86_emit_setcc(c, dst, setcc_codes[cond]);
}
```

**Problem:** If register allocator spills between cmp and setcc, flags are clobbered.

### 2. Inefficient Condition Code Encoding

**Current:**
```c
// Condition code stored in int16_t imm field
Instr {
    ...
    int16_t imm;  // Used for cond (0-5), but supports -32768 to 32767
    ...
}
```

**Problem:** Wastes encoding space, no validation.

### 3. OP_LNOT Fragility

**Current:**
```c
// Logical NOT uses two instructions relying on implicit flags
ir_emit_cmp_zero(operand_temp);      // Sets flags
ir_emit_cmp_branch(result, 0xFF, 0xFF, CC_EQ);  // Reads flags
```

**Problem:** If backend reorders or spills, flags lost.

## Proposed Solution: Split CMP_BRANCH into Two Instructions

### Design

```c
// New opcodes
typedef enum {
    // ... existing opcodes ...
    
    /* Comparison (3) */
    OP_CMP,            /* compare src1, src2; sets flags */
    OP_COND_SET,       /* set dst based on flags and condition code */
    
    /* Condition codes for OP_COND_SET */
    CC_LT = 0,
    CC_LE = 1,
    CC_GT = 2,
    CC_GE = 3,
    CC_EQ = 4,
    CC_NE = 5,
} Opcode;

// New instruction format for OP_COND_SET
typedef struct {
    uint8_t  op;
    uint8_t  temp_dest;    // Destination register
    uint8_t  cond;         // Condition code (0-5, fits in uint8_t)
    uint8_t  _padding;     // Align to 2 bytes
    int16_t  imm;
    int64_t  imm64;
    uint16_t label_idx;
} Instr;
```

### Benefits

1. **Explicit flag dependency** - OP_COND_SET clearly depends on flags from previous OP_CMP
2. **Better optimization** - DCE can see flag usage, CSE can share cmp results
3. **Cleaner backend interface** - Two separate functions:
   ```c
   void (*emit_cmp)(ZeroCCCompiler *c, int src1, int src2);
   void (*emit_cond_set)(ZeroCCCompiler *c, int dst, int cond);
   ```
4. **No flag clobbering risk** - cmp and cond_set are separate instructions
5. **Efficient encoding** - cond fits in uint8_t field

### Migration Plan

#### Step 1: Add new opcodes to ir.h

```c
// Replace OP_CMP_BRANCH with OP_COND_SET
// Keep OP_CMP (already exists)
```

#### Step 2: Update IR lowering (ast_lowering_expr.c)

```c
// Comparison: cmp + cond_set
ir_emit_cmp(left_temp, right_temp);
ir_emit_cond_set(result_temp, cond);
```

#### Step 3: Update ast_lowering_mc.c

```c
case OP_COND_SET:
    if (BE(c)->emit_cond_set) {
        BE(c)->emit_cond_set(c, phys_dst, (int)instr->cond);
    }
    break;
```

#### Step 4: Update backends

```c
// x86_64.c
static void x86_emit_cond_set(ZeroCCCompiler *c, int dst, int cond) {
    static const uint8_t setcc_codes[] = {0x9C, 0x9E, 0x9F, 0x9D, 0x94, 0x95};
    x86_emit_setcc(c, dst, setcc_codes[cond]);
}

// aarch64.c
static void aarch64_emit_cond_set(ZeroCCCompiler *c, int dst, int cond) {
    // cset dst, cond
    static const uint8_t cond_codes[] = {...};
    aarch64_emit_cset(c, dst, cond_codes[cond]);
}

// riscv.c
static void riscv64_emit_cond_set(ZeroCCCompiler *c, int dst, int cond) {
    // slt + li pattern based on cond
}
```

#### Step 5: Update OP_LNOT

```c
// Clearer flag dependency
ir_emit_cmp_zero(operand_temp);
ir_emit_cond_set(result_temp, CC_EQ);
```

## Alternative: Keep Single Instruction but Fix Encoding

If we want to minimize changes, we could:

1. Keep OP_CMP_BRANCH as single instruction
2. Change imm field to cond field (uint8_t)
3. Add validation in backends

**Pros:** Less refactoring
**Cons:** Still has implicit flag dependency, worse for optimization

## Recommendation

**Go with the split approach (OP_CMP + OP_COND_SET)**

**Why:**
1. Cleaner semantics - cmp sets flags, cond_set reads them
2. Better optimization opportunities
3. Safer - no implicit flag state between instructions
4. Easier to implement backends correctly
5. Minimal runtime cost (just one extra instruction in IR, same machine code)

**Effort:** 2-3 days for complete refactoring

**Risk:** Medium (changes IR format, but all tests pass before/after)

## Implementation Checklist

- [ ] Update ir.h: Add OP_COND_SET, move cond codes
- [ ] Update ir_emit_ops.h: Add ir_emit_cond_set(), remove ir_emit_cmp_branch()
- [ ] Update ast_lowering_expr.c: Use cmp + cond_set
- [ ] Update ast_lowering_mc.c: Handle OP_COND_SET
- [ ] Update opt_dce.c: Mark OP_COND_SET as reading flags
- [ ] Update x86_64.c: Implement emit_cond_set()
- [ ] Update isa_backend.h: Add emit_cond_set() interface
- [ ] Update ir_opcodes_test.c: New opcode values
- [ ] Test: 118/118 must pass

---

*Ready for review and implementation*
