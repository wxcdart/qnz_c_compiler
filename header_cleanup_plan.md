# Header Cleanup Plan - Move Code to .c Files

## Executive Summary

**Problem**: Our codebase has 98 non-inline static functions and static variables in headers. This causes:
- Duplicate copies of state in each translation unit (same bug as `ir.h` had!)
- Wasted code space
- Potential subtle bugs
- Slower compilation

**Solution**: Move static functions and variables from headers to `.c` files, following the pattern established with `ir_emitter.h`.

## Current State Analysis

### ✅ What's Correct (Should Stay in Headers)

| Pattern | Count | Status |
|---------|-------|--------|
| `static inline` functions | 410 | ✅ OK - each .c gets its own copy, no linking issues |
| Function declarations | Many | ✅ OK - this is what headers are for |
| Struct/typedef definitions | Many | ✅ OK - needed for compilation |
| Macros | Many | ✅ OK - must be in headers (preprocessor) |

### ❌ What's Wrong (Should Move to .c)

| File | Issue | Impact |
|------|-------|--------|
| `statements.h` | **static variables** (label_table, label_count, etc.) | **CRITICAL** - Same bug as ir.h had! |
| `statements.h` | 19 static function implementations | Wasted code, potential bugs |
| `ast_lowering.h` | 19 static functions | Some could move to .c |
| `ir_emitter.h` | 14 static functions | Some could move to .c |

## Critical Issue: statements.h Static Variables

### The Bug (Same as ir.h had!)

```c
// statements.h - EACH .c FILE GETS ITS OWN COPY!
static struct {
    char name[64];
    size_t position;
} label_table[256];

static int label_count = 0;
static size_t* goto_patch_list[256];
static int goto_patch_list_count = 0;
static int block_depth = 0;
```

**Impact**: If multiple `.c` files include `statements.h`, each gets its own `label_table`. This means:
- Labels defined in one file aren't visible to another
- `label_count` is different in each file
- goto/label functionality is broken!

### Why This Doesn't Crash (Yet)

Currently, only `parser.c` includes `statements.h` directly, so there's only one copy. But this is a ticking time bomb:
- If we split parser.c into multiple files → broken
- If we add another .c file that includes statements.h → broken
- The bug is latent, not manifest

## Refactoring Plan

### Phase 1: Fix statements.h (CRITICAL - High Priority)

**Goal**: Move static variables and functions to `statements.c`

**Pattern**: Follow `ir_emitter.h` structure

#### Step 1: Create `statements.c`

```c
// src/zero_cc/statements.c
#include "zero_cc/statements.h"

/* Global label state - SINGLE COPY */
static struct {
    char name[MAX_LABEL_NAME];
    size_t position;
} label_table[MAX_LABELS];

static int label_count = 0;
static size_t* goto_patch_list[MAX_LABELS];
static int goto_patch_list_count = 0;
static int block_depth = 0;

/* Implementations of static functions */
static void reset_label_state(void) {
    label_count = 0;
    goto_patch_list_count = 0;
}

static int find_label(const char *name) {
    for (int i = 0; i < label_count; i++) {
        if (strcmp(label_table[i].name, name) 0) {
            return i;
        }
    }
    return -1;
}

static int add_label(const char *name, size_t position) {
    if (label_count >= MAX_LABELS) return -1;
    strncpy(label_table[label_count].name, name, MAX_LABEL_NAME - 1);
    label_table[label_count].name[MAX_LABEL_NAME - 1] = '\0';
    label_table[label_count].position = position;
    return label_count++;
}

/* Export functions that need to be called from other files */
void statements_reset_label_state(void) {
    reset_label_state();
}

int statements_find_label(const char *name) {
    return find_label(name);
}

int statements_add_label(const char *name, size_t position) {
    return add_label(name, position);
}

/* Include modular handlers (they call the functions above) */
#include "statements/control.h"
#include "statements/definitions.h"
#include "statements/variables.h"
```

#### Step 2: Update `statements.h`

```c
// src/zero_cc/statements.h
#ifndef ZERO_CC_STATEMENTS_H
#define ZERO_CC_STATEMENTS_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Forward declarations */
typedef struct ZeroCCCompiler ZeroCCCompiler;

/* Goto/Label support - moved to statements.c */
#define MAX_LABELS 256
#define MAX_LABEL_NAME 64

/* Exported functions (renamed to avoid conflicts) */
void statements_reset_label_state(void);
int statements_find_label(const char *name);
int statements_add_label(const char *name, size_t position);

/* Forward declarations for inline helpers */
static inline void zero_cc_stmt_decl(ZeroCCCompiler *c, ZeroCCTokType t, size_t decl_align_spec);
static inline void zero_cc_stmt_var_incdec(ZeroCCCompiler *c, char *var_name, bool is_inc);
// ... other inline helpers ...

/* Main statement parser */
void zero_cc_parse_stmt(ZeroCCCompiler *c);

/* Include modular headers (they call exported functions) */
#include "statements/control.h"
#include "statements/definitions.h"
#include "statements/variables.h"

#endif
```

#### Step 3: Update `parser.c`

```c
// src/zero_cc/parser.c
#include "zero_cc/statements.h"

/* Replace direct calls to static functions */
/* Before: reset_label_state(); */
/* After:  statements_reset_label_state(); */

/* Before: int idx = find_label(name); */
/* After:  int idx = statements_find_label(name); */
```

#### Step 4: Update `Makefile`

```makefile
# Add statements.o to compilation
OBJS += src/zero_cc/statements.o
```

**Files to Modify**:
- [ ] Create `src/zero_cc/statements.c`
- [ ] Update `src/zero_cc/statements.h`
- [ ] Update `src/zero_cc/parser.c`
- [ ] Update `Makefile`
- [ ] Run all tests

