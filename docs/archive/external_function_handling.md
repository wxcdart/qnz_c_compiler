# External Function Handling in JIT Compilation: Lessons Learned

## Overview

This document details the investigation and fix for a segmentation fault in the `rodata_demo_jit` test, which revealed fundamental issues in how external function calls (like `printf`, `fopen`) are handled in a JIT compiler. The fix involved changes across 10 files and exposed important lessons about JIT compilation architecture.

---

## Problem Statement

The `rodata_demo_jit` test compiles and executes this C code at runtime:
```c
#include <stdio.h>
int main(void) { printf("hello world\n"); return 0; }
```

**Initial Symptom**: Segmentation fault during compilation in `zero_cc_type_to_typeinfo()` → `type_function()` → `memcpy()`

**After Fix 1**: Compilation succeeds but JIT execution crashes when calling `printf`

---

## Root Cause Analysis

### Issue 1: Compilation Crash - NULL Pointer in `type_function()`

**Location**: `src/zero_cc/semantic/type.c` → `type_function()`

```c
TypeInfo *type_function(Arena *perm, TypeInfo *ret, TypeInfo **params, 
                        size_t param_cnt, bool is_variadic) {
    // ...
    if (param_cnt > 0 && params) {  // BUG: Missing NULL check!
        TypeInfo **p_copy = arena_alloc(perm, param_cnt * sizeof(TypeInfo *));
        if (p_copy) {
            memcpy(p_copy, params, param_cnt * sizeof(TypeInfo *));  // CRASH: params is NULL
        }
    }
}
```

**Why it happened**: When parsing external function signatures from `stdio.h` (included via `#include <stdio.h>` in the JIT source), the compiler creates `ZeroCCFunction` entries for `printf`, `fopen`, etc. but their parameter type arrays (`param_types`) were not properly initialized. The semantic analysis phase called `type_function()` with `param_cnt > 0` but `params = NULL`.

**Fix**: Added explicit NULL check:
```c
if (param_cnt > 0 && params != NULL) {  // Now safe
```

---

### Issue 2: JIT Execution Crash - Incorrect External Function Call Handling

**The Core Problem**: External functions (from libc) were being treated identically to internal functions defined in the JIT source code.

#### How Function Registration Works

When the parser encounters a function declaration, it creates a `ZeroCCFunction` entry:

```c
// For prototypes (extern declarations or forward declarations)
if (next_token == ';') {
    fn->text_offset = 0;      // No code generated
    fn->is_prototype = true;  // NEW: Marks as external prototype
}

// For definitions (function bodies)
if (next_token == '{') {
    fn->text_offset = text_section.length;  // Code starts here
    fn->is_prototype = false;               // NEW: Internal function
}
```

