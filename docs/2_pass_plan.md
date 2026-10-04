# Two-Pass Register Allocation Integration Plan with Optimization Levels

## Status: Phases 1–3 Complete ✅

**Current Progress**: Two-pass IR and Register Allocation fully integrated and passing test suite.
- [x] Phase 1: IR buffer (64K array, 8-byte instructions)
- [x] Phase 2: Pass 1 (Instruction Selection: AST → IR)
- [x] Phase 3: Register Allocation integration (Linear-scan, GPR/XMM classes, spill lowering)
- [ ] Phase 4: Pass 2 (Code Emission completion across all targets)
- [ ] Phase 5: Optimization passes (-O1 to -O3)
- [ ] Phase 6: Testing and benchmarks

**See**: `2_pass_progress.md` for latest status

## Overview

Implement a two-pass code generation approach that separates **instruction selection** from **register assignment**, enabling optimal register allocation **and** multiple optimization levels (-O0, -O1, -O2, -O3) similar to GCC/Clang.

---

## GCC Optimization Levels Reference

| Level | What It Does | Our Implementation |
|-------|--------------|--------------------|
| **-O0** | No optimization, fastest compile | Single-pass, no register allocation |
| **-O1** | Basic optimizations | Two-pass, register allocation, dead code elimination |
| **-O2** | Aggressive optimizations | + Loop unrolling, function inlining, common subexpression elimination |
| **-O3** | Maximum optimization | + Auto-vectorization (SSE/AVX), aggressive inlining |

---

## Architecture with Optimization Pipeline

```
┌─────────────────────────────────────────────────────────────────────┐
│  OPTIMIZATION PIPELINE                                              │
│  ─────────────────────────────────────────────────────────────────  │
│                                                                    │
│  AST                                                                │
│    ↓                                                                │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │ PASS 1: Instruction Selection (AST → IR)                    │   │
│  │ - Generate intermediate instructions with temps             │   │
│  │ - Track liveness                                            │   │
│  └─────────────────────────────────────────────────────────────┘   │
│    ↓                                                                │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │ OPTIMIZATION PASS (level-dependent)                         │   │
│  │ -O0: Skip                                                   │   │
│  │ -O1: Dead code elimination, constant propagation            │   │
│  │ -O2: + Loop unrolling, CSE, inlining                        │   │
│  │ -O3: + Vectorization, aggressive inlining                   │   │
│  └─────────────────────────────────────────────────────────────┘   │
│    ↓                                                                │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │ REGISTER ALLOCATION                                         │   │
│  │ - Linear-scan algorithm                                     │   │
│  │ - Spill code insertion                                      │   │
│  └─────────────────────────────────────────────────────────────┘   │
│    ↓                                                                │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │ PASS 2: Code Emission (IR → Machine Code)                   │   │
│  │ - Emit instructions using allocated registers               │   │
│  │ - Generate spill code for spilled temps                     │   │
│  └─────────────────────────────────────────────────────────────┘   │
│    ↓                                                                │
│  Machine Code                                                       │
└─────────────────────────────────────────────────────────────────────┘
```

### What Works ✅
- **Pass 1 (Instruction Selection)**: AST → Intermediate instructions with temp variables
- **Temp Tracking**: `temp_vars[]` array tracks live ranges
- **Register Allocator**: Linear-scan algorithm in `regalloc.h`
- **Live Interval Computation**: Start/end positions computed during lowering

### What's Missing ❌
- **Pass 2 (Code Emission)**: Intermediate instructions → Machine code with registers
- **Spill Code Generation**: Load/store for spilled temporaries
- **Register Usage**: Allocated registers never used in emitted code

### Code Size
- **298** `zero_cc_emit_*` calls in `ast_lowering.h`
- **1096** lines total in lowering code
- **~50** different instruction patterns (mov, add, sub, call, etc.)

---

## Optimization Levels Implementation

### -O0 (No Optimization)
**Purpose**: Fastest compilation, debugging

