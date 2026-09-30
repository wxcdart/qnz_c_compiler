# SSE2 Floating-Point Refactoring Plan

## Cross-ISA Architecture Considerations

### Current State

**Backend Abstraction Layer Exists** ✓
The compiler already has a backend abstraction in `zero_cc_backends.h`:
```c
typedef struct ZeroCCEmitBackend {
    void (*emit_prologue)(...);
    void (*emit_mov)(...);
    void (*emit_alu)(...);
    /* ... */
} ZeroCCEmitBackend;

typedef struct ZeroCCABICallBackend {
    const char **int_arg_regs;
    const char **fp_arg_regs;      // Float arg registers
    const char *int_ret;
    const char *fp_ret;            // Float return register
    /* ... */
} ZeroCCABICallBackend;
```

**Problem:** Backend is initialized but NOT used!
- `ast_lowering.h` directly emits x86-64 bytes (`me_emit_u8(c, 0xF2)`, etc.)
- Backend pointers are set to NULL in `minimal.c`
- No abstraction between lowering and instruction encoding

### ISA Comparison

| Feature | x86-64 (Current) | ARM64 (AArch64) | RISC-V |
|---------|------------------|-----------------|--------|
| **Integer Regs** | rax, rbx, rcx, rdx, rsi, rdi, r8-r15 | x0-x30 | x0-x31 |
| **Float Regs** | xmm0-xmm15 (SSE2) | v0-v31 (NEON) | f0-f31 (F/D) |
| **Float ABI** | xmm0-xmm7 for args | v0-v7 for args | fa0-fa7 for args |
| **Float Return** | xmm0 | v0 | fa0 |
| **Float Add** | `addsd xmm0, xmm1` | `fadd d0, d0, d1` | `fadd.d fa0, fa0, fa1` |
| **Float Load** | `movsd xmm0, [mem]` | `ld1r d0, [mem]` | `fld fa0, [mem]` |
| **Float Store** | `movsd [mem], xmm0` | `st1r d0, [mem]` | `fsw fa0, [mem]` |
| **Float→Int** | `cvttsd2si rax, xmm0` | `fcvtzs w0, d0` | `fcvtws a0, fa0` |

### Architecture Options

#### Option A: Backend-Driven Codegen (Recommended)
**Approach:** Refactor `ast_lowering.h` to use backend functions

**Pros:**
- Clean separation of concerns
- True multi-ISA support
- Easier to test each backend independently
- Aligns with existing backend infrastructure

**Cons:**
- Requires significant refactoring
- Performance overhead from function calls
- More complex codebase

**Implementation:**
```c
// Current (x86-64 hardcoded):
me_emit_u8(c, 0xF2);
me_emit_u8(c, 0x0F);
me_emit_u8(c, 0x58);
me_emit_u8(c, 0xC1); // addsd xmm0, xmm1

// New (backend-driven):
c->emit_backend->emit_float_add(c, DEST_REG, SRC_REG1, SRC_REG2);
```

#### Option B: IR-Based Approach (LLVM-like)
**Approach:** Emit platform-independent IR, then lower to ISA

**Pros:**
- Maximum portability
- Easy to add optimizations
- Clean separation of frontend/backend

**Cons:**
- Major architectural change
- Memory overhead for IR
- Complex to implement

**Implementation:**
```c
// Emit IR:
IRRef add_ir = zero_cc_ir_emit_binop(IR_FADD, left_ir, right_ir);

// Lower to ISA (backend-specific):
c->emit_backend->emit_ir(c, add_ir);
```

#### Option C: Conditional Compilation (Simplest)
**Approach:** `#ifdef __aarch64__` blocks for each ISA

**Pros:**
- Minimal refactoring
- No runtime overhead
- Direct control per ISA

**Cons:**
- Code duplication
- Hard to maintain
- Can't cross-compile easily

**Implementation:**
```c
#ifdef __x86_64__
    me_emit_u8(c, 0xF2); // x86-64 SSE2
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x58);
#elif defined(__aarch64__)
    me_emit_u32(c, 0x4E208400); // ARM64 fadd d0, d0, d1
#endif
```

### Recommendation

**Short-term (Current Phase 1):** Use **Option C** (conditional compilation)
- Minimal risk
- Quick to implement
- Works for x86-64 and ARM64

**Long-term (Future):** Migrate to **Option A** (backend-driven)
- Leverages existing backend infrastructure
- Cleaner architecture
- Better for maintenance

---

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

## Phase 1: Quick Wins - Detailed Plan (ISA-Aware)

### Task 1.1: Use xmm1/v0 Instead of Stack (2 hours)

**Files:** `src/zero_cc/parser/ast_lowering.h`

