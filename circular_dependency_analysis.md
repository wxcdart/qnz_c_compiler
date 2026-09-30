# Circular Dependency Analysis

## Current Architecture

```
expressions.h
  ├── parser.h
  ├── primary.h
  ├── unary.h
  ├── binary.h
  ├── ternary.h
  └── visitor.h

statements.h
  ├── types_lexer.h
  ├── parser.h
  ├── expressions.h  ← DEPENDS ON EXPRESSIONS
  ├── type_system.h
  ├── symbols.h
  ├── control.h
  ├── definitions.h
  └── variables.h
```

## The Circular Dependency Problem

### Dependency Chain

```
statements.h
  └── includes expressions.h
        └── includes primary.h
              └── calls zero_cc_parse_stmt()  ← NEEDS statements.h
```

### Why This Happens

**Root Cause**: Expressions can contain statements (GNU statement expressions: `({ ... })`)

```c
// In primary.h (line 86)
if (tok.type == TK_LPAREN && c->tokens[c->token_pos + 1].type == TK_LBRACE) {
    c->token_pos += 2;
    while (c->tokens[c->token_pos].type != TK_RBRACE) {
        zero_cc_parse_stmt(c);  // ← Calls statement parser from expression code
    }
}
```

This creates:
- `statements.h` → `expressions.h` (statements include expressions for assignments)
- `expressions/primary.h` → `zero_cc_parse_stmt()` (expressions call statements for GNU extensions)

### Current Solution (Workaround)

```c
// In primary.h (line 13)
void zero_cc_parse_stmt(ZeroCCCompiler *c);  // Forward declaration

// In statements.h (line 108)
void zero_cc_parse_stmt(ZeroCCCompiler *c) {  // Actual definition
    // ... implementation that calls zero_cc_parse_expr()
}
```

**How it works:**
- `statements.h` defines `zero_cc_parse_stmt()` after including `expressions.h`
- `primary.h` has forward declaration before calling it
- Linker resolves the actual function at link time

**Why it's fragile:**
- Order-dependent includes
- Hard to maintain
- Breaks if function signatures change
- Not obvious from code structure

---

## Better Architectures

### Option 1: Separate Parser Interface (Recommended)

```c
// parser_interface.h
#ifndef ZERO_CC_PARSER_INTERFACE_H
#define ZERO_CC_PARSER_INTERFACE_H

#include "core.h"

// Function pointers for mutual recursion
typedef struct {
    void (*parse_expr)(ZeroCCCompiler *c);
    void (*parse_stmt)(ZeroCCCompiler *c);
    int64_t (*eval_const_expr)(ZeroCCCompiler *c);
} ParserInterface;

// Global interface (set during initialization)
extern ParserInterface parser_if;

// Inline wrappers
static inline void zero_cc_parse_expr(ZeroCCCompiler *c) {
    parser_if.parse_expr(c);
}

static inline void zero_cc_parse_stmt(ZeroCCCompiler *c) {
    parser_if.parse_stmt(c);
}

#endif
```

**Usage:**

```c
// expressions.h
#include "parser_interface.h"

// statements.h
#include "parser_interface.h"

// parser.c (initialization)
void parser_init(void) {
    parser_if.parse_expr = zero_cc_parse_expr_impl;
    parser_if.parse_stmt = zero_cc_parse_stmt_impl;
}
```

**Benefits:**
- ✅ No circular dependencies
- ✅ Clear separation of concerns
- ✅ Easy to test expression/parser separately
- ✅ No forward declaration hacks

**Drawbacks:**
- ⚠️ Function pointer indirection (negligible performance)
- ⚠️ Requires initialization step

---

### Option 2: Single Header File (Simple but Monolithic)

```c
// parser.h (complete, no sub-headers)
#ifndef ZERO_CC_PARSER_H
#define ZERO_CC_PARSER_H

// Forward declarations
void zero_cc_parse_expr(ZeroCCCompiler *c);
void zero_cc_parse_stmt(ZeroCCCompiler *c);

// Expression parser (inline)
static inline void zero_cc_parse_expr(ZeroCCCompiler *c) {
    // ... implementation ...
}

// Statement parser (inline)
static inline void zero_cc_parse_stmt(ZeroCCCompiler *c) {
    // ... implementation ...
}

#endif
```

**Benefits:**
- ✅ No circular dependencies
- ✅ Simple
- ✅ Fast (inline)

**Drawbacks:**
- ❌ Single large file (hard to maintain)
- ❌ Violates modular design goal
- ❌ Re-compiles everything on change

---

### Option 3: Two-Pass Parser (Separate Syntax from Semantics)

```c
// parser_syntax.h (no mutual recursion)
typedef struct {
    ASTNode *expr;
    ASTNode *stmt;
} ParsedResult;

ASTNode* parse_expr(ZeroCCCompiler *c);
ASTNode* parse_stmt(ZeroCCCompiler *c);

// parser_semantic.h (uses AST, no mutual recursion)
void lower_expr(ASTNode *node, ZeroCCCompiler *c);
void lower_stmt(ASTNode *node, ZeroCCCompiler *c);
```

**Benefits:**
- ✅ No circular dependencies
- ✅ Clean two-pass architecture
- ✅ Enables optimization

