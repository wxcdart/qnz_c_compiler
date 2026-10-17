# Include and Forward Declaration Best Practices

## Overview

This document establishes the conventions for header includes and forward declarations in the zero_cc compiler project. Following these guidelines reduces compilation dependencies, prevents circular includes, and improves build times.

## Quick Reference

| Situation | Use |
|-----------|-----|
| Need full type definition | `#include "full/header.h"` |
| Need only pointer/reference | Forward declaration + `#include "zero_cc_fwd.h"` |
| Creating new header | Check if forward decl suffices first |
| Header needs multiple types | Group related includes, avoid `types_lexer.h` if possible |
| Implementation file | Include the corresponding `.h` first |

---

## 1. Include Hierarchy

### 1.1 Header Categories

The codebase has three categories of headers:

#### **Leaf Headers** (Minimal dependencies)
These headers have few or no internal dependencies:

```
include/zero_cc_fwd.h          # Forward declarations only
src/zero_cc/lexer/state.h      # ZeroCCLexer struct
src/zero_cc/core/buffers.h     # ZeroCCBuffer struct
src/zero_cc/core/compiler_state.h  # Preprocessor state
```

#### **Aggregate Headers** (Multiple includes)
These headers combine multiple leaf headers:

```
src/zero_cc/types/token.h      # Token types
src/zero_cc/types/type_system.h # Type definitions
src/zero_cc/types/symbols.h    # Compiler struct
src/zero_cc/lexer.h            # Lexer + types
```

#### **Entry Point Headers** (For users)
These are what external code should include:

```
include/zero_cc.h              # Full API
include/zero_cc_fwd.h          # Forward decls only
```

### 1.2 Include Order Rules

When including multiple headers, follow this order:

1. **Standard library headers** (`<stdio.h>`, `<stddef.h>`, etc.)
2. **Forward declaration header** (`zero_cc_fwd.h`)
3. **Leaf headers** (buffers.h, state.h, etc.)
4. **Aggregate headers** (token.h, type_system.h, etc.)
5. **Entry point headers** (zero_cc.h - rarely needed internally)

Example:
```c
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>

#include "zero_cc_fwd.h"
#include "zero_cc/core/buffers.h"
#include "zero_cc/types/token.h"
```

---

## 2. Forward Declaration Guidelines

### 2.1 When to Use Forward Declarations

**DO use forward declarations when:**

1. You only need a pointer or reference to a type
2. You're writing a header file to minimize dependencies
3. Breaking a circular include dependency

```c
// GOOD: Only need pointer
#include "zero_cc_fwd.h"

void compile_code(ZeroCCCompiler *comp);  // Forward decl sufficient
```

**DON'T use forward declarations when:**

1. You need to access struct members
2. You need `sizeof(type)`
3. You're passing by value

```c
// BAD: Need full definition
#include "zero_cc_fwd.h"

size_t get_buffer_size(ZeroCCBuffer buf) {  // ERROR: incomplete type
    return buf.length;  // ERROR: can't access members
}
```

### 2.2 Forward Declaration Patterns

#### Pattern 1: Simple Function Pointer

```c
// In header (use forward decl)
#include "zero_cc_fwd.h"

void process_compiler(ZeroCCCompiler *comp);
```

#### Pattern 2: Callback with Opaque Handle

```c
// In header
#include "zero_cc_fwd.h"

typedef void (*ZeroCCCompileCallback)(ZeroCCCompiler *comp, void *user_data);

void register_callback(ZeroCCCompileCallback cb, void *user_data);
```

#### Pattern 3: Struct with Forward Declared Member

```c
// In header
#include "zero_cc_fwd.h"

typedef struct {
    ZeroCCBuffer *text_section;  // Pointer OK with forward decl
    int status;
} CompileContext;
```

### 2.3 Creating Forward Declarations

For new types, add forward declarations to `include/zero_cc_fwd.h`:

```c
// In your new header (my_feature.h)
typedef struct MyFeature MyFeature;  // Forward decl

// In zero_cc_fwd.h
typedef struct MyFeature MyFeature;
```

---

## 3. Circular Include Prevention

### 3.1 Common Circular Patterns

#### Pattern A: A includes B, B includes A

**Problem:**
```c
// a.h
#include "b.h"
typedef struct { B* b; } A;

// b.h
#include "a.h"
typedef struct { A* a; } B;
```

**Solution:**
```c
// a.h
typedef struct B B;  // Forward decl
typedef struct { B* b; } A;

// b.h
typedef struct A A;  // Forward decl
typedef struct { A* a; } B;
```

#### Pattern B: Header includes implementation header

