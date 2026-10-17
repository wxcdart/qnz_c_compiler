# IR ISA-Agnostic Refactoring Plan

## Current State: x86-64 Hardcoded in IR

The IR layer has **8 critical x86-64 assumptions** that break AArch64/RISC-V:

### 1. RIP-Relative Addressing (OP_LEA_RIP, OP_MOV_GLOBAL)

**Problem:**
```c
case OP_LEA_RIP: {
    size_t rodata_offset = (size_t)instr->imm64;
    if (BE(c)->emit_lea_rip) {
        BE(c)->emit_lea_rip(c, phys_dst, rodata_offset);  // x86: lea rax, [rip+offset]
    }
    break;
}
```

**x86-64:** `lea rax, [rip + 0x1234]` - single instruction
**AArch64:** Needs `adr x0, .+8` + relocation patching
**RISC-V:** Needs `auipc t0, %pcrel_hi` + `addi t0, t0, %pcrel_lo`

**Solution:**
```c
/* Replace with ISA-neutral memory operation */
typedef enum {
    IR_MEM_STACK,      /* [fp - offset] - stack slot */
    IR_MEM_GLOBAL,     /* symbol - global variable */
    IR_MEM_RODATA,     /* rodata_offset - string literal */
    IR_MEM_BASE,       /* [base + offset] - pointer deref */
} IRMemMode;

typedef struct {
    uint8_t temp_dest;
    IRMemMode mode;
    union {
        int32_t stack_offset;
        const char *global_symbol;
        size_t rodata_offset;
        struct { int base_reg; int32_t offset; };
    };
} IRLoadInst;
```

### 2. Stack Operations (OP_PUSH_RAX, OP_POP_RAX)

**Problem:**
```c
case OP_PUSH_RAX:
    if (BE(c)->emit_push_reg) BE(c)->emit_push_reg(c, phys_src1);
    break;
case OP_POP_RAX:
    if (BE(c)->emit_pop_reg) BE(c)->emit_pop_reg(c, phys_src1);
    break;
```

**x86-64:** `push rax`, `pop rax` - single instructions
**AArch64:** `str x0, [sp, #-8]!` or `stp` pair
**RISC-V:** `sd x0, -8(sp)` + `addi sp, sp, -8`

**Solution:**
```c
/* Make stack operations ISA-neutral */
typedef enum {
    IR_STACK_PUSH,
    IR_STACK_POP,
    IR_STACK_ALLOC,   /* sub sp, sp, #size */
    IR_STACK_DEALLOC, /* add sp, sp, #size */
} IRStackOp;

/* Backend implements stack operation */
void (*emit_stack_push)(ZeroCCCompiler *c, int reg);
void (*emit_stack_pop)(ZeroCCCompiler *c, int reg);
void (*emit_stack_alloc)(ZeroCCCompiler *c, size_t size);
```

### 3. Flags/Condition Codes (OP_TEST, OP_SETL, etc.)

**Problem:**
```c
case OP_TEST:
    if (BE(c)->emit_test) BE(c)->emit_test(c, phys_src1, phys_src2);
    break;
case OP_SETL:
    if (BE(c)->emit_setl) BE(c)->emit_setl(c, phys_dst);
    break;
```

**x86-64:** `test rax, rax` sets ZF/SF/OF, then `setl al` reads flags
**AArch64:** `cmp x0, x0` sets NZCV, then `csel` or conditional branch
**RISC-V:** `beqz/bnez` branches directly, no setcc instruction

**Solution:**
```c
/* Replace setcc with ISA-neutral compare+branch pattern */
typedef enum {
    IR_CMP_EQ,
    IR_CMP_NE,
    IR_CMP_LT,
    IR_CMP_LE,
    IR_CMP_GT,
    IR_CMP_GE,
    IR_CMP_LT_U,  /* unsigned */
    IR_CMP_LE_U,
    IR_CMP_GT_U,
    IR_CMP_GE_U,
} IRCmpKind;

/* Emit compare and branch directly */
void ir_emit_cmp_branch(IRCmpKind kind, uint8_t temp1, uint8_t temp2, uint16_t label);
void ir_emit_cmp_set(IRCmpKind kind, uint8_t temp_dest, uint8_t temp1, uint8_t temp2);

/* Backend handles ISA-specific implementation */
void (*emit_cmp_and_branch)(ZeroCCCompiler *c, IRCmpKind kind, int src1, int src2, const char *label);
void (*emit_cmp_and_set)(ZeroCCCompiler *c, IRCmpKind kind, int dst, int src1, int src2);
```

