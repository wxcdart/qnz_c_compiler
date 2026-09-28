# Parser.c Refactoring Plan ✅ COMPLETED

## Goal
Split the massive `src/zero_cc/parser.c` file (2,232 lines) into smaller, logically-grouped sub-files while preserving the public API. The original `parser.c` will be replaced with a wrapper that includes the new sub-files.

---

## ✅ Status: COMPLETED

All five sub-files have been successfully created. Build passes and 39/40 tests pass (1 pre-existing failure unrelated to this work).

---

## 📊 Actual Results

| File | Planned | Actual |
|------|---------|--------|
| `parser/ast_arena.h` | ~70 lines | 82 lines |
| `parser/ast_expr.h` | ~560 lines | 584 lines |
| `parser/ast_stmt.h` | ~610 lines | 632 lines |
| `parser/ast_dump.h` | ~240 lines | 254 lines |
| `parser/ast_lowering.h` | ~750 lines | 769 lines |

**Total:** 2,232 lines → 2,321 lines (includes + headers overhead)

---

## 🧪 Test Results

```
Build: ✅ Clean (no errors)
Tests: 39/40 PASS
  ❌ rodata_demo_jit (pre-existing segfault, unrelated to parser refactoring)
```

---

## 📋 Proposed Split

### 1️⃣ `src/zero_cc/parser/ast_arena.h` (~70 lines)

| Line Range | Content |
|------------|---------|
| 1-68 | AST arena initialization, allocation, freeing, and node creation |

**Functions:**
- `zero_cc_ast_arena_init()`
- `zero_cc_ast_arena_free()`
- `zero_cc_ast_arena_reset()`
- `zero_cc_ast_alloc_node()`
- `zero_cc_ast_get()`

**Guard:** `ZERO_CC_PARSER_AST_ARENA_H`

---

### 2️⃣ `src/zero_cc/parser/ast_expr.h` (~560 lines)

| Line Range | Content |
|------------|---------|
| 69-630 | Expression AST parser with recursive descent for all operator precedence levels |

**Functions:**
- `parse_primary_ast()` - Literals, identifiers, function calls, casts
- `parse_postfix_ast()` - Array indexing, member access, increment/decrement
- `parse_unary_ast()` - Unary operators (+, -, !, ~, *, &)
- `parse_multiplicative_ast()` - *, /, %
- `parse_additive_ast()` - +, -
- `parse_shift_ast()` - <<, >>
- `parse_relational_ast()` - <, >, <=, >=
- `parse_equality_ast()` - ==, !=
- `parse_bit_and_ast()` - &
- `parse_bit_xor_ast()` - ^
- `parse_bit_or_ast()` - |
- `parse_logical_and_ast()` - &&
- `parse_logical_or_ast()` - ||
- `parse_ternary_ast()` - cond ? a : b
- `parse_assignment_ast()` - =, +=, -=, etc.
- `parse_expr_ast_internal()` - Internal expression parser
- `zero_cc_parse_expr_ast()` - Public entry point

**Guard:** `ZERO_CC_PARSER_AST_EXPR_H`

---

### 3️⃣ `src/zero_cc/parser/ast_stmt.h` (~610 lines)

| Line Range | Content |
|------------|---------|
| 631-1239 | Statement AST parser for all C statement types |

**Functions:**
- `zero_cc_parse_stmt_ast()` - Main statement parser
- Handles: compound blocks, return, if/else, while, do-while, for, switch, case, default, break, continue, static assert, variable/typedef declarations, function definitions/prototypes, expression statements

**Guard:** `ZERO_CC_PARSER_AST_STMT_H`

---

### 4️⃣ `src/zero_cc/parser/ast_dump.h` (~240 lines)

| Line Range | Content |
|------------|---------|
| 1240-1478 | AST debugging utilities |

**Functions:**
- `op_to_str()` - Convert operator token to string
- `zero_cc_ast_dump()` - Pretty print AST to stdout
- `zero_cc_ast_dump_indent()` - Helper for indented output

**Guard:** `ZERO_CC_PARSER_AST_DUMP_H`

---

### 5️⃣ `src/zero_cc/parser/ast_lowering.h` (~750 lines)

| Line Range | Content |
|------------|---------|
| 1479-2232 | AST → machine code generation (x86-64) |

**Functions:**
- `lower_node()` - Main AST lowering function (recursive)
- `lower_expr()` - Expression lowering
- `zero_cc_lower_ast()` - Public entry point for AST lowering