```c
// Single-pass, direct code generation
void compile_O0(ZeroCCCompiler *c, ZeroCCASTArena *arena, ASTRef root) {
    // Skip register allocation
    // Skip optimization passes
    // Direct emission to text_section
    lower_expr_direct(c, arena, root);  // Old single-pass code
}
```

**Characteristics**:
- ✅ Fastest compilation (2-3x faster than -O2)
- ✅ Easier debugging (code matches source line-by-line)
- ❌ Slowest generated code (heavy stack usage)
- ❌ No register allocation

---

### -O1 (Basic Optimization)
**Purpose**: Good balance of speed and code quality

```c
void compile_O1(ZeroCCCompiler *c, ZeroCCASTArena *arena, ASTRef root) {
    // Pass 1: Generate IR
    lower_expr_pass1(c, arena, root);
    
    // Optimization: Dead code elimination
    optimize_dead_code_elimination();
    
    // Optimization: Constant propagation
    optimize_constant_propagation();
    
    // Register allocation
    regalloc_allocate(&c->regalloc);
    
    // Pass 2: Emit code
    emit_machine_code();
}
```

**Optimizations included**:
1. **Dead Code Elimination** - Remove instructions whose results are never used
2. **Constant Propagation** - Replace variables with known constant values
3. **Register Allocation** - Linear-scan algorithm

**Characteristics**:
- ✅ 2-5x faster generated code vs -O0
- ✅ Fast compilation (only 20% slower than -O0)
- ✅ Good for development builds

---

### -O2 (Aggressive Optimization)
**Purpose**: Production builds, maximum performance

```c
void compile_O2(ZeroCCCompiler *c, ZeroCCASTArena *arena, ASTRef root) {
    // Pass 1: Generate IR
    lower_expr_pass1(c, arena, root);
    
    // All -O1 optimizations
    optimize_dead_code_elimination();
    optimize_constant_propagation();
    
    // NEW: Common Subexpression Elimination (CSE)
    optimize_common_subexpression_elimination();
    
    // NEW: Loop Unrolling
    optimize_loop_unrolling(4);  // Unroll factor
    
    // NEW: Copy Propagation
    optimize_copy_propagation();
    
    // NEW: Strength Reduction
    optimize_strength_reduction();
    
    // Register allocation
    regalloc_allocate(&c->regalloc);
    
    // Pass 2: Emit code
    emit_machine_code();
}
```

**Additional optimizations**:
1. **Common Subexpression Elimination (CSE)** - Detect and eliminate redundant computations
2. **Loop Unrolling** - Reduce branch overhead (factor of 4)
3. **Copy Propagation** - Replace copies with original values
4. **Strength Reduction** - Replace expensive ops (multiply by shift)

**Characteristics**:
- ✅ 5-10x faster generated code vs -O0
- ✅ Production-ready performance
- ⚠️ 2-3x slower compilation than -O0

---

### -O3 (Maximum Optimization)
**Purpose**: Extreme performance, scientific computing

```c
void compile_O3(ZeroCCCompiler *c, ZeroCCASTArena *arena, ASTRef root) {
    // All -O2 optimizations
    compile_O2_base(c, arena, root);
    
    // NEW: Auto-vectorization (SSE/AVX)
    optimize_vectorization();
    
    // NEW: Aggressive function inlining
    optimize_inlining(aggressive = true);
    
    // NEW: Loop fusion
    optimize_loop_fusion();
}
```

**Additional optimizations**:
1. **Auto-Vectorization** - Convert scalar ops to SIMD (SSE/AVX)
2. **Aggressive Function Inlining** - Inline small functions at call site
3. **Loop Fusion** - Combine adjacent loops with same bounds

**Characteristics**:
- ✅ 10-20x faster generated code vs -O0
- ✅ Best for number-crunching, scientific code
- ❌ Slowest compilation (5-10x slower than -O0)
- ❌ Largest binary size

---

## Optimization Flags in ZeroCC

