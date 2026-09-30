# Directed Acyclic Graph (DAG) Source Refactoring Plan

## Overview

This document analyzes the include dependency structure of the zero_cc C compiler and provides recommendations for maintaining a clean Directed Acyclic Graph (DAG) of header dependencies.

**Key Principle**: When files A and B both need the same functionality, extract it to a common file C. Now A→C and B→C instead of A↔B circular dependency.

## Current State Analysis

### ✅ What's Already Good

The codebase follows DAG patterns in most critical areas:

#### 1. Semantic Layer (Clean DAG)
```
stmt.h ──┐
         ├─→ visitor.h ──→ scope.h
expr.h ──┤                type.h
         └────────────────→ promote.h
                          diagnostics.h
```
- Both `stmt.h` and `expr.h` depend on common headers
- No circular dependencies between them ✅

#### 2. Types Layer (Clean Facade)
```
types_lexer.h (facade)
    ├─→ token.h
    ├─→ type_system.h
    ├─→ symbols.h
    ├─→ type_funcs.h
    ├─→ registry.h
    ├─→ emit_helpers.h
    └─→ symbol_mgmt.h
```
- No `types/*.h` file includes back to `types_lexer.h` ✅
- Clean one-way dependency ✅

#### 3. JIT → Semantic Layer (Clean DAG)
```
compile.h ──┐
compile_unit.h ──┤
execution.h ────┤
type_info.h ────┤
                ↓
        semantic/*.h (scope, type, decl, etc.)
```
- JIT headers depend on semantic headers
- Semantic headers do NOT depend on JIT headers ✅

### ⚠️ Areas for Potential Improvement

#### 1. types_lexer.h is a "God Header"

**Problem**: Includes 7 other headers, creating unnecessary coupling.

```c
// types_lexer.h
#include "types/token.h"
#include "types/type_system.h"
#include "types/symbols.h"
#include "types/type_funcs.h"
#include "types/registry.h"
#include "types/emit_helpers.h"
#include "types/symbol_mgmt.h"
```

**Impact**: Any file including `types_lexer.h` gets ALL types, even if it only needs one.

**Recommendation**: Split into logical groups (see Plan below).

#### 2. JIT Headers Duplicate Includes

**Problem**: Multiple JIT headers include the same semantic headers.

```c
// compile.h
#include "zero_cc/semantic/decl.h"
#include "zero_cc/semantic/type.h"
#include "zero_cc/semantic/scope.h"
#include "zero_cc/semantic/diagnostics.h"
#include "zero_cc/semantic/visitor.h"

// compile_unit.h (same includes!)
#include "zero_cc/semantic/decl.h"
#include "zero_cc/semantic/type.h"
#include "zero_cc/semantic/scope.h"
#include "zero_cc/semantic/diagnostics.h"
#include "zero_cc/semantic/visitor.h"

// execution.h (same includes!)
#include "zero_cc/semantic/decl.h"
#include "zero_cc/semantic/type.h"
#include "zero_cc/semantic/scope.h"
#include "zero_cc/semantic/diagnostics.h"
```

**Impact**: Redundant parsing, slower compilation.

**Recommendation**: Create a common `semantic/context.h` header.

#### 3. statements.h → expressions.h Chain

**Problem**: Long include chain for simple cases.

```
statements.h → expressions.h → parser.h → ...
```

**Impact**: Files that only need statement parsing still get full expression parsing.

**Recommendation**: Use forward declarations where possible.

#### 4. expressions.h and statements.h Coupling

**Current State**:
```c
// statements.h
#include "zero_cc/expressions.h"  // Includes everything from expressions
```

**Impact**: Statement parser depends on full expression parser.

**Recommendation**: Consider if this coupling is necessary or if interfaces can be abstracted.

## Refactoring Plan

### Phase 1: Split types_lexer.h (Low Risk)

**Goal**: Reduce unnecessary header inclusion.

**Steps**:

1. Create `types/core.h`:
   ```c
   // types/core.h
   #include "types/token.h"
   #include "types/type_system.h"
   ```

2. Create `types/symbols.h` (renamed from existing):
   ```c
   // types/symbols.h
   #include "types/symbols.h"  // existing
   #include "types/symbol_mgmt.h"
   ```

3. Create `types/emit.h`:
   ```c
   // types/emit.h
   #include "types/emit_helpers.h"
   #include "types/registry.h"
   #include "types/type_funcs.h"
   ```

4. Keep `types_lexer.h` as a backward-compatible facade:
   ```c
   // types_lexer.h (facade)
   #include "types/core.h"
   #include "types/symbols.h"
   #include "types/emit.h"
   ```

**Files to Update**:
- Files that include `types_lexer.h` → include specific sub-headers
- Keep `types_lexer.h` for backward compatibility

**Risk**: Low - only affects files that include types headers.

**Testing**: All existing tests should pass.

### Phase 2: Create semantic/context.h (Low Risk)

**Goal**: Reduce duplicate includes in JIT headers.

**Steps**:

1. Create `semantic/context.h`:
   ```c
   // semantic/context.h
   #ifndef ZERO_CC_SEMANTIC_CONTEXT_H
   #define ZERO_CC_SEMANTIC_CONTEXT_H

   #include "semantic/decl.h"
   #include "semantic/type.h"
   #include "semantic/scope.h"
   #include "semantic/diagnostics.h"
   #include "semantic/visitor.h"
   #include "semantic/promote.h"

   #endif
   ```

2. Update JIT headers:
   ```c
   // compile.h
   #include "zero_cc/semantic/context.h"  // Instead of 5 separate includes

   // compile_unit.h
   #include "zero_cc/semantic/context.h"

   // execution.h
   #include "zero_cc/semantic/context.h"
   ```

**Files to Update**:
- `src/zero_cc/jit/compile.h`
- `src/zero_cc/jit/compile_unit.h`
- `src/zero_cc/jit/execution.h`
- `src/zero_cc/jit/type_info.h`
- `src/zero_cc/jit/elf_emitter.h`

**Risk**: Low - only affects JIT headers.

**Testing**: All existing tests should pass.

### Phase 3: Optimize statements.h (Medium Risk)

**Goal**: Reduce unnecessary expression parser coupling.

**Current**:
```c
// statements.h includes full expressions.h
#include "zero_cc/expressions.h"
```

**Option A - Forward Declarations**:
```c
// statements.h
typedef struct ZeroCCCompiler ZeroCCCompiler;
typedef uint32_t ASTRef;

// Only include what's needed for declarations
#include "zero_cc/parser.h"
```

**Option B - Keep as-is**:
If statement parsing genuinely needs expression parsing, keep the dependency.

**Recommendation**: Keep as-is unless there's a performance problem.

**Risk**: Medium - could break statement parsing if dependencies are incomplete.

**Testing**: Requires full test suite validation.

### Phase 4: Monitor and Iterate (Ongoing)

**Metrics to Track**:

1. **Compilation Time**:
   ```bash
   time make clean && time make test
   ```

2. **Header Dependencies**:
   ```bash
   gcc -M src/zero_cc/parser.c  # Show all dependencies
   ```

3. **Include Depth**:
   ```bash
   grep -r "#include" src/zero_cc | wc -l  # Count includes
   ```

**Triggers for Further Refactoring**:

- Compilation time increases by >20%
- Changing one header breaks unrelated code
- New circular dependencies detected
- Code review identifies confusing include patterns

## Benefits of DAG Structure

### 1. Faster Compilation
```
Before: A → B → C → D (4 files parsed)
After:  A → C, B → C (3 files parsed)
```

### 2. Easier Maintenance
```
Before: Change in C breaks A and B
After:  Change in C only breaks files that directly include C
```

### 3. No Circular Dependencies
```
Before: A ↔ B (can't determine compile order)
After:  A → C ← B (clear compile order)
```

### 4. Better IDE Support
- "Go to definition" works reliably
- Code completion is accurate
- Refactoring tools work correctly

## Implementation Checklist

### Phase 1: types_lexer.h Split
- [ ] Create `types/core.h`
- [ ] Create `types/symbols.h` (consolidated)
- [ ] Create `types/emit.h`
- [ ] Update `types_lexer.h` as facade
- [ ] Update files that include types headers
- [ ] Run full test suite
- [ ] Measure compilation time

### Phase 2: semantic/context.h
- [ ] Create `semantic/context.h`
- [ ] Update `compile.h`
- [ ] Update `compile_unit.h`
- [ ] Update `execution.h`
- [ ] Update `type_info.h`
- [ ] Update `elf_emitter.h`
- [ ] Run full test suite
- [ ] Measure compilation time

### Phase 3: statements.h Optimization
- [ ] Analyze actual dependencies
- [ ] Consider forward declarations
- [ ] Test with minimal includes
- [ ] Revert if problems found

### Phase 4: Documentation
- [ ] Update include_refactor_plan.md
- [ ] Add dependency diagrams to README
- [ ] Document include patterns for new code

## Reference: Successful Refactoring Example

### ir_emitter.h (Completed)

**Before**:
```
compile.h ←→ ast_lowering.h (circular!)
    ↓              ↓
  Both need IR buffer, regalloc, temp tracking
```

**After**:
```
compile.h ──┐
            ├─→ ir_emitter.h (common state)
ast_lowering.h ──┘
```

**Results**:
- ✅ No circular dependencies
- ✅ All 70+ tests pass
- ✅ Clear separation of concerns
- ✅ Easier to maintain

## Conclusion

The current codebase is in good shape with no critical circular dependencies. The refactoring plan above focuses on:

1. **Low-risk improvements** (splitting god headers)
2. **Reducing redundancy** (common context headers)
3. **Monitoring for problems** (compilation time, dependency depth)

**Key Principle**: Refactor when there's a concrete problem, not just for theoretical cleanliness. The ir_emitter.h refactoring succeeded because it solved an actual coupling problem, not just to reduce header count.

---

**Last Updated**: 2024
**Status**: Phase 1-2 recommended, Phase 3 optional, Phase 4 ongoing
