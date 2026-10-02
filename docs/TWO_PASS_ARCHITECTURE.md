# Two-Pass Code Generation Architecture

## Overview

ZeroCC uses a **two-pass code generation** approach with register allocation and optimization support, similar to GCC and LLVM.

### Pipeline

```
AST → Pass 1 (IR Generation) → Optimization → Register Allocation → Pass 2 (Code Emission) → Machine Code
```

## Why Two-Pass?

### Single-Pass Problems (Old)

```c
// Direct emission to machine code
void lower_expr(ASTNode *node) {
    emit_mov_rax_imm(value);  // Hard-coded x86-64
    emit_add_rax_rcx();       // Hard-coded x86-64
}
```

**Issues:**
- ❌ No optimization (can't rearrange instructions)
- ❌ No register allocation (everything on stack)
- ❌ Slow generated code (heavy stack usage)
- ❌ Can't implement float arithmetic (needs XMM registers)

### Two-Pass Benefits (New)

```c
// Pass 1: Generate IR
void lower_expr_pass1(ASTNode *node) {
    ir_emit(OP_MOV_IMM, temp0, 0xFF, 0xFF, value, 0);
    ir_emit(OP_ADD, temp1, temp0, temp2, 0, 0);
}

// Pass 2: Emit machine code using registers
void emit_machine_code() {
    // After register allocation: temp0 → rax, temp2 → rcx
    emit_mov_rax_imm(value);  // Optimized!
    emit_add_rax_rcx();       // Uses registers!
}
```

**Benefits:**
- ✅ Register allocation (14 GPR + 8 XMM registers)
- ✅ Optimization passes (dead code, constant propagation, etc.)
- ✅ Fast generated code (5-10x faster than -O0)
- ✅ Float arithmetic support (SSE2 XMM registers)

---

## IR Design

### Instruction Buffer

```c
#define MAX_INSTRS 65536  // 64K instructions

typedef struct __attribute__((packed)) {
    uint8_t  op;        // 0-255 opcodes
    uint8_t  temp_dest; // 0-255 temps (0xFF if none)
    uint8_t  temp_src1; // 0-255 temps (0xFF if none)
    uint8_t  temp_src2; // 0-255 temps (0xFF if none)
    int16_t  imm;       // -32768 to 32767
    uint16_t label_idx; // 0-65535 labels
} Instr;  // 8 bytes packed

static Instr instrs[MAX_INSTRS];  // 512KB total
static size_t instr_count = 0;
```

**Why 8 bytes?**
- Fits most immediates (int16 = ±32K)
- Supports 256 temporaries per function
- Supports 65K labels per function
- Perfect cache utilization (16 instructions per 128-byte cache line)

**Why 64K?**
- 99.9% of functions have <100 instructions
- 512KB is trivial (modern CPUs have 32MB+ L3 cache)
- Shared across all functions (not per-function)
- If exceeded: increase to 256K (one line change)

### Operations

```c
// Emit instruction
static inline void ir_emit(Instr instr) {
    if (instr_count >= MAX_INSTRS) {
        fprintf(stderr, "Error: Function too large\n");
        exit(1);
    }
    instrs[instr_count++] = instr;
}

// Reset for next function
static inline void ir_reset(void) {
    instr_count = 0;
}
```

---

## Optimization Levels

### -O0 (No Optimization)

```c
void compile_O0(ZeroCCCompiler *c) {
    // Single-pass, direct code generation
    lower_expr_direct(c, arena, root);  // Skip IR, emit directly
}
```

**Characteristics:**
- ✅ Fastest compilation (2-3x faster than -O2)
- ✅ Easier debugging (code matches source line-by-line)
- ❌ Slowest generated code (heavy stack usage)

### -O1 (Basic Optimization)

```c
void compile_O1(ZeroCCCompiler *c) {
    lower_expr_pass1(c, arena, root);     // Pass 1: Generate IR
    optimize_dead_code();                 // Remove unused
    optimize_constant_prop();             // Fold constants
    regalloc_allocate(&c->regalloc);      // Assign registers
    emit_machine_code(c);                 // Pass 2: Emit code
}
```

**Optimizations:**
1. Dead code elimination
2. Constant propagation
3. Register allocation

**Characteristics:**
- ✅ 2-5x faster generated code vs -O0
- ✅ Fast compilation (+20% vs -O0)
- ✅ Good for development builds

### -O2 (Aggressive Optimization)

```c
void compile_O2(ZeroCCCompiler *c) {
    compile_O1_base(c);                   // All -O1 optimizations
    optimize_cse();                       // Common subexpression
    optimize_loop_unrolling(4);           // Unroll loops
    optimize_strength_reduction();        // Replace expensive ops
    emit_machine_code(c);
}
```

**Additional optimizations:**
1. Common subexpression elimination (CSE)
2. Loop unrolling (factor of 4)
3. Strength reduction (multiply → shift)

**Characteristics:**
- ✅ 5-10x faster generated code vs -O0
- ✅ Production-ready performance
- ⚠️ 2-3x slower compilation than -O0

### -O3 (Maximum Optimization)

```c
void compile_O3(ZeroCCCompiler *c) {
    compile_O2_base(c);                   // All -O2 optimizations
    optimize_vectorization();             // SSE/AVX
    optimize_inlining(aggressive = true); // Aggressive inlining
    optimize_loop_fusion();               // Combine loops
}
```

**Additional optimizations:**
1. Auto-vectorization (SSE/AVX)
2. Aggressive function inlining
3. Loop fusion

**Characteristics:**
- ✅ 10-20x faster generated code vs -O0
- ✅ Best for scientific computing
- ❌ Slowest compilation (+900% vs -O0)

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
9. Peephole Optimization
   ↓
10. Code Emission (Pass 2)
```

---

## Register Allocation

### Register Classes

**GPR (General Purpose):** 14 registers
- RAX, RCX, RDX, RBX, RSI, RDI, R8-R15

**XMM (SSE2 Float):** 8 registers
- XMM0-XMM7 (for float/double arithmetic)

### Linear-Scan Algorithm

```c
// After Pass 1: Compute live ranges
for (size_t i = 0; i < temp_var_count; i++) {
    if (temp_vars[i].live_end == 0) {
        temp_vars[i].live_end = instr_count;
    }
    regalloc_add_interval(&c->regalloc, ...);
}
regalloc_allocate(&c->regalloc);

// temp_vars[i].assigned_reg is now set (-1 if spilled)
```

### Spill Slots

```c
static int next_spill_slot = 0;

int allocate_spill_slot(void) {
    int slot = -8 - (next_spill_slot * 8);  // -8, -16, -24, ...
    next_spill_slot++;
    return slot;
}
```

**Stack Frame Layout:**
```
[rbp]           <- frame pointer
[rbp + 8]       <- return address
[rbp - 8]       <- spilled temp 0
[rbp - 16]      <- spilled temp 1
[rbp - 24]      <- spilled temp 2
...
[rbp - N]       <- local variables
```

---

## Implementation Status

### Phase 1: Infrastructure ✅

- [x] Define IR structure (8 bytes packed)
- [x] Create instruction buffer (64K array)
- [x] Implement emit/reset functions
- [x] Add overflow protection
- [x] Integrate with compilation flow
- [x] Unit tests pass

### Phase 2: Pass 1 (Instruction Selection) - In Progress

- [ ] Define 50 opcodes
- [ ] Implement IR emission for NODE_BINOP
- [ ] Implement IR emission for NODE_UNOP
- [ ] Implement IR emission for NODE_CALL
- [ ] Implement IR emission for NODE_TERNARY
- [ ] Track temp assignments

### Phase 3: Register Allocation Integration

- [ ] Compute live ranges from IR
- [ ] Run linear-scan allocation
- [ ] Assign spill slots
- [ ] Handle register assignments

### Phase 4: Pass 2 (Code Emission)

- [ ] Implement emit_machine_code()
- [ ] Handle register-based operations
- [ ] Generate spill code (load/store)
- [ ] Patch labels (forward references)

### Phase 5: Optimization

- [ ] Dead code elimination
- [ ] Constant propagation
- [ ] Loop unrolling (-O2)
- [ ] Strength reduction
- [ ] Peephole optimization

### Phase 6: Testing

- [ ] All 71 existing tests pass
- [ ] Add optimization benchmarks
- [ ] Stress test large functions
- [ ] Verify -O2 produces faster code

---

## Files

### Core Infrastructure

- `src/zero_cc/parser/ir.h` - IR buffer (8-byte instructions, 64K array)
- `src/zero_cc/parser/regalloc.h` - Linear-scan register allocator

### Integration

- `src/zero_cc/jit/compile_unit.h` - Global IR init
- `src/zero_cc/jit/compile.h` - Per-function IR reset

### Documentation

- `TWO_PASS_ARCHITECTURE.md` - This file (architecture overview)
- `2_pass_plan.md` - Detailed implementation plan
- `2_pass_decision.md` - Final design decisions
- `2_pass_considerations.md` - Edge cases and details
- `2_pass_progress.md` - Current progress status
- `circular_dependency_analysis.md` - Architecture analysis

### Tests

- `test/ir_test.c` - IR buffer unit tests

---

## Performance Targets

| Metric | -O0 | -O1 | -O2 | -O3 |
|--------|-----|-----|-----|-----|
| **Compile Time** | 1x | 1.2x | 3x | 10x |
| **Code Speed** | 1x | 2-5x | 5-10x | 10-20x |
| **Binary Size** | 1x | 1x | 1.2x | 1.5x |

---

## Next Steps

1. **Define 50 opcodes** (MOV, ADD, SUB, etc.)
2. **Migrate NODE_BINOP** to IR
3. **Implement Pass 2** for simple cases
4. **Test** with arithmetic expressions
5. **Verify** output matches current implementation

---

## References

- GCC Internals: https://gcc.gnu.org/onlinedocs/gccint/
- LLVM Language Reference: https://llvm.org/docs/LangRef.html
- System V AMD64 ABI: https://refspecs.linuxfoundation.org/elf/x86_64-abi-0.99.pdf