```c
// Compiler state
typedef struct {
    int optimization_level;  // 0, 1, 2, or 3
    bool debug_info;         // -g
    bool inline_functions;   // -finline-functions
    bool vectorize;          // -ftree-vectorize
} ZeroCCCompiler;

// Command line parsing
void parse_args(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-O0") == 0) c.optimization_level = 0;
        else if (strcmp(argv[i], "-O1") == 0) c.optimization_level = 1;
        else if (strcmp(argv[i], "-O2") == 0) c.optimization_level = 2;
        else if (strcmp(argv[i], "-O3") == 0) c.optimization_level = 3;
        else if (strcmp(argv[i], "-g") == 0) c.debug_info = true;
    }
}

// Main compilation entry point
void compile(ZeroCCCompiler *c, ZeroCCASTArena *arena, ASTRef root) {
    switch (c->optimization_level) {
        case 0: compile_O0(c, arena, root); break;
        case 1: compile_O1(c, arena, root); break;
        case 2: compile_O2(c, arena, root); break;
        case 3: compile_O3(c, arena, root); break;
    }
}
```

---

## Optimization Pass Order

```
1. Instruction Selection (Pass 1)
   ↓
2. Dead Code Elimination
   ↓
3. Constant Propagation
   ↓
4. Copy Propagation
   ↓
5. Common Subexpression Elimination (CSE)
   ↓
6. Strength Reduction
   ↓
7. Loop Unrolling
   ↓
8. Register Allocation (linear-scan)
   ↓
9. Peephole Optimization (local cleanup)
   ↓
10. Code Emission (Pass 2)
```

---

## Two-Pass Architecture

```
┌─────────────────────────────────────────────────────────────────────┐
│  PASS 1: Instruction Selection (AST → Intermediate Code)           │
│  ─────────────────────────────────────────────────────────────────  │
│  Input:  AST nodes                                                  │
│  Output: Instruction buffer with temp variable references          │
│                                                                    │
│  For each AST node:                                                │
│    1. Allocate temp variable (alloc_temp)                          │
│    2. Recursively lower children                                   │
│    3. Emit instruction to buffer (not to text_section)             │
│    4. Record live range (live_start = current pos)                 │
│                                                                    │
│  After all nodes:                                                  │
│    - Set live_end for all temps                                    │
│    - Run register allocation                                       │
└─────────────────────────────────────────────────────────────────────┘
                              ↓
┌─────────────────────────────────────────────────────────────────────┐
│  PASS 2: Code Emission (Intermediate Code → Machine Code)          │
│  ─────────────────────────────────────────────────────────────────  │
│  Input:  Instruction buffer + Register allocation results          │
│  Output: Actual x86-64 machine code in text_section                │
│                                                                    │
│  For each instruction in buffer:                                   │
│    1. Check if temps are spilled or in registers                   │
│    2. If register: emit instruction using register                 │
│    3. If spilled: emit load/store + instruction                    │
│    4. Handle control flow (jumps, labels)                          │
└─────────────────────────────────────────────────────────────────────┘
```

---

## Instruction Buffer Design

```c
// Fixed-size array: 64K instructions = 1MB memory
#define MAX_INSTRS 65536

/* Instruction structure - 16 bytes with padding */
typedef struct {
    uint8_t op;           /* Operation code */
    int8_t temp_dest;     /* Destination temp (-1 if none) */
    int8_t temp_src1;     /* Source temp 1 (-1 if none) */
    int8_t temp_src2;     /* Source temp 2 (-1 if none) */
    int32_t imm;          /* Immediate value */
    uint16_t label_idx;   /* Label index for jumps */
} Instr;  /* Total: 16 bytes */

/* Global instruction buffer */
static Instr instrs[MAX_INSTRS];  /* 1MB total */
static size_t instr_count = 0;

/* Emit instruction - O(1) */
static inline void emit_instr(Instr instr) {
    if (instr_count >= MAX_INSTRS) {
        fprintf(stderr, "Error: Function too large (>64K instructions)\n");
        exit(1);
    }
    instrs[instr_count++] = instr;
}

/* Reset for next function - O(1) */
static inline void reset_instrs(void) {
    instr_count = 0;
}
```