### 4. Division/Modulo (OP_DIV, OP_MOD, OP_CQO, OP_IDIV)

**Problem:**
```c
case OP_DIV:
    if (BE(c)->emit_idiv) {
        BE(c)->emit_cqo(c);       /* x86: cqo sign-extends rax into rdx:rax */
        BE(c)->emit_idiv(c, src); /* x86: idiv rsi */
    }
    break;
```

**x86-64:** `cqo` + `idiv rsi` (uses rax input, rdx:rax dividend)
**AArch64:** `sdiv x0, x0, x1` (single instruction)
**RISC-V:** `div x0, x0, x1` (single instruction)

**Solution:**
```c
/* Single IR opcode for division */
case OP_DIV:
    if (BE(c)->emit_div) {
        BE(c)->emit_div(c, phys_dst, phys_src1, phys_src2);
    }
    break;

/* Backend handles ISA-specific sequence */
/* x86-64: emit_cqo() + emit_idiv() */
/* AArch64/RISC-V: single sdiv instruction */
```

### 5. Register-Specific Operations (OP_LEA_RAX_MEM)

**Problem:**
```c
case OP_LEA_RAX_MEM:
    /* Load effective address into RAX */
    if (BE(c)->emit_lea) {
        BE(c)->emit_lea(c, 0 /* rax */, phys_src1, instr->imm);
    }
    break;
```

**x86-64:** `lea rax, [rbp - 8]` - address computation
**AArch64:** `add x0, x29, #-8` (uses frame pointer)
**RISC-V:** `addi a0, s0, -8`

**Solution:**
```c
/* Remove RAX-specific opcodes */
case OP_LEA:
    if (BE(c)->emit_lea) {
        BE(c)->emit_lea(c, phys_dst, phys_src1, instr->imm);
    }
    break;

/* Make all operations use temp registers, not hardcoded RAX */
```

### 6. Immediate Size Limitations (int16_t imm)

**Problem:**
```c
typedef struct __attribute__((packed)) {
    uint8_t  op;
    uint8_t  temp_dest;
    uint8_t  temp_src1;
    uint8_t  temp_src2;
    int16_t  imm;       /* Only 16-bit! */
    int64_t  imm64;     /* 64-bit for pointers/symbols */
    uint16_t label_idx;
} Instr;
```

**x86-64:** `add rax, imm8` or `add rax, imm32` (sign-extended)
**AArch64:** `add x0, x1, #imm12` (12-bit immediate, shifted)
**RISC-V:** `addi` uses 12-bit signed immediate

**Solution:**
```c
/* Keep int16_t for small immediates (common case) */
/* For large immediates, emit sequence: */
void ir_emit_mov_large_imm(uint8_t temp_dest, int64_t imm) {
    if (imm >= INT16_MIN && imm <= INT16_MAX) {
        ir_emit_mov_imm(temp_dest, imm);
    } else {
        /* Backend emits ISA-specific sequence:
         * x86-64: movabs rax, imm64
         * AArch64: movz + movk sequence
         * RISC-V: lui + addi sequence
         */
        ir_emit((Instr){.op = OP_MOV_LARGE_IMM, .temp_dest = temp_dest, .imm64 = imm});
    }
}
```

### 7. Floating-Point Stack vs Registers

**Problem:**
```c
case OP_FLOAD:
    if (BE(c)->emit_load_fp) {
        BE(c)->emit_load_fp(c, phys_dst, BE(c)->fp_reg, -offset);
    }
    break;
```

**x86-64:** SSE2 `movsd xmm0, [rbp - offset]`
**AArch64:** `ldr d0, [x29, -offset]`
**RISC-V:** `fld f0, -offset(s0)`

**Solution:**
```c
/* Already mostly neutral - uses backend fp_reg */
/* Just need to ensure backends set fp_reg correctly */
/* x86-64: fp_reg = base for stack, but uses XMM for values */
/* AArch64: fp_reg = x29, fp values in v0-v31 */
/* RISC-V: fp_reg = s0, fp values in f0-f31 */
```

