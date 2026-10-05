# Task 57 Pre-Work - Optimization Pipeline Readiness

## Task 57: LuaJIT-Style Middle-End Optimizations (-O1 / -O2)

**Effort**: 1 week  
**Goal**: Fast linear-pass constant folding, algebraic simplification, DCE, and CSE

---

## Pre-Check Results

```
✅ Circular Dependencies: PASS
❌ Optimization Infrastructure: FAIL
❌ IR Opcode Coverage: FAIL
❌ Constant Folding: FAIL
❌ DCE Infrastructure: FAIL
❌ CSE Infrastructure: FAIL
❌ Optimization Pass Framework: FAIL
❌ Test Coverage: FAIL
```

**Status**: Task 57 NOT READY - Infrastructure needs to be built

---

## What Needs to Be Built

### Phase 1: Optimization Infrastructure (Day 1-2)

#### 1.1 Optimization Pass Framework
```c
// src/zero_cc/parser/opt_pass.h
typedef enum {
    OPT_LEVEL_NONE = 0,    // -O0
    OPT_LEVEL_O1 = 1,      // -O1: Constant folding, DCE
    OPT_LEVEL_O2 = 2,      // -O2: + CSE, algebraic simplification
    OPT_LEVEL_O3 = 3,      // -O3: + LICM, strength reduction
} OptLevel;

typedef struct OptPass {
    const char *name;
    void (*run)(ZeroCCCompiler *c);
    int enabled_by_default;
} OptPass;

// Pass manager
void opt_init(OptLevel level);
void opt_run_passes(ZeroCCCompiler *c);
void opt_shutdown(void);
```

#### 1.2 Pass List for -O1
- **Constant Folding**: Evaluate constant expressions at compile time
- **Dead Code Elimination**: Remove unused instructions
- **Algebraic Simplification**: x + 0 → x, x * 1 → x, etc.

#### 1.3 Pass List for -O2
- All -O1 passes
- **Common Subexpression Elimination**: Detect and eliminate duplicate computations
- **Strength Reduction**: Replace expensive ops with cheaper ones

---

### Phase 2: Constant Folding (Day 2-3)

#### 2.1 IR-Level Constant Folding
```c
// src/zero_cc/parser/opt_constant_fold.h
void opt_fold_constants(ZeroCCCompiler *c);

// Fold binary operations
int opt_fold_int_binop(int op, int left, int right);
double opt_fold_float_binop(int op, double left, double right);

// Examples:
// OP_ADD: 2 + 3 → OP_CONST 5
// OP_MUL: x * 1 → OP_MOV x (eliminate multiply)
// OP_AND: x & 0 → OP_CONST 0
```

#### 2.2 Pattern Matching
```c
// Detect constant patterns
if (instr->op == OP_ADD && instr->imm_right == 0) {
    // x + 0 → x
    replace_with_mov(instr, instr->temp_left);
}

if (instr->op == OP_MUL && instr->imm_right == 1) {
    // x * 1 → x
    replace_with_mov(instr, instr->temp_left);
}

if (instr->op == OP_MUL && instr->imm_right == 0) {
    // x * 0 → 0
    replace_with_const(instr, 0);
}
```

---

### Phase 3: Dead Code Elimination (Day 3-4)

#### 3.1 Liveness Analysis
```c
// src/zero_cc/parser/opt_dce.h
void opt_eliminate_dead_code(ZeroCCCompiler *c);

// Compute liveness
typedef struct LiveAnalysis {
    bool *live_in;   // Live at instruction entry
    bool *live_out;  // Live at instruction exit
} LiveAnalysis;

void live_analyze(ZeroCCCompiler *c, LiveAnalysis *la);
```

#### 3.2 DCE Algorithm
```c
// Mark live instructions
for (int i = count - 1; i >= 0; i--) {
    Instr *instr = &ir[i];
    
    // If destination is never used, mark as dead
    if (!is_live(instr->temp_dest)) {
        // Check if instruction has side effects
        if (!has_side_effects(instr)) {
            mark_dead(instr);
        }
    }
    
    // Update liveness
    update_liveness(instr, la);
}

// Remove dead instructions
compact_ir();
```

---

### Phase 4: Common Subexpression Elimination (Day 4-5)

#### 4.1 Hash Table for CSE
```c
// src/zero_cc/parser/opt_cse.h
void opt_eliminate_cse(ZeroCCCompiler *c);

// Hash function for IR instructions
uint64_t hash_instruction(Instr *instr);

// Check if expression already computed
int find_existing_cse(ZeroCCCompiler *c, Instr *instr);
```