**Memory footprint:** 1MB (65536 × 16 bytes)
**Handles:** 99.9% of real-world functions
**Overflow check:** Fails fast with clear error message

---

## Implementation Design

### 1. Instruction Buffer Structure

```c
/* Instruction opcodes */
typedef enum {
    OP_NOP,
    OP_MOV_RAX_RAX,      // mov rax, rax
    OP_MOV_RAX_MEM,      // mov rax, [rbp - offset]
    OP_MOV_MEM_RAX,      // mov [rbp - offset], rax
    OP_ADD_RAX_RCX,      // add rax, rcx
    OP_SUB_RAX_RCX,      // sub rax, rcx
    OP_MUL_RAX_RCX,      // imul rax, rcx
    OP_DIV_RAX_RCX,      // idiv rcx
    OP_PUSH_RAX,         // push rax
    OP_POP_RAX,          // pop rax
    OP_CALL,             // call function
    OP_JMP,              // jmp label
    OP_JE,               // je label
    OP_CMP_RAX_ZERO,     // test rax, rax
    OP_LEA_RAX_MEM,      // lea rax, [rbp - offset]
    OP_Deref_RAX,        // mov rax, [rax]
    OP_NEG_RAX,          // neg rax
    OP_NOT_RAX,          // not rax
    OP_LNOT_RAX,         // test rax, rax; sete al; movzx rax, al
    OP_BNOT_RAX,         // not rax
    OP_RET,              // ret
    OP_LEAVE,            // leave
    OP_STACK_ALIGN,      // sub rsp, imm8
    OP_PUSH_RBP,         // push rbp
    OP_MOV_RBP_RSP,      // mov rbp, rsp
    OP_ADD_RAX_IMM,      // add rax, imm32
    OP_SUB_RAX_IMM,      // sub rax, imm32
    OP_OR_RAX_RCX,       // or rax, rcx
    OP_AND_RAX_RCX,      // and rax, rcx
    OP_XOR_RAX_RCX,      // xor rax, rcx
    OP_SHL_RAX_RCX,      // shl rax, rcx
    OP_SHR_RAX_RCX,      // shr rax, rcx
    OP_CMP_RAX_RCX,      // cmp rax, rcx
    OP_SETL_AL,          // setl al
    OP_SETLE_AL,         // setle al
    OP_SETG_AL,          // setg al
    OP_SETGE_AL,         // setge al
    OP_SETE_AL,          // sete al
    OP_SETNE_AL,         // setne al
} OpCode;

/* Single intermediate instruction */
typedef struct {
    OpCode op;
    int temp_dest;       // Destination temp (-1 if none)
    int temp_src1;       // Source temp 1 (-1 if none)
    int temp_src2;       // Source temp 2 (-1 if none)
    int64_t imm_val;     // Immediate value
    size_t label_idx;    // Label index for jumps
    size_t patch_pos;    // Position to patch (for forward jumps)
} Instr;

/* Instruction buffer */
#define MAX_INSTRS 4096
static Instr instr_buffer[MAX_INSTRS];
static size_t instr_count = 0;
```

### 2. Pass 1: Instruction Emission Functions

```c
/* Emit instruction to buffer instead of text_section */
static void emit_instr(OpCode op) {
    Instr *instr = &instr_buffer[instr_count++];
    instr->op = op;
    instr->temp_dest = -1;
    instr->temp_src1 = -1;
    instr->temp_src2 = -1;
    instr->imm_val = 0;
    instr->label_idx = 0;
    instr->patch_pos = 0;
}

static void emit_instr_imm(OpCode op, int64_t imm) {
    emit_instr(op);
    instr_buffer[instr_count - 1].imm_val = imm;
}

static void emit_instr_temp(OpCode op, int temp) {
    emit_instr(op);
    instr_buffer[instr_count - 1].temp_src1 = temp;
}

static void emit_instr_binop(OpCode op, int dest, int src1, int src2) {
    emit_instr(op);
    instr_buffer[instr_count - 1].temp_dest = dest;
    instr_buffer[instr_count - 1].temp_src1 = src1;
    instr_buffer[instr_count - 1].temp_src2 = src2;
}

static void emit_instr_jump(OpCode op, size_t label_idx, size_t patch_pos) {
    emit_instr(op);
    instr_buffer[instr_count - 1].label_idx = label_idx;
    instr_buffer[instr_count - 1].patch_pos = patch_pos;
}
```

