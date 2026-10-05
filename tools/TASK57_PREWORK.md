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
#ifndef ZERO_CC_PARSER_OPT_PASS_H
#define ZERO_CC_PARSER_OPT_PASS_H

#include <stdint.h>
#include <stdbool.h>

typedef struct ZeroCCCompiler ZeroCCCompiler;

typedef enum {
    OPT_LEVEL_NONE = 0,    // -O0: No optimizations
    OPT_LEVEL_O1 = 1,      // -O1: Constant folding, DCE
    OPT_LEVEL_O2 = 2,      // -O2: + CSE, algebraic simplification
    OPT_LEVEL_O3 = 3,      // -O3: + LICM, strength reduction
} OptLevel;

typedef struct OptPass {
    const char *name;
    void (*run)(ZeroCCCompiler *c);
    int enabled_by_default;
    int priority;  // Lower = run first
} OptPass;

// Pass manager
void opt_init(OptLevel level);
void opt_run_passes(ZeroCCCompiler *c);
void opt_shutdown(void);
OptLevel opt_get_level(void);

#endif /* ZERO_CC_PARSER_OPT_PASS_H */
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
#ifndef ZERO_CC_PARSER_OPT_CONSTANT_FOLD_H
#define ZERO_CC_PARSER_OPT_CONSTANT_FOLD_H

void opt_fold_constants(ZeroCCCompiler *c);

#endif
```

```c
// src/zero_cc/parser/opt_constant_fold.c
#include "opt_constant_fold.h"
#include "ir.h"
#include "regalloc.h"

void opt_fold_constants(ZeroCCCompiler *c) {
    // Iterate through IR and fold constant expressions
    // Example: OP_ADD with two constants → OP_CONST
}
```

#### 2.2 Pattern Matching Examples
```c
// Detect constant patterns
if (instr->op == OP_ADD && is_constant(instr->temp_left) && is_constant(instr->temp_right)) {
    int left = get_constant(instr->temp_left);
    int right = get_constant(instr->temp_right);
    int result = left + right;
    replace_with_const(instr, result);
}

// Algebraic simplification
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
#ifndef ZERO_CC_PARSER_OPT_DCE_H
#define ZERO_CC_PARSER_OPT_DCE_H

void opt_eliminate_dead_code(ZeroCCCompiler *c);

#endif
```

#### 3.2 DCE Algorithm
```c
// src/zero_cc/parser/opt_dce.c
#include "opt_dce.h"
#include "ir.h"

void opt_eliminate_dead_code(ZeroCCCompiler *c) {
    // Backward pass: compute liveness
    // Forward pass: eliminate dead instructions
    // Compact IR array
}
```

---

### Phase 4: Common Subexpression Elimination (Day 4-5)

#### 4.1 Hash Table for CSE
```c
// src/zero_cc/parser/opt_cse.h
#ifndef ZERO_CC_PARSER_OPT_CSE_H
#define ZERO_CC_PARSER_OPT_CSE_H

void opt_eliminate_cse(ZeroCCCompiler *c);

#endif
```

#### 4.2 CSE Algorithm
```c
// src/zero_cc/parser/opt_cse.c
#include "opt_cse.h"
#include "ir.h"
#include "../common/hash_table_tpht.h"

void opt_eliminate_cse(ZeroCCCompiler *c) {
    // Build expression hash table
    // Detect duplicate computations
    // Replace with copy from existing
}
```

---

### Phase 5: Integration (Day 5-6)

#### 5.1 Connect to Compiler
```c
// Update src/zero_cc/jit/compile.h
void compile_function(ZeroCCCompiler *c, OptLevel opt_level);

// Run optimization passes between Pass 1 and Pass 2
void compile_function(ZeroCCCompiler *c, OptLevel opt_level) {
    // Pass 1: AST → IR
    zero_cc_lower_ast(c, arena, ast);
    
    // Run optimizations (between Pass 1 and Pass 2)
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
#include <stdio.h>
#include <stdlib.h>
#include "zero_cc.h"

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

## Files to Create (in src/zero_cc/parser/)

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
| `Makefile` | Add opt_*.o to EXTRA_OBJS |
| `ir.h` | Add optimization-related declarations |
| `ir.c` | Add optimization helper functions |
| `jit/compile.h` | Add OptLevel parameter |
| `compile.h` | Integrate optimization passes |

### Makefile Changes

Add to `EXTRA_OBJS` in Makefile:
```makefile
EXTRA_OBJS = $(addprefix $(OBJ_DIR)/, \
    ...
    src/zero_cc/parser/ir.o \
    src/zero_cc/parser/opt_pass.o \
    src/zero_cc/parser/opt_constant_fold.o \
    src/zero_cc/parser/opt_dce.o \
    src/zero_cc/parser/opt_cse.o \
    ...
)
```

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
3. ✅ TASK57_PREWORK.md created
4. ⏳ Review optimization algorithms (LuaJIT docs, LLVM docs)
5. ⏳ Start Task 57 implementation
6. ⏳ Run tests after each phase

**Estimated Time**: 5-7 days  
**Risk**: Medium - Optimizations can introduce subtle bugs  
**Mitigation**: Test after each optimization pass

---

*Last updated: 2024-10-04*
*Scheduled for: Sprint 8 (Weeks 30-33)*

---

## Current Status (Post-Implementation)

**Date**: 2024-10-04

**Infrastructure Complete**: ✅ All 8 files created in `src/zero_cc/parser/`

**Passes Status**:
- Constant folding: ❌ Disabled (corrupts IR)
- DCE: ❌ Disabled (needs better analysis)
- CSE: ❌ Disabled (needs work)

**Tests**: ✅ All 118 tests passing (with passes disabled)

**Next Steps**:
1. Fix constant folding to not corrupt IR
2. Improve DCE side-effect analysis
3. Enable passes incrementally with testing
4. Add optimization-specific test cases

---

*Implementation note: Infrastructure is complete but optimization algorithms need debugging before enabling.*