**Before the fix**: No `is_prototype` field existed. Both external prototypes and internal functions had `text_offset = 0` initially (since internal functions hadn't emitted code yet). This made them indistinguishable.

#### Code Generation Paths

**Internal Function Call** (correct):
```assembly
call rel32          ; Relative call to function in same text section
; Patched at runtime with: target_offset - (call_site + 4)
```

**External Function Call** (incorrect before fix):
```assembly
call rel32          ; WRONG: Relative call with 0 offset or garbage
; Should be:
mov r11, <address>  ; Load absolute address from dlsym()
call r11            ; Indirect call through register
```

#### The Crash Mechanism

1. Parser sees `printf("hello")` in JIT source
2. Looks up `printf` → finds it in function table (registered from `stdio.h` parsing)
3. Since `printf.text_offset == 0` (prototype), code generator creates:
   ```assembly
   call 0x00000000   ; Relocation with 0 offset
   ```
4. At JIT execution time, relocation patching computes:
   ```c
   rel = target_fn->text_offset - (patch_pos + 4);
   // rel = 0 - (patch_pos + 4) = negative offset into code section
   ```
5. Generated code jumps to wrong address → segfault

---

## The Fix: Comprehensive External Function Handling

### 1. Type System Extension (`src/zero_cc/types/type_system.h`)

```c
typedef struct {
    char name[128];
    size_t text_offset;
    ZeroCCType return_type;
    size_t param_count;
    // ...
    bool is_prototype;        // NEW: Distinguishes extern from internal
} ZeroCCFunction;
```

### 2. Registration Logic (`src/zero_cc/jit/compile.h`)

```c
// Prototype declaration (extern or forward)
if (token == ';') {
    fn->text_offset = 0;
    fn->is_prototype = true;      // External prototype
}

// Function definition
if (token == '{') {
    fn->text_offset = c->text_section.length;
    fn->is_prototype = false;     // Internal function with body
}
```

### 3. Code Generation - Three Locations Fixed

#### A. Parser AST Lowering (`src/zero_cc/parser.c` - NODE_CALL)
```c
ZeroCCFunction *fn = zero_cc_find_function(c, name);
void *host_fn = zero_cc_resolve_symbol(name);  // dlsym()

if (fn && !fn->is_prototype) {
    // Internal: call rel32 with relocation
    emit_call_rel32(fn->text_offset);
} else if (host_fn) {
    // External resolved at compile time: mov r11, addr; call r11
    emit_mov_r11_imm64(host_fn);
    emit_call_r11();
} else if (fn && fn->is_prototype) {
    // External prototype: generate indirect call with placeholder
    emit_mov_r11_imm64(0);  // Placeholder
    emit_call_r11();
    record_relocation(..., is_call=false);  // Address patch, not call patch
}
```

#### B. Expressions (`src/zero_cc/expressions.h`)
Same pattern for function calls in expressions.

#### C. Primary Expressions (`src/zero_cc/expressions/primary.h`)
Same pattern for primary expression function calls.

### 4. Runtime Patching (`src/zero_cc/jit/execution.h`)

```c
for (each relocation) {
    if (reloc.is_call) {
        // Internal function: patch relative offset
        target_fn = find_function(reloc.target_symbol);
        if (target_fn && !target_fn->is_prototype) {
            rel = target_fn->text_offset - (patch_pos + 4);
            memcpy(code + patch_pos, &rel, 4);
        }
    } else {
        // External function: patch absolute address
        void *host_fn = dlsym(RTLD_DEFAULT, reloc.target_symbol);
        if (host_fn) {
            memcpy(code + patch_pos, &host_fn, 8);  // 64-bit address
        }
    }
}
```

### 5. ELF Emission (`src/zero_cc/jit/elf_emitter.h`)

```c
// Skip prototype functions when emitting ELF
if (reloc.is_call) {
    target_fn = find_function(reloc.target_symbol);
    if (target_fn && !target_fn->is_prototype) {  // Skip prototypes
        patch_relative_call();
    }
}
```

---

## Key Lessons Learned

### 1. **Distinguish External vs Internal at Registration Time**

The critical insight: **the moment you parse a function declaration, you must know if it's external or internal**. This cannot be deferred to code generation time because:
- `text_offset == 0` is ambiguous (could be prototype OR first internal function)
- External functions need fundamentally different code generation
- The decision affects relocation type and runtime patching strategy

### 2. **Two-Phase Relocation System**

JIT compilers need **two distinct relocation types**:

| Relocation Type | Purpose | Runtime Action |
|----------------|---------|----------------|
| `is_call=true` | Internal function calls | Patch relative offset |
| `is_call=false` | External function addresses | Patch absolute address via `dlsym()` |

This is cleaner than trying to patch call instructions for external functions (which requires instruction rewriting).

### 3. **Code Generation Must Match Runtime Patching**

The code generator and runtime patcher must agree on:
- What instruction sequence is emitted
- Where the patchable bytes are located
- What value gets written at runtime

Example: For external calls, generator emits `mov r11, 0; call r11` (13 bytes), patcher writes 8-byte address at offset 2. Both must agree on this layout.

### 4. **dlsym() Resolution Timing Matters**

```c
// At code generation time: host_fn = dlsym() may fail if library not loaded
// At runtime patching time: Library IS loaded (we're running in it)
```

**Lesson**: Defer `dlsym()` to runtime patching phase, not code generation. This also handles ASLR correctly.

### 5. **Prototype Registration from System Headers**

When parsing `#include <stdio.h>`, the compiler parses external declarations. These must be marked as prototypes immediately:

```c
// In parser when seeing: int printf(const char*, ...);
fn->is_prototype = true;  // Even if no ';' follows (it's a declaration)
```

System headers don't have function bodies - they're all prototypes.

---

## Architecture Diagram

```
┌─────────────────────────────────────────────────────────────────┐
│                        PARSING PHASE                            │
├─────────────────────────────────────────────────────────────────┤
│  #include <stdio.h>                                            │
│    → Parse "int printf(const char*, ...);"                     │
│    → Create ZeroCCFunction { name="printf", is_prototype=true }│
│                                                                 │
│  int main() { printf("hello"); }                               │
│    → Parse function definition                                 │
│    → Create ZeroCCFunction { name="main", is_prototype=false } │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                      CODE GENERATION                            │
├─────────────────────────────────────────────────────────────────┤
│  printf("hello")                                               │
│    → Find "printf" → is_prototype=true                         │
│    → Generate:                                                 │
│      mov r11, 0x0000000000000000    ; placeholder              │
│      call r11                     ; indirect call              │
│    → Record relocation: {symbol="printf", is_call=false,       │
│                           patch_offset=<addr of 0x00...>}      │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                     JIT EXECUTION                               │
├─────────────────────────────────────────────────────────────────┤
│  For each relocation:                                          │
│    if (is_call) {                                              │
│        // Internal: patch relative offset                      │
│        target = find_function(name);                           │
│        rel = target.offset - (patch_pos + 4);                  │
│        *(int32*)(code + patch_pos) = rel;                      │
│    } else {                                                    │
│        // External: patch absolute address                     │
│        addr = dlsym(RTLD_DEFAULT, name);                       │
│        *(uint64*)(code + patch_pos) = addr;                    │
│    }                                                           │
│                                                                 │
│  mmap(RX) → copy code → call entry_point()                     │
└─────────────────────────────────────────────────────────────────┘
```

---

## Common Pitfalls to Avoid

| Pitfall | Symptom | Prevention |
|---------|---------|------------|
| Missing `is_prototype` | Internal & external indistinguishable | Add explicit flag at registration |
| `dlsym()` at codegen time | Fails if lib not loaded; ASLR issues | Defer to runtime patching |
| Single relocation type | Can't distinguish call vs address patch | Use `is_call` boolean |
| `text_offset == 0` check | Ambiguous for first internal function | Use explicit `is_prototype` flag |
| Patching call instruction for extern | Requires instruction rewriting | Use indirect call + address patch |

---

## Testing Strategy

### Unit Tests for External Calls

```c
// Test 1: Simple external call
"int main() { return printf(\"hello\"); }"

// Test 2: Multiple external calls
"int main() { printf(\"a\"); fprintf(stderr, \"b\"); return 0; }"

// Test 3: Mixed internal/external
"int foo() { return 42; } int main() { printf(\"%d\", foo()); }"

// Test 4: External function pointers
"int main() { int (*f)(const char*) = printf; return f(\"test\"); }"
```

### Verification Checklist

- [ ] Compilation succeeds with `#include <stdio.h>`
- [ ] `printf`, `fopen`, `malloc` etc. resolve correctly
- [ ] Internal function calls still work
- [ ] Mixed internal/external calls work
- [ ] ELF emission produces valid executable
- [ ] ASLR compatibility (run multiple times)
- [ ] Function pointer calls to external functions

---

## Related Files

| File | Purpose |
|------|---------|
| `src/zero_cc/types/type_system.h` | `ZeroCCFunction` struct with `is_prototype` |
| `src/zero_cc/jit/compile.h` | Function registration logic |
| `src/zero_cc/parser.c` | AST lowering for NODE_CALL |
| `src/zero_cc/expressions.h` | Expression function calls |
| `src/zero_cc/expressions/primary.h` | Primary expression function calls |
| `src/zero_cc/jit/execution.h` | JIT runtime relocation patching |
| `src/zero_cc/jit/elf_emitter.h` | ELF emission with prototype skipping |
| `src/zero_cc/semantic/type.c` | `type_function()` NULL safety |
| `src/zero_cc/jit/type_info.h` | Type validation for invalid types |

---

## Conclusion

The `rodata_demo_jit` crash revealed that **external function handling is not a minor edge case—it's a fundamental architectural concern in JIT compilation**. The fix required:

1. **Explicit classification** of functions at parse time
2. **Dual relocation system** for calls vs address patches
3. **Consistent code generation** across all call sites
4. **Deferred symbol resolution** to runtime patching phase

These patterns apply to any JIT compiler that must interact with host libraries. The key principle: **the moment you see a function name, you must know if it's "yours" or "theirs"**, and generate code accordingly.

---

*Document created as part of fixing commit `ebfd63a` - "fix: Fix rodata_demo_jit crash - external function handling in JIT"*