#### 4.2 CSE Algorithm
```c
// Build expression table
for (int i = 0; i < count; i++) {
    Instr *instr = &ir[i];
    
    // Only pure expressions (no side effects)
    if (is_pure_expression(instr)) {
        uint64_t hash = hash_instruction(instr);
        
        // Check if we've seen this before
        int existing = lookup_cse_table(hash, instr);
        if (existing >= 0) {
            // Replace with copy from existing
            replace_with_copy(instr, existing);
        } else {
            // Add to table
            add_to_cse_table(hash, instr, i);
        }
    }
}
```

---

### Phase 5: Integration (Day 5-6)

#### 5.1 Connect to Compiler
```c
// src/zero_cc/jit/compile.h
// Add optimization level parameter
void compile_function(ZeroCCCompiler *c, OptLevel opt_level);

// Run optimization passes
void compile_function(ZeroCCCompiler *c, OptLevel opt_level) {
    // Pass 1: AST → IR
    zero_cc_lower_ast(c, arena, ast);
    
    // Run optimizations
    opt_init(opt_level);
    opt_run_passes(c);
    
    // Register allocation
    regalloc_analyze(c);
    regalloc_allocate(c);
    
    // Pass 2: IR → Machine Code
    emit_ir_to_machine_code(c, spill_base_offset);
    
    opt_shutdown();
}
```

#### 5.2 Command Line Interface
```c
// Add -O flag support
int main(int argc, char **argv) {
    OptLevel opt_level = OPT_LEVEL_NONE;
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-O0") == 0) {
            opt_level = OPT_LEVEL_NONE;
        } else if (strcmp(argv[i], "-O1") == 0) {
            opt_level = OPT_LEVEL_O1;
        } else if (strcmp(argv[i], "-O2") == 0) {
            opt_level = OPT_LEVEL_O2;
        } else if (strcmp(argv[i], "-O3") == 0) {
            opt_level = OPT_LEVEL_O3;
        }
    }
    
    compile_function(c, opt_level);
}
```

---

### Phase 6: Testing (Day 6-7)

#### 6.1 Constant Folding Tests
```c
// test/unit/opt_constant_fold.c
void test_fold_add_constants() {
    const char *src = "int x = 2 + 3; return x;";
    // Should compile to: return 5;
}

void test_fold_mul_by_one() {
    const char *src = "int x = 5; return x * 1;";
    // Should compile to: return x;
}
```

#### 6.2 DCE Tests
```c
// test/unit/opt_dce.c
void test_eliminate_unused() {
    const char *src = "int x = 5; int y = 10; return x;";
    // Should eliminate: y = 10 (unused)
}
```

#### 6.3 CSE Tests
```c
// test/unit/opt_cse.c
void test_eliminate_duplicate() {
    const char *src = "int x = a + b; int y = a + b; return x + y;";
    // Should compute a + b once, reuse result
}
```

---

## Files to Create

| File | Purpose | Lines |
|------|---------|-------|
| `opt_pass.h` | Optimization pass framework | 50 |
| `opt_pass.c` | Pass manager implementation | 100 |
| `opt_constant_fold.h` | Constant folding API | 30 |
| `opt_constant_fold.c` | Constant folding implementation | 200 |
| `opt_dce.h` | DCE API | 30 |
| `opt_dce.c` | DCE implementation | 150 |
| `opt_cse.h` | CSE API | 30 |
| `opt_cse.c` | CSE implementation | 200 |

**Total**: ~790 lines of new code

---

## Files to Modify

| File | Changes |
|------|---------|
| `ir.h` | Add optimization-related declarations |
| `ir.c` | Add optimization helper functions |
| `compile.h` | Add OptLevel parameter |
| `jit/compile.h` | Integrate optimization passes |
| `Makefile` | Add new .o files |

---

## Verification Checklist

After Task 57 implementation:

```
[ ] Constant folding works (2 + 3 → 5)
[ ] Algebraic simplification works (x + 0 → x)
[ ] DCE removes unused code
[ ] CSE eliminates duplicate computations
[ ] -O1 flag enables basic optimizations
[ ] -O2 flag enables all -O1 + CSE
[ ] All 118 tests still pass
[ ] Optimization tests added and passing
[ ] No performance regression at -O0
```

---

## Pre-Work Tools

Run these before starting Task 57:

```bash
# Update dependency graph
python3 tools/generate_deps.py src > deps_graph.json

# Check for circular dependencies
python3 tools/task57_precheck.py

# Review current IR structure
cat src/zero_cc/parser/ir.h | grep "OP_"

# Review register allocator (needed for DCE)
cat src/zero_cc/parser/regalloc.h
```

---

## Next Steps

1. ✅ deps_graph.json updated
2. ✅ task57_precheck.py created
3. ⏳ Review optimization algorithms (LuaJIT docs, LLVM docs)
4. ⏳ Start Task 57 implementation
5. ⏳ Run tests after each phase

**Estimated Time**: 5-7 days
**Risk**: Medium - Optimizations can introduce subtle bugs
**Mitigation**: Test after each optimization pass