**Problem:**
```c
// lexer.h
#include "lexer/scan.h"  // scan.h includes lexer.h -> CIRCULAR!
```

**Solution:**
```c
// lexer/state.h (extracted)
typedef struct { ... } ZeroCCLexer;

// lexer.h
#include "lexer/state.h"
#include "types_lexer.h"

// scan.h
#include "lexer/state.h"  // Only what it needs
```

### 3.2 Detecting Circular Includes

Use this command to check for circular dependencies:

```bash
# Find headers that include each other
for h in src/zero_cc/**/*.h; do
    includes=$(grep -h '#include.*"' "$h" | sed 's/.*"\([^"]*\)".*/\1/')
    for inc in $includes; do
        if grep -q "#include.*$(basename $h)" "src/zero_cc/$inc" 2>/dev/null; then
            echo "CIRCULAR: $h <-> src/zero_cc/$inc"
        fi
    done
done
```

---

## 4. types_lexer.h Dependencies

### 4.1 Required Include Order

`types_lexer.h` has strict dependency order (documented in header):

```c
// MUST be in this order:
#include "types/token.h"      // 1. Token types
#include "types/type_system.h" // 2. Type enums
#include "types/symbols.h"    // 3. ZeroCCCompiler
#include "types/type_funcs.h" // 4. Type helpers
#include "types/registry.h"   // 5. Symbol registry
#include "types/emit_helpers.h" // 6. Code emission
#include "types/symbol_mgmt.h"  // 7. Symbol management
```

**DO NOT** include these files individually in a different order.

### 4.2 When NOT to Include types_lexer.h

If you only need specific types, include only what you need:

```c
// BAD: Pulls in entire type system
#include "types_lexer.h"

// GOOD: Only need tokens
#include "types/token.h"

// GOOD: Only need compiler struct
#include "types/symbols.h"
```

---

## 5. New Header Guidelines

### 5.1 Checklist for New Headers

When creating a new header file:

- [ ] Does it need full type definitions or just pointers?
- [ ] Can it use forward declarations from `zero_cc_fwd.h`?
- [ ] Does it include standard library headers first?
- [ ] Is the include guard named correctly?
- [ ] Does it document its dependencies?
- [ ] Is it placed in the correct directory?

### 5.2 Template for New Headers

```c
/* ==========================================================================
 *  my_feature.h
 *  Brief description of what this header provides
 *  ==========================================================================
 *
 *  DEPENDENCIES:
 *  - zero_cc_fwd.h (for forward declarations)
 *  - <standard headers as needed>
 *
 *  USAGE:
 *  #include "zero_cc/my_feature.h"
 *  ==========================================================================*/

#ifndef ZERO_CC_MY_FEATURE_H
#define ZERO_CC_MY_FEATURE_H

#include <stddef.h>
#include <stdint.h>
#include "zero_cc_fwd.h"  // Forward declarations first

// Your declarations here

#endif /* ZERO_CC_MY_FEATURE_H */
```

### 5.3 Directory Placement

| Header Type | Location |
|-------------|----------|
| Public API | `include/` |
| Core types | `src/zero_cc/core/` |
| Type system | `src/zero_cc/types/` |
| Lexer | `src/zero_cc/lexer/` |
| Parser | `src/zero_cc/parser/` |
| Semantic | `src/zero_cc/semantic/` |
| JIT | `src/zero_cc/jit/` |

---

## 6. Common Pitfalls

### 6.1 Incomplete Type Errors

**Error:** `error: incomplete definition of type 'ZeroCCBuffer'`

**Cause:** Forward declared but need full definition:

```c
#include "zero_cc_fwd.h"

void foo(ZeroCCBuffer buf) {  // ERROR: can't pass by value
    buf.length = 0;  // ERROR: can't access members
}
```

**Fix:** Include the full header:

```c
#include "zero_cc/core/buffers.h"

void foo(ZeroCCBuffer buf) {  // OK: full definition
    buf.length = 0;  // OK: can access members
}
```

### 6.2 Implicit Declaration Warnings

**Warning:** `warning: implicit declaration of function 'snprintf'`

**Cause:** Missing standard library include:

```c
// Missing: #include <stdio.h>
snprintf(buf, sizeof(buf), "%d", x);  // Warning
```

**Fix:** Add the required standard header:

```c
#include <stdio.h>  // For snprintf
```

### 6.3 Multiple Definition Errors

**Error:** `error: multiple definition of 'my_function'`

**Cause:** Non-static function defined in header:

```c
// my.h
void my_function() {  // ERROR: defined in header
    // ...
}
```

**Fix:** Make it `static inline`:

```c
// my.h
static inline void my_function() {  // OK: internal linkage
    // ...
}
```

---

## 7. Performance Tips

