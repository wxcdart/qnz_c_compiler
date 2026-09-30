# C99 Atomic Operations with GCC Builtins

## Overview

This project uses **GCC's `__atomic_*` builtins** for atomic operations, which work in **C99 mode** (GNU99). No C11 `stdatomic.h` required.

## Why This Works

GCC's `__atomic_*` builtins are **compiler intrinsics**, not C language features. They work in any C standard:
- C99 ✓
- C11 ✓
- C17 ✓

## How GCC Handles `__atomic_*`

On **x86-64** (your target):
- GCC emits **lock-prefixed instructions** directly (`LOCK XADD`, `LOCK CMPXCHG`, etc.)
- **No libatomic library needed** - everything is inlined by GCC

On **platforms without native atomics** (some embedded ARM):
- GCC calls **libatomic functions** (`__atomic_load_8`, `__atomic_store_8`, etc.)
- Links against `libatomic.so`

## Your Current Setup

### Minimal Atomic Backend (`src/zero_cc/backends/minimal.c`)

```c
static void *atomic_resolve_fwd(ZeroCCCompiler *c, const char *name) {
    (void)c; return zero_cc_resolve_symbol(name);
}
static const ZeroCCAtomicBackend minimal_atomic = {
    .emit_load = NULL,      // Not implemented
    .emit_store = NULL,     // Not implemented
    .emit_rmw = NULL,       // Not implemented
    .resolve_libatomic = atomic_resolve_fwd,  // Just resolves symbols
};
```

**What this means**: Your compiler doesn't emit atomic instructions itself. It relies on GCC's `__atomic_*` builtins being lowered to lock-prefixed instructions.

### Usage in Code

```c
// In your C source files - works in C99 mode!
int counter = 0;
__atomic_add_fetch(&counter, 1, __ATOMIC_SEQ_CST);  // Returns new value
__atomic_load_n(&counter, __ATOMIC_SEQ_CST);        // Returns value
__atomic_store_n(&counter, 0, __ATOMIC_SEQ_CST);    // Stores value
```

## C11 `stdatomic.h` Comparison

```c
// C11 approach (requires -std=c11)
#include <stdatomic.h>
atomic_int counter;
atomic_fetch_add(&counter, 1);
atomic_load(&counter);
atomic_store(&counter, 0);

// GCC __atomic_* (works in C99+)
int counter;
__atomic_add_fetch(&counter, 1, __ATOMIC_SEQ_CST);
__atomic_load_n(&counter, __ATOMIC_SEQ_CST);
__atomic_store_n(&counter, 0, __ATOMIC_SEQ_CST);
```

**Both generate identical machine code on x86-64**. The C11 version is just more portable across compilers.

## Memory Order Constants

GCC uses the same constants as C11:
- `__ATOMIC_RELAXED` - no ordering guarantees
- `__ATOMIC_ACQUIRE` - acquire semantics
- `__ATOMIC_RELEASE` - release semantics
- `__ATOMIC_ACQ_REL` - acquire + release
- `__ATOMIC_SEQ_CST` - sequential consistency (default)

## Future Work

If you want **native atomic instruction emission** in your compiler:

1. Implement `emit_load`, `emit_store`, `emit_rmw` in `minimal.c`
2. For x86-64:
   - `emit_load` → `LOCK XCHG` or `MOV` with memory barrier
   - `emit_store` → `LOCK XCHG` or `MOV` with memory barrier
   - `emit_rmw` → `LOCK XADD`, `LOCK CMPXCHG`, etc.
3. Remove dependency on libatomic

But for now, **GCC's builtins work perfectly** and don't require C11.

## Summary

- ✓ Using `__atomic_*` builtins in C99 mode
- ✓ No C11 required
- ✓ No libatomic needed on x86-64 (GCC emits lock-prefixed instructions)
- ✓ Minimal atomic backend just resolves symbols (not used on x86-64)
- ✓ Can upgrade to C11 `stdatomic.h` later if desired
