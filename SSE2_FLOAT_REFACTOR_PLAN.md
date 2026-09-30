# SSE2 Floating-Point Refactoring Plan

## Current State (Post-Commit)

### What Works ✓
- Float literals (`2.5`) → `movsd xmm0, [rodata]`
- Float variables → `movsd xmm0, [rbp-offset]` / `movsd [rbp-offset], xmm0`
- Float-to-int casts → `cvttsd2si rax, xmm0`
- Float arithmetic (+, -, *, /) → SSE2 instructions (`addsd`, `subsd`, `mulsd`, `divsd`)
- All 70+ tests pass

### Performance Issues ✗

#### Issue 1: Stack Spilling in Float BINOP
**Current Implementation:**
```assembly
; double c = a + b;
movsd [rsp], xmm0      ; Save left operand (MEM WRITE)
; ... evaluate b (result in xmm0) ...
movsd xmm1, [rsp]      ; Load left operand (MEM READ)
addsd xmm0, xmm1       ; xmm0 = xmm0 + xmm1
```

**Problem:** 2 unnecessary memory operations per float operation.

**Root Cause:** We save left operand to `[rsp]` because we don't know which XMM register to use for intermediate values.

#### Issue 2: No Float Register Allocation
- Integer values use `rax`, `rcx`, `rbx` as temporaries
- Float values have no dedicated register tracking
- `RegAllocator` exists but is unused for floats

#### Issue 3: Missing Float Operations
- Float comparisons (`==`, `!=`, `<`, `>`, `<=`, `>=`) → Not implemented
- Float unary negation → Not implemented
- Mixed int/float operations → Not implemented (e.g., `double + int`)

#### Issue 4: Function Call Convention Not Handled
- System V AMD64 ABI: float args passed in `xmm0-xmm7`
- Current: all args passed in integer registers

---

## Refactoring Goals

### Phase 1: Quick Wins (1-2 days)
**Objective:** Eliminate stack spilling without major refactoring

**Changes:**
1. Use `xmm1` for left operand instead of `[rsp]`
2. Add float comparisons using `ucomisd`
3. Add float unary negation (`subsd xmm0, xmm0` then `subsd xmm0, reg`)

**Expected Improvement:**
- 2 memory operations eliminated per float BINOP
- ~20% faster float arithmetic in generated code

### Phase 2: Float Register Tracking (2-3 days)
**Objective:** Track float registers during lowering

**Changes:**
1. Add `float_reg` field to AST node or use `node->aux.extra`
2. Track which XMM register holds each temporary float value
3. Extend register allocator to handle XMM registers separately

**Expected Improvement:**
- Proper register allocation for complex expressions
- Better code generation for nested float operations

### Phase 3: Calling Convention (2-3 days)
**Objective:** Implement System V AMD64 ABI for floats

**Changes:**
1. Detect float function parameters during parsing
2. Pass float args in `xmm0-xmm7` instead of `rdi-rsi`
3. Handle return values in `xmm0` for float functions

**Expected Improvement:**
- Correct ABI compliance
- Interoperability with system libraries

### Phase 4: Full Type-Driven Codegen (3-5 days)
**Objective:** Integrate semantic analysis type info into lowering

**Changes:**
1. Run semantic analysis before lowering (already exists, not wired)
2. Use `node->aux.type_ref` to determine operand types during lowering
3. Generate optimal code based on actual types (not just float flag)

**Expected Improvement:**
- Mixed int/float operations handled correctly
- Better optimization opportunities
- Cleaner separation of concerns

---

## Phase 1: Quick Wins - Detailed Plan

### Task 1.1: Use xmm1 Instead of Stack (2 hours)

**File:** `src/zero_cc/parser/ast_lowering.h`