**Features:**
- Loop control flow (break/continue)
- Return epilogue patching
- x86-64 machine code emission

**Guard:** `ZERO_CC_PARSER_AST_LOWERING_H`

---

## 📦 Wrapper File

### `src/zero_cc/parser.c` (after refactoring)

```c
/* =========================================================================
 *  parser.c
 *  C99 AST Parser and Code Generator
 *  ========================================================================= */

#ifndef ZERO_CC_PARSER_H
#define ZERO_CC_PARSER_H

#include "parser/ast_arena.h"
#include "parser/ast_expr.h"
#include "parser/ast_stmt.h"
#include "parser/ast_dump.h"
#include "parser/ast_lowering.h"

#endif /* ZERO_CC_PARSER_H */
```

**Status:** ✅ Implemented

---

## 🔧 Implementation Details

### Forward Declarations Needed

Since `ast_expr.h` and `ast_stmt.h` call each other, we need forward declarations:

**In `ast_expr.h`:**
```c
/* Forward declaration for statement parsing */
static ASTRef zero_cc_parse_stmt_ast(ZeroCCCompiler *c, ZeroCCASTArena *arena);
```

**In `ast_stmt.h`:**
```c
/* Forward declaration for expression parsing */
static ASTRef zero_cc_parse_expr_ast(ZeroCCCompiler *c, ZeroCCASTArena *arena);
static ASTRef parse_expr_ast_internal(ZeroCCCompiler *c, ZeroCCASTArena *arena);
```

### Required Includes

Each sub-file will need:
```c
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "types_lexer.h"
#include "parser.h"
```

### Global Variables

The lowering section uses static global arrays for loop control flow. These should remain in `ast_lowering.h`:

```c
#define MAX_EPILOGUE_PATCHES 256
static size_t epilogue_patches[MAX_EPILOGUE_PATCHES];
static size_t epilogue_patch_count = 0;

#define MAX_LOOP_NESTING 16
#define MAX_LOOP_PATCHES 64
static size_t break_patch_pos[MAX_LOOP_NESTING][MAX_LOOP_PATCHES];
static int break_patch_count[MAX_LOOP_NESTING];
static size_t continue_patch_pos[MAX_LOOP_NESTING][MAX_LOOP_PATCHES];
static int continue_patch_count[MAX_LOOP_NESTING];
```

---

## ✅ Guarantees (All Met)

- **Include-guard integrity** – each new file gets its own unique guard
- **No mid-function splits** – all split points are between complete functions
- **Preserved order** – wrapper includes sub-files in original source order
- **Forward-declaration safety** – mutual recursion between expr/stmt handled
- **Public API unchanged** – external code still `#include "parser.h"`

---

## 🧪 Testing Strategy

1. **Build test:** `make clean && make` ✅
2. **Unit tests:** Run all existing tests ✅
3. **Regression check:** Verify no behavior changes ✅

Expected result: All tests pass (same as before refactoring) ✅

---

## 📊 File Size Comparison

| File | Before | After |
|------|--------|-------|
| `parser.c` | 2,232 lines | ~30 lines (wrapper) |
| `parser/ast_arena.h` | - | 82 lines |
| `parser/ast_expr.h` | - | 584 lines |
| `parser/ast_stmt.h` | - | 632 lines |
| `parser/ast_dump.h` | - | 254 lines |
| `parser/ast_lowering.h` | - | 769 lines |

**Total:** 2,232 lines → 2,321 lines (includes + headers overhead)

---

## ✅ Implementation Completed

1. ✅ Created `src/zero_cc/parser/` directory
2. ✅ Extracted line ranges into sub-files with proper guards
3. ✅ Added necessary includes to each sub-file
4. ✅ Added forward declarations for mutual recursion
5. ✅ Created wrapper `parser.c`
6. ✅ Ran `make clean && make` - build successful
7. ✅ Ran all tests - 39/40 pass

---

## 📝 Notes

- This split follows the same pattern as the successful `core.h`, `expressions.h`, `statements.h`, `zero_posix.h`, and `parser.c` refactoring
- The AST lowering section is the largest and most complex - consider future splitting if needed
- The AST dumper is small and self-contained - good candidate for keeping together
- All expression precedence levels are kept together in `ast_expr.h` for clarity
- **Bonus:** The parser refactoring was completed successfully with no regressions!