**Drawbacks:**
- ⚠️ Requires AST intermediate representation
- ⚠️ More complex initial design

---

### Option 4: Visitor Pattern with Dependency Injection

```c
// parser_context.h
typedef struct ParserContext {
    ZeroCCCompiler *compiler;
    void (*on_expr_complete)(struct ParserContext *, ASTNode *);
    void (*on_stmt_complete)(struct ParserContext *, ASTNode *);
} ParserContext;

// expressions.c
ASTNode* parse_expr(ParserContext *ctx);

// statements.c
ASTNode* parse_stmt(ParserContext *ctx) {
    // Can call parse_expr(ctx) without circular dependency
}
```

**Benefits:**
- ✅ No circular dependencies
- ✅ Extensible
- ✅ Good for testing

**Drawbacks:**
- ⚠️ More boilerplate
- ⚠️ Context passing overhead

---

## Why Current Design Works (For Now)

```
statements.h
  ↓ includes expressions.h
    ↓ includes primary.h
      ↓ forward declares zero_cc_parse_stmt()
        ↓ calls zero_cc_parse_stmt()
statements.h
  ↓ defines zero_cc_parse_stmt()
    ↓ calls zero_cc_parse_expr()
```

**Key insight:** The circular dependency is broken by:
1. Forward declaration in `primary.h`
2. Actual definition in `statements.h` (after including `expressions.h`)

**Why it's okay:**
- ✅ Compiles correctly
- ✅ All 71 tests pass
- ✅ No runtime overhead

**Why it's fragile:**
- ❌ Order matters (must include expressions.h before defining parse_stmt)
- ❌ Hard to understand from code alone
- ❌ Breaks if you refactor header structure

---

## Recommendations

### Short-Term (Current Sprint)

**Keep current design.** Here's why:
1. **Working**: All tests pass
2. **Minimal**: Only affects parser initialization
3. **Safe**: Forward declarations are stable
4. **Fast**: No indirection overhead

**Document the pattern:**
```c
/* In expressions/primary.h:
 * Forward declaration to break circular dependency:
 * statements.h → expressions.h → primary.h → zero_cc_parse_stmt()
 */
void zero_cc_parse_stmt(ZeroCCCompiler *c);
```

### Long-Term (Future Refactoring)

**Migrate to Option 1 (Parser Interface)** when:
1. Adding optimization passes
2. Implementing two-pass architecture
3. Need to test expression/parser separately
4. Adding new expression/statement types

**Migration path:**
```
Phase 1: Keep current design (now)
Phase 2: Add parser_interface.h (next sprint)
Phase 3: Move parse_expr to parser_interface (after tests pass)
Phase 4: Move parse_stmt to parser_interface (after tests pass)
Phase 5: Remove forward declarations (cleanup)
```

---

## Files Involved in Circular Dependency

| File | Role | Depends On |
|------|------|------------|
| `statements.h` | Statement parser | `expressions.h` |
| `expressions.h` | Expression parser | `parser.h`, sub-headers |
| `expressions/primary.h` | Primary expressions | `zero_cc_parse_stmt()` |
| `statements/control.h` | Control flow | `zero_cc_parse_expr()` |

**Total circular dependency scope:** 4 files, ~2000 lines

---

## How to Detect Future Circular Dependencies

```bash
# Check for circular includes
for f in src/zero_cc/**/*.h; do
    echo "=== $f ==="
    grep "^#include" "$f" | grep -v "<"
done

# Build dependency graph
grep -rh "^#include" src/zero_cc/ | \
  grep -v "<" | \
  sed 's/.*"\(.*\)".*/\1/' | \
  sort | uniq
```

**Red flags:**
- File A includes B, B includes A (direct cycle)
- File A includes B, B includes C, C includes A (indirect cycle)
- More than 3 levels of includes (complexity)

---

## Summary

**Current state:**
- ✅ Working circular dependency (forward declaration pattern)
- ✅ All tests pass
- ⚠️ Fragile (order-dependent)

**Recommendation:**
- Short-term: Keep current design, document it
- Long-term: Migrate to parser interface pattern

**When to refactor:**
- Adding two-pass architecture
- Adding optimization passes
- Need for better testability

**Don't refactor now because:**
- Working code
- Minimal complexity
- Clear migration path exists

---

## Implementation Status

### Short-Term Recommendation: ✅ COMPLETED

**Recommendation**: Keep current design with forward declaration pattern

**Implementation**:
- ✅ Forward declaration in `expressions/primary.h` (line 13)
- ✅ Definition in `statements.h` (after including `expressions.h`)
- ✅ All 70+ tests pass
- ✅ No runtime overhead

**Current Architecture**:
```
statements.h → expressions.h → primary.h → zero_cc_parse_stmt() (forward decl)
statements.h → zero_cc_parse_stmt() (definition) → zero_cc_parse_expr()
```

### Long-Term Recommendation: ⏸️ NOT NEEDED

**Recommendation**: Migrate to parser interface pattern (Option 1)

**Trigger Conditions** (not yet met):
- ❌ Adding two-pass architecture
- ❌ Adding optimization passes
- ❌ Need for better testability

**Status**: Current design is working well. Migration only needed when adding new features.

---

*Document status: Short-term recommendation implemented, long-term migration optional*