**Current Code (NODE_BINOP):**
```c
case NODE_BINOP: {
    lower_expr(c, arena, node->left);
    bool left_is_float = c->last_expr_is_float;
    
    if (left_is_float) {
        me_emit_u8(c, 0xF2);
        me_emit_u8(c, 0x0F);
        me_emit_u8(c, 0x11);
        me_emit_u8(c, 0x04);
        me_emit_u8(c, 0x24); /* movsd [rsp], xmm0 */
    } else {
        me_push_rax(c);
    }
    
    lower_expr(c, arena, node->right);
    bool right_is_float = c->last_expr_is_float;
    
    if (left_is_float && right_is_float) {
        me_emit_u8(c, 0xF2);
        me_emit_u8(c, 0x0F);
        me_emit_u8(c, 0x10);
        me_emit_u8(c, 0x0C);
        me_emit_u8(c, 0x24); /* movsd xmm1, [rsp] */
        /* ... SSE2 arithmetic ... */
    }
    /* ... */
}
```

**New Code:**
```c
case NODE_BINOP: {
    lower_expr(c, arena, node->left);
    bool left_is_float = c->last_expr_is_float;
    
    if (left_is_float) {
        me_emit_u8(c, 0xF2);
        me_emit_u8(c, 0x0F);
        me_emit_u8(c, 0x10);
        me_emit_u8(c, 0xC9); /* movsd xmm1, xmm0 (copy to xmm1) */
    } else {
        me_push_rax(c);
    }
    
    lower_expr(c, arena, node->right);
    bool right_is_float = c->last_expr_is_float;
    
    if (left_is_float && right_is_float) {
        /* xmm0 has right operand, xmm1 has left operand */
        switch (node->aux.op) {
            case TK_PLUS:
                me_emit_u8(c, 0xF2);
                me_emit_u8(c, 0x0F);
                me_emit_u8(c, 0x58);
                me_emit_u8(c, 0xC1); /* addsd xmm0, xmm1 */
                break;
            /* ... other operations ... */
        }
        c->last_expr_is_float = 1;
    } else {
        /* Integer arithmetic unchanged */
        me_pop_rcx(c);
        /* ... */
    }
    break;
}
```

**Impact:**
- Eliminates 2 memory operations per float BINOP
- No changes to register allocator needed
- Simple, isolated change

### Task 1.2: Add Float Comparisons (3 hours)

**File:** `src/zero_cc/parser/ast_lowering.h`

**New Code (add to NODE_BINOP float case):**
```c
if (left_is_float && right_is_float) {
    /* xmm0 has right operand, xmm1 has left operand */
    switch (node->aux.op) {
        case TK_PLUS:
            me_emit_u8(c, 0xF2);
            me_emit_u8(c, 0x0F);
            me_emit_u8(c, 0x58);
            me_emit_u8(c, 0xC1); /* addsd xmm0, xmm1 */
            break;
        /* ... arithmetic ops ... */
        
        /* Float comparisons */
        case TK_EQ:
        case TK_NE:
        case TK_LT:
        case TK_LE:
        case TK_GT:
        case TK_GE:
            /* Compare xmm1 (left) with xmm0 (right) */
            me_emit_u8(c, 0x66);
            me_emit_u8(c, 0x0F);
            me_emit_u8(c, 0x2F);
            me_emit_u8(c, 0xC9); /* ucomisd xmm0, xmm1 */
            
            /* Set flags based on comparison */
            me_emit_u8(c, 0x0F);
            me_emit_u8(c, 0x9A); /* setnp al (check for unordered) */
            me_emit_u8(c, 0x44);
            me_emit_u8(c, 0x24);
            me_emit_u8(c, 0x00);
            
            /* Generate result based on operator */
            /* ... conditional logic ... */
            break;
        
        default:
            break;
    }
    c->last_expr_is_float = 1;
}
```

**Note:** Float comparisons are complex due to NaN handling. May need separate implementation.

### Task 1.3: Add Float Unary Negation (1 hour)

**File:** `src/zero_cc/parser/ast_lowering.h`