### 3. Pass 2: Code Emission with Register Allocation

```c
/* Emit actual machine code using allocated registers */
static void emit_machine_code(Instr *instr) {
    switch (instr->op) {
        case OP_MOV_RAX_MEM: {
            /* Load from stack slot to rax */
            int temp = instr->temp_src1;
            if (is_temp_spilled(temp)) {
                int slot = temp_vars[temp].spill_slot;
                zero_cc_emit_u8(&c->text_section, 0x48);
                zero_cc_emit_u8(&c->text_section, 0x8B);
                zero_cc_emit_u8(&c->text_section, 0x85);
                zero_cc_emit_u32(&c->text_section, -slot);
            } else {
                int reg = get_temp_reg(temp);
                zero_cc_emit_u8(&c->text_section, 0x48);
                zero_cc_emit_u8(&c->text_section, 0x89);
                zero_cc_emit_u8(&c->text_section, 0xC0 + reg);
            }
            break;
        }
        
        case OP_ADD_RAX_RCX: {
            /* add rax, rcx where both are temps */
            int src1 = instr->temp_src1;
            int src2 = instr->temp_src2;
            
            /* Load src1 into rax if spilled */
            if (is_temp_spilled(src1)) {
                emit_load_from_spill(src1, 0); // Load to rax
            }
            
            /* Load src2 into rcx if spilled */
            if (is_temp_spilled(src2)) {
                emit_load_from_spill(src2, 1); // Load to rcx
            } else {
                int reg2 = get_temp_reg(src2);
                zero_cc_emit_u8(&c->text_section, 0x48);
                zero_cc_emit_u8(&c->text_section, 0x89);
                zero_cc_emit_u8(&c->text_section, 0xC8 + reg2); // mov rax, rcX
            }
            
            /* Perform add */
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0x01);
            zero_cc_emit_u8(&c->text_section, 0xC0 + get_temp_reg(src2));
            
            /* Store result to dest if spilled */
            if (is_temp_spilled(instr->temp_dest)) {
                emit_store_to_spill(instr->temp_dest);
            }
            break;
        }
        
        /* ... more opcodes ... */
    }
}

/* Helper: Load temp from spill slot to register */
static void emit_load_from_spill(int temp, int target_reg) {
    int slot = temp_vars[temp].spill_slot;
    zero_cc_emit_u8(&c->text_section, 0x48);
    zero_cc_emit_u8(&c->text_section, 0x8B);
    zero_cc_emit_u8(&c->text_section, 0x85);
    zero_cc_emit_u32(&c->text_section, -slot);
    /* Result in rax, move to target if needed */
    if (target_reg == 1 && get_temp_reg(temp) != 1) {
        zero_cc_emit_u8(&c->text_section, 0x48);
        zero_cc_emit_u8(&c->text_section, 0x89);
        zero_cc_emit_u8(&c->text_section, 0xC0 + target_reg);
    }
}

/* Helper: Store rax to temp's spill slot */
static void emit_store_to_spill(int temp) {
    int slot = temp_vars[temp].spill_slot;
    zero_cc_emit_u8(&c->text_section, 0x48);
    zero_cc_emit_u8(&c->text_section, 0x89);
    zero_cc_emit_u8(&c->text_section, 0x85);
    zero_cc_emit_u32(&c->text_section, -slot);
}
```

### 4. Modified Lowering Functions