**Current Code (x86-64 only):**
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
    }
    /* ... */
}
```

**New Code (ISA-aware):**
```c
case NODE_BINOP: {
    lower_expr(c, arena, node->left);
    bool left_is_float = c->last_expr_is_float;
    
    if (left_is_float) {
#ifdef __x86_64__
        me_emit_u8(c, 0xF2);
        me_emit_u8(c, 0x0F);
        me_emit_u8(c, 0x10);
        me_emit_u8(c, 0xC9); /* movsd xmm1, xmm0 */
#elif defined(__aarch64__)
        me_emit_u32(c, 0x5E000401); /* fmov v1.2d, v0.2d */
#elif defined(__riscv)
        me_emit_u32(c, 0x00151053); /* fmvd fa1, fa0 (RV32) */
#endif
    } else {
        me_push_rax(c);
    }
    /* ... */
}
```

**ARM64 Float Instructions (for reference):**
```c
// Float load: ldr d0, [xN] or ld1r d0, [xN]
// Float store: str d0, [xN] or st1r d0, [xN]
// Float add: fadd d0, d0, d1
// Float sub: fsub d0, d0, d1
// Float mul: fmul d0, d0, d1
// Float div: fdiv d0, d0, d1
// Float→int: fcvtzs w0, d0
// Int→float: scvtf d0, w0
```

### Task 1.2: Add Float Comparisons (3 hours)

**Files:** `src/zero_cc/parser/ast_lowering.h`

**New Code (ISA-aware):**
```c
if (left_is_float && right_is_float) {
    switch (node->aux.op) {
        case TK_PLUS:
#ifdef __x86_64__
            me_emit_u8(c, 0xF2); me_emit_u8(c, 0x0F); me_emit_u8(c, 0x58); me_emit_u8(c, 0xC1);
#elif defined(__aarch64__)
            me_emit_u32(c, 0x4E208400); // fadd d0, d0, d1
#elif defined(__riscv)
            me_emit_u32(c, 0x00152053); // fadd.d fa0, fa0, fa1
#endif
            break;
        
        case TK_LT: // float comparison
#ifdef __x86_64__
            me_emit_u8(c, 0x66); me_emit_u8(c, 0x0F); me_emit_u8(c, 0x2F); me_emit_u8(c, 0xC9); // ucomisd
            // ... set flags ...
#elif defined(__aarch64__)
            me_emit_u32(c, 0x6E20E400); // fcmp d0, d1
            me_emit_u32(c, 0x1E644000); // cset w0, lt
#elif defined(__riscv)
            me_emit_u32(c, 0x00157053); // fsgnjx.d (for comparison)
            // ... more complex on RISC-V ...
#endif
            break;
        
        /* ... other operations ... */
    }
    c->last_expr_is_float = 1;
}
```

### Task 1.3: Add Float Unary Negation (1 hour)

**New Code (ISA-aware):**
```c
case NODE_UNOP: {
    if (node->aux.op == TK_MINUS) {
        lower_expr(c, arena, node->left);
        if (c->last_expr_is_float) {
#ifdef __x86_64__
            // XOR sign bit: xorps xmm0, xmm0; subsd xmm0, xmm0 (or use sign mask)
            me_emit_u8(c, 0x66); me_emit_u8(c, 0x0F); me_emit_u8(c, 0x54); me_emit_u8(c, 0xC0);
            // Actually simpler: load -0.0 and xor, or use subsd xmm0, xmm0 then subsd
#elif defined(__aarch64__)
            me_emit_u32(c, 0x6E208400); // fneg d0, d0
#elif defined(__riscv)
            me_emit_u32(c, 0x00153053); // fneg.d fa0, fa0
#endif
        } else {
            me_emit_u8(c, 0x48); me_emit_u8(c, 0xF7); me_emit_u8(c, 0xD8); // neg rax
        }
    }
    /* ... */
}
```

---

## Phase 2: Float Register Tracking - ISA-Aware

### Task 2.1: ISA-Specific Register Names

**File:** `src/zero_cc/types/symbols.h`

```c
// Add register name tables per ISA
typedef struct {
    const char *int_regs[16];
    const char *float_regs[16];
    const char *int_arg_regs[6];
    const char *float_arg_regs[8];
    const char *int_ret_reg;
    const char *float_ret_reg;
} ISARegisterInfo;

// x86-64
static const ISARegisterInfo x86_64_regs = {
    .int_regs = {"rax", "rbx", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"},
    .float_regs = {"xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15"},
    .int_arg_regs = {"rdi", "rsi", "rdx", "rcx", "r8", "r9"},
    .float_arg_regs = {"xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7"},
    .int_ret_reg = "rax",
    .float_ret_reg = "xmm0",
};

// ARM64
static const ISARegisterInfo arm64_regs = {
    .int_regs = {"x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15"},
    .float_regs = {"v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v8", "v9", "v10", "v11", "v12", "v13", "v14", "v15"},
    .int_arg_regs = {"x0", "x1", "x2", "x3", "x4", "x5"},
    .float_arg_regs = {"v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7"},
    .int_ret_reg = "x0",
    .float_ret_reg = "v0",
};
```

### Task 2.2: ISA Detection Macro

```c
// Add to compiler initialization
#ifdef __x86_64__
    c->isa = ISA_X86_64;
    c->reg_info = &x86_64_regs;
#elif defined(__aarch64__)
    c->isa = ISA_ARM64;
    c->reg_info = &arm64_regs;
#elif defined(__riscv)
    c->isa = ISA_RISCV;
    c->reg_info = &riscv_regs;
#endif
```

---

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