**New Code (in NODE_UNOP):**
```c
case NODE_UNOP: {
    if (node->aux.op == TK_INC || node->aux.op == TK_DEC) {
        /* ... existing increment/decrement ... */
    } else if (node->aux.op == TK_MINUS) {
        lower_expr(c, arena, node->left);
        if (c->last_expr_is_float) {
            /* Float negation: xor sign bit */
            me_emit_u8(c, 0x66);
            me_emit_u8(c, 0x0F);
            me_emit_u8(c, 0x2E); /* Or use pxor with sign mask */
            /* Simpler: load -1.0 and multiply */
            /* For now, use subtraction from 0.0 */
            /* TODO: optimize with sign bit manipulation */
        } else {
            me_emit_u8(c, 0x48);
            me_emit_u8(c, 0xF7);
            me_emit_u8(c, 0xD8); /* neg rax */
        }
    } else {
        /* ... other unops ... */
    }
    break;
}
```

---

## Phase 2: Float Register Tracking - Detailed Plan

### Task 2.1: Extend AST Node for Float Registers (2 hours)

**File:** `src/zero_cc/types/ast.h`

**Current:**
```c
typedef struct ZeroCCASTNode {
    ZeroCCASTTag tag;
    union {
        int64_t int_val;
        double float_val;
        char *s;
        /* ... */
    };
    struct {
        ASTRef left, right;
        int op;
        ASTRef alt;
        ZeroTypeRef type_ref;  /* Type info from semantic analysis */
    } aux;
} ZeroCCASTNode;
```

**Add:**
```c
typedef struct ZeroCCASTNode {
    ZeroCCASTTag tag;
    union {
        int64_t int_val;
        double float_val;
        char *s;
    };
    struct {
        ASTRef left, right;
        int op;
        ASTRef alt;
        ZeroTypeRef type_ref;
        uint8_t float_reg;     /* XMM register index (0-15) or 0xFF if spilled */
        uint8_t is_float;      /* 1 if this node produces float value */
    } aux;
} ZeroCCASTNode;
```

### Task 2.2: Implement Float Register Allocator (4 hours)

**File:** `src/zero_cc/parser/regalloc.h` (extend existing)

**Add:**
```c
typedef struct FloatRegAllocator {
    int xmm_regs[16];          /* 0 = free, >0 = busy with temp ID */
    int next_float_temp_id;
} FloatRegAllocator;

typedef struct ZeroCCCompiler {
    /* ... existing fields ... */
    RegAllocator regalloc;         /* Integer register allocator */
    FloatRegAllocator float_regalloc; /* Float register allocator */
    /* ... */
} ZeroCCCompiler;
```

**Functions to add:**
```c
static inline int float_reg_alloc(ZeroCCCompiler *c) {
    for (int i = 0; i < 16; i++) {
        if (c->float_regalloc.xmm_regs[i] == 0) {
            c->float_regalloc.xmm_regs[i] = c->float_regalloc.next_float_temp_id++;
            return i;
        }
    }
    /* All regs busy - spill to stack */
    return spill_float(c);
}

static inline void float_reg_free(ZeroCCCompiler *c, int reg) {
    if (reg >= 0 && reg < 16) {
        c->float_regalloc.xmm_regs[reg] = 0;
    }
}
```

---

## Phase 3: Calling Convention - Detailed Plan

### Task 3.1: Detect Float Parameters (2 hours)

**File:** `src/zero_cc/parser/ast_stmt.h`

**Add:** Track float parameters during function declaration parsing:
```c
typedef struct ZeroCCFunction {
    char name[128];
    ZeroCCType ret_type;
    ZeroCCType param_types[10];
    uint8_t param_count;
    uint8_t float_param_mask;  /* Bitmask: 1 = float param, 0 = integer param */
} ZeroCCFunction;
```

### Task 3.2: Implement Float Argument Passing (4 hours)

**File:** `src/zero_cc/parser/ast_lowering.h`

**In NODE_CALL:**
```c
case NODE_CALL: {
    /* Count float vs integer arguments */
    int float_arg_count = 0;
    int int_arg_count = 0;
    
    /* ... analyze arguments ... */
    
    /* Pass float args in xmm0-xmm7 */
    /* Pass integer args in rdi-rsi, rdx, rcx, r8, r9 */
    
    /* Call function */
    /* ... */
    break;
}
```