```c
/* Example: NODE_BINOP with two-pass */
static void lower_expr_pass1(ZeroCCCompiler *c, ZeroCCASTArena *arena, ASTRef ref) {
    ZeroCCASTNode *node = zero_cc_ast_get(arena, ref);
    
    switch (node->tag) {
        case NODE_BINOP: {
            int left_temp = alloc_temp(c, REG_CLASS_GPR);
            int right_temp = alloc_temp(c, REG_CLASS_GPR);
            int result_temp = alloc_temp(c, REG_CLASS_GPR);
            
            /* Recursively lower operands */
            lower_expr_pass1(c, arena, node->left);
            lower_expr_pass1(c, arena, node->right);
            
            /* Emit binary operation instruction */
            emit_instr_binop(OP_ADD_RAX_RCX, result_temp, left_temp, right_temp);
            break;
        }
        
        /* ... other cases ... */
    }
}

static void lower_expr_pass2(ZeroCCCompiler *c) {
    for (size_t i = 0; i < instr_count; i++) {
        emit_machine_code(&instr_buffer[i]);
    }
}
```

---

## Implementation Steps

### Phase 1: Infrastructure (Week 1)

1. **Define instruction set** (50 opcodes)
   - [ ] Create `OpCode` enum
   - [ ] Create `Instr` struct
   - [ ] Create instruction buffer
   - [ ] Create emit functions

2. **Modify Pass 1** (instruction selection)
   - [ ] Rename `lower_expr` → `lower_expr_pass1`
   - [ ] Replace `zero_cc_emit_*` with `emit_instr_*`
   - [ ] Track temp assignments for each instruction
   - [ ] Keep control flow (labels, jumps) working

3. **Modify Pass 2** (code emission)
   - [ ] Create `emit_machine_code()` function
   - [ ] Implement register-based emission
   - [ ] Implement spill code generation
   - [ ] Handle all opcodes

### Phase 2: Testing & Debugging (Week 1-2)

4. **Test each opcode**
   - [ ] Binary operations (+, -, *, /, %)
   - [ ] Unary operations (!, ~, -, &)
   - [ ] Function calls
   - [ ] Ternary operator
   - [ ] Control flow (if, while, for)

5. **Test spill scenarios**
   - [ ] Force spills by creating many temps
   - [ ] Verify load/store correctness
   - [ ] Check register pressure handling

### Phase 3: Optimization (Week 2-3)

6. **Optimize spill code**
   - [ ] Coalesce adjacent load/store
   - [ ] Avoid redundant moves
   - [ ] Use callee-saved registers for long-lived temps

7. **Add peephole optimizations**
   - [ ] Remove mov rax, rax
   - [ ] Combine add/sub with immediate
   - [ ] Optimize common patterns

---

## Migration Strategy

### Step 1: Keep Current System Working
- Don't break existing tests
- Implement two-pass alongside current single-pass
- Gradually migrate one node type at a time

### Step 2: Migrate Node Types
1. **NODE_NUM** (simplest)
2. **NODE_BINOP** (core operations)
3. **NODE_UNOP** (unary ops)
4. **NODE_CALL** (function calls)
5. **NODE_TERNARY** (conditional)
6. **NODE_IDENT** (variables)
7. **Statements** (control flow)

### Step 3: Remove Old Code
- Delete single-pass code
- Clean up instruction set
- Document final architecture

---

## Expected Benefits

### Performance
- **10-30x** fewer stack accesses
- **2-5x** faster generated code
- Better register utilization

### Maintainability
- Clear separation of concerns
- Easier to add optimizations
- Simpler register allocator integration

### Future-Ready
- Foundation for:
  - Peephole optimization
  - Constant propagation
  - Dead code elimination
  - Loop optimization

---

## Risks & Mitigations

| Risk | Impact | Mitigation |
|------|--------|------------|
| Instruction set too large | Slow implementation | Start with subset, expand gradually |
| Spill code incorrect | Bugs in generated code | Test with forced spills early |
| Performance regression | Slower compilation | Profile and optimize hot paths |
| Control flow breaks | Jumps/labels broken | Keep label handling simple initially |

---

## Success Criteria

- ✅ All 71 existing tests pass
- ✅ No compiler warnings
- ✅ Generated code runs correctly
- ✅ Register allocation visible in output
- ✅ Spill code generated when needed
- ✅ Code is maintainable and extensible