**Risk**: Medium - requires careful testing of goto/label functionality

**Testing**:
- `test/break_continue` - tests loop control
- `test/goto_forward` - tests forward goto
- `test/goto_loop` - tests goto in loops
- `test/goto_simple` - tests basic goto

### Phase 2: Clean ast_lowering.h (Medium Priority)

**Goal**: Move non-essential static functions to `ast_lowering.c`

**Current**: 19 static functions in header

**Strategy**:
1. Identify which static functions are only used within `ast_lowering.h`
2. Move them to `ast_lowering.c`
3. Keep inline helpers in header (they're small and used frequently)

**Files to Modify**:
- [ ] Create `src/zero_cc/parser/ast_lowering.c`
- [ ] Move static functions to .c file
- [ ] Update `parser.c` if needed

**Risk**: Low - we already refactored state to `ir_emitter.h`

### Phase 3: Optimize ir_emitter.h (Low Priority)

**Goal**: Move some static functions to `ir.c`

**Current**: 14 static functions in header

**Strategy**:
1. Identify functions that don't need to be inline
2. Move them to `ir.c`
3. Keep performance-critical functions inline

**Files to Modify**:
- [ ] Move non-critical static functions to `ir.c`
- [ ] Profile to ensure no performance regression

**Risk**: Low - `ir_emitter.h` is already well-structured

## What Should NOT Move to .c

### Keep in Headers:

1. **`static inline` functions** (410 total)
   - Each .c gets its own copy
   - No linking issues
   - Good for small helpers

2. **Function declarations**
   - This is what headers are for!

3. **Struct/typedef definitions**
   - Needed for compilation

4. **Macros**
   - Preprocessor substitution
   - Must be visible at compile time

5. **`static const` data**
   - Each .c gets its own copy (OK)
   - Often compiler-optimized anyway

## Benefits of This Cleanup

### 1. Fix Critical Bug
- No more duplicate static variables
- Labels work correctly across translation units

### 2. Faster Compilation
```
Before: Each .c includes full statements.h (372 lines)
After:  Each .c includes declarations only (~50 lines)
```

### 3. Easier Debugging
```
Before: State scattered across multiple .c files
After:  State in one place (statements.c)
```

### 4. Better Maintainability
```
Before: Change in statements.h affects all includers
After:  Change in statements.c only affects callers
```

### 5. Clearer Separation
```
Before: Declarations and implementations mixed
After:  Clear .h (what) vs .c (how) separation
```

## Implementation Checklist

### Phase 1: statements.h → statements.c ✅ COMPLETED

**Status**: Successfully completed!

**Changes Made**:
- [x] Create `src/zero_cc/statements.c`
- [x] Move static variables to statements.c (label_table, label_count, goto_patch_list, block_depth)
- [x] Move static function implementations to statements.c (reset_label_state, find_label, add_label)
- [x] Update statements.h to use declarations only
- [x] Add forward declarations for zero_cc_parse_expr and zero_cc_eval_const_expr
- [x] Include expressions.h in statements.h for static inline definitions
- [x] Update parser.c (no changes needed - includes via zero_cc.h)
- [x] Update Makefile to compile statements.c
- [x] Run `make clean && make test`
- [x] Verify goto/label tests pass (goto_forward, goto_loop, goto_simple, goto_test, break_continue, switch_case)

**Result**: All 70+ tests pass. Static variable bug fixed!

### Phase 2: ast_lowering.h cleanup
- [ ] Analyze which static functions can move
- [ ] Create `src/zero_cc/parser/ast_lowering.c`
- [ ] Move functions to .c file
- [ ] Run tests
- [ ] Measure compilation time improvement

### Phase 3: ir_emitter.h optimization
- [ ] Profile to identify non-critical functions
- [ ] Move to `ir.c`
- [ ] Verify no performance regression
- [ ] Run tests

### Phase 4: Documentation
- [ ] Update include_refactor_plan.md
- [ ] Update DAG_source_refactoring.md
- [ ] Add coding guidelines for new code

## Coding Guidelines for Future Code

### When Writing New Headers:

1. **Prefer declarations over definitions**
   ```c
   // ✅ GOOD
   void my_function(int x);
   
   // ❌ BAD (unless static inline)
   void my_function(int x) { ... }
   ```

2. **Never put static variables in headers**
   ```c
   // ❌ BAD
   static int counter = 0;
   
   // ✅ GOOD
   extern int counter;  // In header
   int counter = 0;     // In .c file
   ```

3. **Use static inline for small helpers**
   ```c
   // ✅ OK (small, performance-critical)
   static inline int add(int a, int b) {
       return a + b;
   }
   ```

4. **Move large implementations to .c**
   ```c
   // ✅ GOOD (header)
   void complex_algorithm(int *data, size_t n);
   
   // ✅ GOOD (.c file)
   void complex_algorithm(int *data, size_t n) {
       // Large implementation here
   }
   ```

## References

- [ir_emitter.h refactoring](include_refactor_plan.md) - Pattern we followed
- [DAG_source_refactoring.md](DAG_source_refactoring.md) - Include dependency analysis
- [Linux kernel coding style](https://www.kernel.org/doc/html/latest/process/coding-style.html) - Industry standard

## Conclusion

**Priority Order**:
1. **Phase 1 (statements.h)** - CRITICAL - fixes actual bug
2. **Phase 2 (ast_lowering.h)** - Medium - cleanup
3. **Phase 3 (ir_emitter.h)** - Low - optimization

**Key Insight**: The `ir_emitter.h` refactoring showed us the right pattern. Apply it to `statements.h` to fix the same bug before it causes problems.

---

**Last Updated**: 2024
**Status**: Phase 1 recommended (critical), Phases 2-3 optional