---

## Phase 4: Full Type-Driven Codegen - Detailed Plan

### Task 4.1: Wire Semantic Analysis (2 hours)

**File:** `src/zero_cc/jit/compile.h`

**Current:**
```c
void zero_cc_compile_c99(ZeroCCCompiler *c) {
    zero_cc_tokenize_c99(c->preprocessed, c);
    zero_cc_parse_c99(c);
    zero_cc_lower_ast(c);  /* No semantic analysis */
    zero_cc_emit_code(c);
}
```

**New:**
```c
void zero_cc_compile_c99(ZeroCCCompiler *c) {
    zero_cc_tokenize_c99(c->preprocessed, c);
    zero_cc_parse_c99(c);
    zero_cc_semantic_analysis(c);  /* Annotate AST with types */
    zero_cc_lower_ast(c);          /* Use type info for codegen */
    zero_cc_emit_code(c);
}
```

### Task 4.2: Use Type Info in Lowering (4 hours)

**File:** `src/zero_cc/parser/ast_lowering.h`

**Change from flag-based to type-based:**
```c
case NODE_BINOP: {
    lower_expr(c, arena, node->left);
    ZeroCCType left_type = zero_cc_ast_get_type(arena, node->left);
    
    lower_expr(c, arena, node->right);
    ZeroCCType right_type = zero_cc_ast_get_type(arena, node->right);
    
    /* Promote types (float + double → double) */
    ZeroCCType result_type = zero_cc_promote_binary_op(left_type, right_type);
    
    /* Generate code based on result_type */
    if (result_type.tag == TYPE_FLOAT || result_type.tag == TYPE_DOUBLE) {
        /* SSE2 arithmetic */
    } else {
        /* Integer arithmetic */
    }
    break;
}
```

---

## Testing Strategy

### For Each Phase
1. **Unit Tests:** Add specific tests for new functionality
2. **Regression Tests:** Ensure all 70+ existing tests still pass
3. **Performance Tests:** Compare generated code efficiency

### New Tests to Add
```c
/* test/float_compare.c */
double a = 1.5, b = 2.5;
return (a < b) ? 1 : 0;  /* Should return 1 */

/* test/float_mixed.c */
double a = 2.5;
int b = 3;
return (int)(a + b);  /* Should return 5 */

/* test/float_func.c */
double add(double a, double b) { return a + b; }
return add(1.5, 2.5);  /* Should return 4 */
```

---

## Timeline

| Phase | Duration | Risk | Dependencies |
|-------|----------|------|--------------|
| Phase 1: Quick Wins | 1-2 days | Low | None |
| Phase 2: Float Reg Alloc | 2-3 days | Medium | Phase 1 |
| Phase 3: Calling Conv | 2-3 days | High | Phase 2 |
| Phase 4: Type-Driven | 3-5 days | High | Semantic analysis integration |

**Total Estimated Time:** 8-13 days

---

## Success Criteria

### Phase 1
- [ ] Float BINOP uses xmm1 instead of stack
- [ ] Float comparisons work correctly
- [ ] All 70+ tests pass
- [ ] Generated code has 2 fewer memory ops per float BINOP

### Phase 2
- [ ] Float register allocator implemented
- [ ] Complex float expressions optimized
- [ ] No unnecessary stack spills

### Phase 3
- [ ] Float function args passed in xmm0-xmm7
- [ ] Float return values in xmm0
- [ ] Interoperable with system libraries

### Phase 4
- [ ] Semantic analysis wired into compilation pipeline
- [ ] Type-driven code generation
- [ ] Mixed int/float operations supported

---

## Notes

- **Priority:** Phase 1 is highest priority (biggest impact, lowest risk)
- **Backwards Compatibility:** All phases must maintain existing test suite
- **Documentation:** Update comments in ast_lowering.h as changes are made
- **Code Review:** Each phase should be reviewed before merging

---

*Created: $(date)*
*Status: Ready for Phase 1 implementation*