---

## Timeline

| Phase | Duration | Deliverables |
|-------|----------|--------------|
| Infrastructure | 3 days | Instruction set, buffer, emit functions |
| Pass 1 Migration | 4 days | All AST nodes emit instructions |
| Pass 2 Implementation | 5 days | Machine code emission with registers |
| Testing & Debugging | 5 days | All tests passing |
| Optimization | 3 days | Spill optimization, peephole |
| **Total** | **~3 weeks** | **Production-ready two-pass compiler** |

---

## Next Immediate Steps

1. **Define instruction set** (today)
   - List all 50 opcodes needed
   - Define `Instr` struct fields
   - Create instruction buffer

2. **Implement basic emit functions** (today)
   - `emit_instr()`
   - `emit_instr_binop()`
   - `emit_instr_unop()`

3. **Migrate NODE_BINOP** (tomorrow)
   - Convert to two-pass
   - Test with simple arithmetic
   - Verify correct output

4. **Implement Pass 2 for BINOP** (tomorrow)
   - Register-based emission
   - Spill handling
   - Test with register pressure

5. **Repeat for other node types** (week 1-2)
   - One type per day
   - Test after each migration

---

## Appendix: Complete Instruction Set

### Data Movement
- `OP_MOV_RAX_IMM` - mov rax, imm64
- `OP_MOV_RAX_MEM` - mov rax, [rbp - offset]
- `OP_MOV_MEM_RAX` - mov [rbp - offset], rax
- `OP_MOV_RAX_RBX` - mov rax, rbx (register to register)
- `OP_LEA_RAX_MEM` - lea rax, [rbp - offset]

### Arithmetic
- `OP_ADD_RAX_IMM` - add rax, imm32
- `OP_SUB_RAX_IMM` - sub rax, imm32
- `OP_ADD_RAX_RCX` - add rax, rcx
- `OP_SUB_RAX_RCX` - sub rax, rcx
- `OP_MUL_RAX_RCX` - imul rax, rcx
- `OP_DIV_RAX_RCX` - idiv rcx
- `OP_MOD_RAX_RCX` - idiv rcx, result in rdx

### Logical
- `OP_AND_RAX_RCX` - and rax, rcx
- `OP_OR_RAX_RCX` - or rax, rcx
- `OP_XOR_RAX_RCX` - xor rax, rcx
- `OP_NOT_RAX` - not rax
- `OP_BNOT_RAX` - not rax (bitwise)
- `OP_LNOT_RAX` - !rax

### Shift
- `OP_SHL_RAX_RCX` - shl rax, rcx
- `OP_SHR_RAX_RCX` - shr rax, rcx

### Comparison
- `OP_CMP_RAX_RCX` - cmp rax, rcx
- `OP_CMP_RAX_ZERO` - test rax, rax
- `OP_SETL_AL` - setl al
- `OP_SETLE_AL` - setle al
- `OP_SETG_AL` - setg al
- `OP_SETGE_AL` - setge al
- `OP_SETE_AL` - sete al
- `OP_SETNE_AL` - setne al

### Stack
- `OP_PUSH_RAX` - push rax
- `OP_POP_RAX` - pop rax
- `OP_PUSH_RBX` - push rbx
- `OP_POP_RBX` - pop rbx
- `OP_STACK_ALIGN` - sub rsp, imm8

### Control Flow
- `OP_JMP` - jmp label
- `OP_JE` - je label
- `OP_JNE` - jne label
- `OP_JL` - jl label
- `OP_JLE` - jle label
- `OP_JG` - jg label
- `OP_JGE` - jge label

### Function
- `OP_CALL` - call function
- `OP_CALL_INDIRECT` - call [r11]
- `OP_RET` - ret
- `OP_LEAVE` - leave
- `OP_PUSH_RBP` - push rbp
- `OP_MOV_RBP_RSP` - mov rbp, rsp

### Unary
- `OP_NEG_RAX` - neg rax
- `OP_Deref_RAX` - mov rax, [rax]

### Special
- `OP_NOP` - nop
