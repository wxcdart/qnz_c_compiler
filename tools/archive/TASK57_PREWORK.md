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

#### 2.2 Pattern Matching Examples
```c
// Fold constant arithmetic
if (instr->op == OP_ADD && ir_is_constant(src1) && ir_is_constant(src2)) {
    int result = ir_get_constant(src1) + ir_get_constant(src2);
    replace_with_const(instr, result);
}

// Algebraic simplification
if (instr->op == OP_ADD && ir_is_constant(src2) && ir_get_constant(src2) == 0) {
    // x + 0 → x
    replace_with_mov(instr, src1);
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
// Backward pass: compute liveness
// Forward pass: eliminate dead instructions
// Compact IR array
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
// Build expression hash table
// Detect duplicate computations
// Replace with copy from existing
```

---

### Phase 5: Integration (Day 5-6)

#### 5.1 Connect to Compiler
```c
// In ast_lowering.c, between regalloc and machine code emission:
opt_init(OPT_LEVEL_O1);
opt_run_passes(c);
opt_shutdown();
```

---

### Phase 6: Testing (Day 6-7)

#### 6.1 Test Cases
```c
// Constant folding
"int x = 2 + 3; return x;"  // Should compile to: return 5;

// DCE
"int x = 5; int y = 10; return x;"  // Should eliminate: y = 10

// CSE  
"int x = a + b; int y = a + b; return x + y;"  // Compute a+b once
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
| `ir.h` | Add OP_DEAD opcode |
| `ast_lowering.c` | Integrate optimization passes |

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

## Current Status (Post-Implementation)

**Date**: 2024-10-04

**Infrastructure Complete**: ✅ All 8 files created in `src/zero_cc/parser/`

**Passes Status**:
- Constant folding: ⚠️ Disabled (needs testing)
- DCE: ⚠️ Disabled (needs testing)
- CSE: ⚠️ Disabled (needs testing)

**Tests**: ✅ All 118 tests passing (with passes disabled)

**Next Steps**:
1. Enable constant folding pass and test
2. Enable DCE pass and test
3. Enable CSE pass and test
4. Proceed to Task 58 (loop optimizations)

**Note**: Task 58 can proceed with passes disabled. The framework is ready.

---

*Last updated: 2024-10-04*
*Scheduled for: Sprint 8 (Weeks 30-33)*