### 8. Calling Convention Hardcoding

**Problem:**
```c
/* In ast_lowering_mc.c */
int n_stack = (arg_count > 6) ? arg_count - 6 : 0;  /* Hardcoded 6! */
for (int a = arg_count - 1; a >= 6; a--) {
    BE(c)->emit_push_reg(c, r);  /* x86-64 calling convention */
}
```

**x86-64 System V:** 6 int args (rdi-rsi-rdx-rcx-r8-r9), 8 FP args (xmm0-xmm7)
**AArch64 AAPCS64:** 8 int args (x0-x7), 8 FP args (v0-v7)
**RISC-V:** 8 int args (a0-a7), 8 FP args (fa0-fa7)

**Solution:**
```c
/* Use backend calling convention tables */
int n_stack = 0;
for (int a = 0; a < arg_count; a++) {
    int reg = backend_get_int_arg_reg(BE(c), a);
    if (reg < 0) {
        /* Stack-passed argument */
        n_stack++;
    }
}
/* Push stack args in reverse order */
```

## Refactoring Priority

### Phase 1: Critical (Remove x86 Hardcoding)

| Change | Files | Effort |
|--------|-------|--------|
| Remove OP_PUSH_RAX/POP_RAX, use generic stack ops | ir.h, ast_lowering_mc.c | 2 days |
| Replace OP_LEA_RAX_MEM with OP_LEA (any reg) | ir.h, ast_lowering_expr.c | 1 day |
| Remove OP_CQO/OP_IDIV, use single OP_DIV | ir.h, ast_lowering_mc.c | 1 day |
| Fix calling convention to use backend tables | ast_lowering_mc.c | 2 days |

### Phase 2: High (Add ISA-Neutral Operations)

| Change | Files | Effort |
|--------|-------|--------|
| Replace OP_TEST+SETCC with CMP_BRANCH | ir.h, ast_lowering_stmt.c | 3 days |
| Add IRMemMode for load/store abstraction | ir.h, ast_lowering_expr.c | 3 days |
| Add OP_MOV_LARGE_IMM for >16-bit immediates | ir.h, all backends | 2 days |

### Phase 3: Medium (Backend Integration)

| Change | Files | Effort |
|--------|-------|--------|
| Update x86_64.c to new IR | src/zero_cc/isa/x86_64.c | 2 days |
| Implement AArch64 with new IR | src/zero_cc/isa/aarch64.c | 5 days |
| Implement RISC-V with new IR | src/zero_cc/isa/riscv.c | 5 days |

### Phase 4: Low (Optimization)

| Change | Files | Effort |
|--------|-------|--------|
| Add SIMD IR opcodes (for vectorization) | ir.h | 2 days |
| Add atomic memory operation IR | ir.h | 2 days |

## Total Effort: 28-32 days for full ISA-agnostic IR

## Quick Wins (1-2 days each)

1. **Remove hardcoded "6" for register args**
   ```c
   /* Before */
   int n_stack = (arg_count > 6) ? arg_count - 6 : 0;
   
   /* After */
   int int_arg_count = BE(c)->conv->int_arg_reg_count;
   int n_stack = (arg_count > int_arg_count) ? arg_count - int_arg_count : 0;
   ```

2. **Remove OP_PUSH_RAX/POP_RAX, use generic**
   ```c
   /* Before */
   case OP_PUSH_RAX: BE(c)->emit_push_reg(c, 0); break;
   case OP_POP_RAX:  BE(c)->emit_pop_reg(c, 0); break;
   
   /* After */
   case OP_PUSH: BE(c)->emit_push_reg(c, phys_src1); break;
   case OP_POP:  BE(c)->emit_pop_reg(c, phys_dest); break;
   ```

3. **Fix OP_DIV to not use OP_CQO**
   ```c
   /* Before */
   case OP_DIV: BE(c)->emit_cqo(); BE(c)->emit_idiv(src); break;
   
   /* After */
   case OP_DIV: BE(c)->emit_div(dst, src1, src2); break;
   ```

## Test Strategy

1. **Unit test each IR opcode** with expected backend output
2. **Compile same C source to all ISAs**, compare execution results
3. **Use differential testing**: x86-64 vs AArch64 vs RISC-V