### 7.1 Reducing Compilation Time

1. **Use forward declarations** in headers whenever possible
2. **Avoid `types_lexer.h`** in internal headers (it includes 7 files)
3. **Include `.h` first** in `.c` files to catch missing includes
4. **Group includes** by category for better cache utilization

### 7.2 Measuring Impact

Check include depth with:

```bash
# Count includes in a file
grep -c '#include' my_file.c

# Find files with most includes
find src/zero_cc -name "*.h" -exec sh -c \
  'echo "$(grep -c "#include" "$1") $1"' _ {} \; | sort -rn
```

---

## 8. Examples

### 8.1 Good Header Example

```c
/* ==========================================================================
 *  zero_cc/semantic/context.h
 *  Semantic analysis context
 *  ==========================================================================*/

#ifndef ZERO_CC_SEMANTIC_CONTEXT_H
#define ZERO_CC_SEMANTIC_CONTEXT_H

#include <stddef.h>
#include <stdbool.h>
#include "zero_cc_fwd.h"  // Forward decls first

// Only needs pointer to ZeroCCCompiler
typedef struct {
    ZeroCCCompiler *compiler;
    bool in_function;
    size_t scope_depth;
} SemanticContext;

static inline void semantic_init(SemanticContext *ctx, ZeroCCCompiler *comp) {
    ctx->compiler = comp;
    ctx->in_function = false;
    ctx->scope_depth = 0;
}

#endif /* ZERO_CC_SEMANTIC_CONTEXT_H */
```

### 8.2 Bad Header Example

```c
/* BAD: Too many dependencies */
#ifndef BAD_HEADER_H
#define BAD_HEADER_H

#include "zero_cc/types_lexer.h"  // Pulls in 7 files!
#include "zero_cc/lexer.h"        // More dependencies
#include "zero_cc/parser.h"
#include "zero_cc/statements.h"
#include "zero_cc/expressions.h"

// Should use forward declarations instead

#endif
```

---

## 9. Migration Guide

### 9.1 Converting Existing Headers

**Step 1:** Identify what types you actually need

```c
// Before
#include "types_lexer.h"

void process(ZeroCCCompiler *comp);  // Only needs pointer!
```

**Step 2:** Replace with forward declarations

```c
// After
#include "zero_cc_fwd.h"

void process(ZeroCCCompiler *comp);  // Still works, fewer deps
```

**Step 3:** Test compilation

```bash
make clean && make
```

### 9.2 Breaking Changes

If you must change an include order:

1. Update documentation in `types_lexer.h`
2. Add migration notes in commit message
3. Test all affected files

---

## 10. Maintenance

### 10.1 Adding New Types

1. Add forward declaration to `include/zero_cc_fwd.h`
2. Update this document if it affects include hierarchy
3. Add example usage if non-obvious

### 10.2 Review Checklist

When reviewing pull requests with header changes:

- [ ] Are forward declarations used where possible?
- [ ] Is `zero_cc_fwd.h` included before other headers?
- [ ] Are standard library headers included first?
- [ ] Is include order documented for complex headers?
- [ ] Are there any new circular dependencies?

---

## Appendix: Complete Forward Declaration List

All forward declarations in `include/zero_cc_fwd.h`:

| Type | Source Header |
|------|---------------|
| `ZeroCCBuffer` | `core/buffers.h` |
| `ZeroCCPreprocessor` | `core/compiler_state.h` |
| `ZeroCCMacro` | `core/compiler_state.h` |
| `ZeroCCToken` | `types/token.h` |
| `ZeroCCTokType` | `types/token.h` |
| `ZeroCCType` | `types/type_system.h` |
| `ZeroCCSym` | `types/type_system.h` |
| `ZeroCCCompiler` | `types/symbols.h` |
| `ZeroCCTLSBackend` | `zero_cc_backends.h` |
| `ZeroCCEmitBackend` | `zero_cc_backends.h` |
| `ZeroCCLinkBackend` | `zero_cc_backends.h` |
| `ZeroCCMemoryBackend` | `zero_cc_backends.h` |
| `ZeroCCABICallBackend` | `zero_cc_backends.h` |
| `ZeroCCAtomicBackend` | `zero_cc_backends.h` |
| `ZeroCCGenericBackend` | `zero_cc_backends.h` |
| `ZeroCCStdlibBackend` | `zero_cc_backends.h` |
| `ZeroCCDiagBackend` | `zero_cc_backends.h` |
| `ZeroCCLexer` | `lexer/state.h` |
| `ZeroCCASTNode` | `parser.h` |
| `ZeroCCASTArena` | `parser.h` |
| `ZeroPtrStack` | `core/buffers.h` |
