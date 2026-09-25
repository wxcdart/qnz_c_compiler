# TLS Backend Plug‑in Guide for `zero_cc`

## Table of Contents
1. [Why a TLS Backend Interface?](#why-a-tls-backend-interface)  
2. [The Backend Contract (`ZeroCCTLSBackend`)](#the-backend-contract-zerocctlsbackend)  
3. [Integrating a Backend into `zero_cc`](#integrating-a-backend-into-zero_cc)  
4. [Default Static‑Alias Backend (already in the code)](#default-static‑alias-backend-already-in-the-code)  
5. [Writing a Platform‑Specific Backend
   * POSIX (`pthread`)  
   * Windows (`TlsAlloc`)  
   * Bare‑metal / Embedded](#writing-a-platform‑specific-backend)  
6. [Choosing a Backend at Compile‑time](#choosing-a-backend-at-compile‑time)  
7. [Choosing a Backend at Run‑time (API)](#choosing-a-backend-at-run‑time-api)  
8. [Testing Your Backend](#testing-your-backend)  
9. [Best Practices & Gotchas](#best-practices--gotchas)  
10. [Full Example: A Simple Logger Backend](#full-example-a-simple-logger-backend)  
11. [License](#license)

---

## 1. Why a TLS Backend Interface?
`_Thread_local` is a C11 storage‑class specifier that promises **per‑thread** objects.  The original implementation in `zero_cc` treats it as a *static alias* – a single global instance stored inside the compiler’s own data structures.  This works for the current test suite but:

* It does not support **writes** or the **address‑of** operator.
* It cannot be shared across multiple source files.
* It provides no real per‑thread isolation on platforms that offer TLS.

By abstracting the handling of `_Thread_local` behind a tiny plug‑in interface we achieve:

* **Portability** – a single front‑end can target Linux, macOS, Windows, or bare‑metal systems.
* **Extensibility** – a project can supply an optimized TLS implementation without touching the parser.
* **Testability** – the default backend can stay simple while a mock or real backend is exercised in integration tests.

---

## 2. The Backend Contract (`ZeroCCTLSBackend`)
All backends must provide the same set of callbacks.  The contract lives in **`zero_cc_tls.h`** (a public header you should ship with the library).

```c
/* zero_cc_tls.h – public TLS backend interface */
#ifndef ZERO_CC_TLS_H
#define ZERO_CC_TLS_H

#include "types_lexer.h"   // for ZeroCCCompiler, ZeroCCType

typedef struct ZeroCCTLSBackend {
    /* Declare a new thread‑local variable.
       *c* – current compiler instance.
       name – identifier (null‑terminated).  The backend may copy it.
       type – the parsed ZeroCCType (size/alignment information).
       init_value – constant initializer if present, otherwise 0.
       Returns an opaque *handle* that will be passed back to the other
       callbacks. The handle must stay valid for the whole compilation
       session (usually a pointer to an internal struct).
    */
    void *(*declare)(ZeroCCCompiler *c,
                     const char *name,
                     const ZeroCCType *type,
                     int64_t init_value);

    /* Emit code that loads the current value of the TLS variable into
       RAX (the JIT compiler's result register).  The backend decides
       whether to generate an immediate, a memory load, or a call to a
       runtime helper.
    */
    void (*emit_load)(ZeroCCCompiler *c, const void *handle);

    /* Emit code that stores the value currently in RAX into the TLS
       variable.  Used for assignments like `tls_var = 5;`.
    */
    void (*emit_store)(ZeroCCCompiler *c, const void *handle);

    /* Emit code that produces the address of the TLS variable (the
       `&tls_var` expression).  The generated address must be usable by
       subsequent loads/stores and may be a real memory address or a
       symbolic placeholder.
    */
    void (*emit_address)(ZeroCCCompiler *c, const void *handle);

    /* Optional cleanup hook, called once after code generation is
       finished.  Useful for freeing resources (e.g., TLS keys).
    */
    void (*finalize)(ZeroCCCompiler *c);
} ZeroCCTLSBackend;

/* Helper to set a backend on a compiler instance.  Passing NULL
   resets to the built‑in default static‑alias backend. */
static inline void zero_cc_set_tls_backend(ZeroCCCompiler *c,
                                           const ZeroCCTLSBackend *b)
{
    extern const ZeroCCTLSBackend zero_cc_default_tls_backend; // defined in the library
    c->tls_backend = b ? b : &zero_cc_default_tls_backend;
}

#endif /* ZERO_CC_TLS_H */
```

**Key points**
* The backend receives a pointer to the `ZeroCCCompiler` so it can emit machine code (using the same helper functions you already use).
* All callbacks are *purely code‑generation* – they must not perform I/O or allocate heavy resources at JIT run‑time.
* The `handle` is opaque to the compiler; you can store anything you need (e.g., a TLS key, an offset in `.data`, a pointer to a runtime stub).

---

## 3. Integrating a Backend into `zero_cc`
The core compiler already contains a stub for the default backend.  To plug in a custom implementation you only need to:

1. **Provide a `ZeroCCTLSBackend` object** (usually `static const`).
2. **Call `zero_cc_set_tls_backend`** on the `ZeroCCCompiler` *before* you start compilation (e.g., right after `zero_cc_compile_c99` creates the compiler instance, or from user code).
3. **Link the backend object** with your program (static library, shared library, or directly compiled into the same binary).

```c
ZeroCCCompiler comp = {0};
zero_cc_tokenize_c99(source, &comp);
/* Choose a backend – could be the default, a POSIX one, etc. */
zero_cc_set_tls_backend(&comp, &my_posix_tls_backend);
zero_cc_compile_c99(&comp);
zero_cc_execute_jit(&comp, NULL);
```

That is all – the parser will automatically call the backend’s `declare`, `emit_load`, `emit_store`, and `emit_address` functions whenever it encounters a `_Thread_local` declaration or use.

---

## 4. Default Static‑Alias Backend (already in the code)
The repository ships a **static‑alias** backend that stores each `_Thread_local` variable in the compiler’s own `thread_locals[]` array and emits a simple immediate move (`mov rax, imm64`) for reads.  It **does not** support:
* mutable writes (`=`/`+=`/`-=`)
* taking the address (`&var`)
* cross‑translation‑unit visibility
* true per‑thread isolation

Because the default backend lives entirely inside the compiler, it requires no external libraries and works on any platform the JIT itself supports.  It is sufficient for the existing `test/tls.c` test.

---

## 5. Writing a Platform‑Specific Backend
Below are three concrete examples.  Each file should include `zero_cc_tls.h` and implement the four required callbacks.

### 5.1. POSIX (`pthread`) Backend
```c
/* zero_cc_tls_pthread.c */
#include "zero_cc_tls.h"
#include <pthread.h>
#include <stdlib.h>

typedef struct {
    pthread_key_t key;   /* TLS key for the variable */
    ZeroCCType type;     /* Size/alignment info (may be useful) */
    int64_t    init;     /* Constant initializer */
} PThreadTLSHandle;

static void *pthread_declare(ZeroCCCompiler *c,
                              const char *name,
                              const ZeroCCType *type,
                              int64_t init)
{
    (void)c; (void)name;   /* name not needed for POSIX TLS */
    PThreadTLSHandle *h = malloc(sizeof(*h));
    h->type = *type;
    h->init = init;
    pthread_key_create(&h->key, free);   // destructor frees the per‑thread slot
    return h;
}

static void pthread_emit_load(ZeroCCCompiler *c, const void *handle)
{
    const PThreadTLSHandle *h = handle;
    /* Generate a call to a tiny helper that fetches the value using
       pthread_getspecific and returns it in RAX.
       The helper is emitted once per compilation unit (see the code at the
       bottom of the file). */
    zero_cc_emit_u8(&c->text_section, 0xE8);        // call rel32
    size_t patch = c->text_section.length;
    zero_cc_emit_u32(&c->text_section, 0);          // placeholder
    /* Record a relocation so the helper address can be patched later. */
    ZeroCCReloc *r = &c->relocs[c->reloc_count++];
    r->text_patch_offset = patch;
    snprintf(r->target_symbol, sizeof(r->target_symbol), "__tls_pthread_load_%p", h);
    r->is_call = true;
}

static void pthread_emit_store(ZeroCCCompiler *c, const void *handle)
{
    const PThreadTLSHandle *h = handle;
    zero_cc_emit_u8(&c->text_section, 0xE8);        // call rel32
    size_t patch = c->text_section.length;
    zero_cc_emit_u32(&c->text_section, 0);
    ZeroCCReloc *r = &c->relocs[c->reloc_count++];
    r->text_patch_offset = patch;
    snprintf(r->target_symbol, sizeof(r->target_symbol), "__tls_pthread_store_%p", h);
    r->is_call = true;
}

static void pthread_emit_address(ZeroCCCompiler *c, const void *handle)
{
    /* For POSIX TLS there is no real address that stays constant across
       threads, so we emit a call to a helper that returns the per‑thread
       pointer. */
    const PThreadTLSHandle *h = handle;
    zero_cc_emit_u8(&c->text_section, 0xE8);
    size_t patch = c->text_section.length;
    zero_cc_emit_u32(&c->text_section, 0);
    ZeroCCReloc *r = &c->relocs[c->reloc_count++];
    r->text_patch_offset = patch;
    snprintf(r->target_symbol, sizeof(r->target_symbol), "__tls_pthread_addr_%p", h);
    r->is_call = true;
}

static void pthread_finalize(ZeroCCCompiler *c)
{
    (void)c; /* no global resources to free */
}

static const ZeroCCTLSBackend pthread_tls_backend = {
    .declare       = pthread_declare,
    .emit_load     = pthread_emit_load,
    .emit_store    = pthread_emit_store,
    .emit_address  = pthread_emit_address,
    .finalize      = pthread_finalize,
};

/* Export the symbol for user code */
const ZeroCCTLSBackend *zero_cc_pthread_tls_backend = &pthread_tls_backend;

/* -------------------------------------------------------------------
 * Helper runtime functions – they are linked into the final JIT binary.
 * The compiler emits calls to these functions using the mangled names
 * generated above (e.g. "__tls_pthread_load_0x7ff…").
 * ------------------------------------------------------------------- */

/* Load helper – returns the per‑thread value in RAX */
static int64_t __tls_pthread_load_generic(PThreadTLSHandle *h)
{
    int64_t *slot = pthread_getspecific(h->key);
    if (!slot) {
        slot = malloc(sizeof(int64_t));
        *slot = h->init;
        pthread_setspecific(h->key, slot);
    }
    return *slot;
}

/* Store helper – stores RAX into the per‑thread slot */
static void __tls_pthread_store_generic(PThreadTLSHandle *h, int64_t val)
{
    int64_t *slot = pthread_getspecific(h->key);
    if (!slot) {
        slot = malloc(sizeof(int64_t));
        pthread_setspecific(h->key, slot);
    }
    *slot = val;
}

/* Address helper – returns a pointer to the per‑thread slot */
static int64_t *__tls_pthread_addr_generic(PThreadTLSHandle *h)
{
    int64_t *slot = pthread_getspecific(h->key);
    if (!slot) {
        slot = malloc(sizeof(int64_t));
        *slot = h->init;
        pthread_setspecific(h->key, slot);
    }
    return slot;
}
```
**How it works**
* `declare` allocates a `pthread_key_t` and stores the constant initializer.
* `emit_load`, `emit_store`, and `emit_address` all emit *calls* to tiny runtime helpers.  The helpers lazily allocate the per‑thread slot on first use.
* The backend is completely self‑contained; you only need to link `-lpthread` when building the host program.

---

### 5.2. Windows TLS Backend
```c
/* zero_cc_tls_windows.c */
#include "zero_cc_tls.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>

typedef struct {
    DWORD  index;      /* TLS index allocated by TlsAlloc */
    ZeroCCType type;   /* kept for potential size checks */
    int64_t init;      /* constant initializer */
} WinTLSHandle;

static void *win_declare(ZeroCCCompiler *c,
                         const char *name,
                         const ZeroCCType *type,
                         int64_t init)
{
    (void)c; (void)name;
    WinTLSHandle *h = malloc(sizeof(*h));
    h->index = TlsAlloc();
    h->type  = *type;
    h->init  = init;
    return h;
}

static void win_emit_load(ZeroCCCompiler *c, const void *handle)
{
    const WinTLSHandle *h = handle;
    zero_cc_emit_u8(&c->text_section, 0xE8);          // call rel32
    size_t patch = c->text_section.length;
    zero_cc_emit_u32(&c->text_section, 0);
    ZeroCCReloc *r = &c->relocs[c->reloc_count++];
    r->text_patch_offset = patch;
    snprintf(r->target_symbol, sizeof(r->target_symbol), "__tls_win_load_%p", h);
    r->is_call = true;
}

static void win_emit_store(ZeroCCCompiler *c, const void *handle)
{
    const WinTLSHandle *h = handle;
    zero_cc_emit_u8(&c->text_section, 0xE8);
    size_t patch = c->text_section.length;
    zero_cc_emit_u32(&c->text_section, 0);
    ZeroCCReloc *r = &c->relocs[c->reloc_count++];
    r->text_patch_offset = patch;
    snprintf(r->target_symbol, sizeof(r->target_symbol), "__tls_win_store_%p", h);
    r->is_call = true;
}

static void win_emit_address(ZeroCCCompiler *c, const void *handle)
{
    const WinTLSHandle *h = handle;
    zero_cc_emit_u8(&c->text_section, 0xE8);
    size_t patch = c->text_section.length;
    zero_cc_emit_u32(&c->text_section, 0);
    ZeroCCReloc *r = &c->relocs[c->reloc_count++];
    r->text_patch_offset = patch;
    snprintf(r->target_symbol, sizeof(r->target_symbol), "__tls_win_addr_%p", h);
    r->is_call = true;
}

static void win_finalize(ZeroCCCompiler *c)
{
    (void)c; /* nothing to free globally */
}

static const ZeroCCTLSBackend win_tls_backend = {
    .declare      = win_declare,
    .emit_load    = win_emit_load,
    .emit_store   = win_emit_store,
    .emit_address = win_emit_address,
    .finalize     = win_finalize,
};

const ZeroCCTLSBackend *zero_cc_windows_tls_backend = &win_tls_backend;

/* -------------------------------------------------------------------
 * Runtime helpers – compiled into the JIT binary (link with -lkernel32).
 * ------------------------------------------------------------------- */

static int64_t __tls_win_load_generic(WinTLSHandle *h)
{
    int64_t *slot = (int64_t *)TlsGetValue(h->index);
    if (!slot) {
        slot = malloc(sizeof(int64_t));
        *slot = h->init;
        TlsSetValue(h->index, slot);
    }
    return *slot;
}

static void __tls_win_store_generic(WinTLSHandle *h, int64_t val)
{
    int64_t *slot = (int64_t *)TlsGetValue(h->index);
    if (!slot) {
        slot = malloc(sizeof(int64_t));
        TlsSetValue(h->index, slot);
    }
    *slot = val;
}

static int64_t *__tls_win_addr_generic(WinTLSHandle *h)
{
    int64_t *slot = (int64_t *)TlsGetValue(h->index);
    if (!slot) {
        slot = malloc(sizeof(int64_t));
        *slot = h->init;
        TlsSetValue(h->index, slot);
    }
    return slot;
}
```
**Notes**
* `TlsAlloc` returns a per‑process index that resolves to a per‑thread pointer.
* The helper functions lazily allocate the per‑thread storage on first use, matching the semantics of the POSIX backend.
* Link the final program with `-lkernel32` (the default on Windows) – no extra flags needed.

---

### 5.3. Bare‑Metal / Embedded Backend
On a microcontroller or other freestanding environment you may not have an OS‑level TLS mechanism.  A common pattern is to allocate a **single global variable** per `_Thread_local` and then let the application provide a tiny context‑switch hook that swaps the values.

```c
/* zero_cc_tls_simple.c */
#include "zero_cc_tls.h"
#include <stdlib.h>

typedef struct {
    int64_t  value;   /* single instance – not truly per‑thread */
    ZeroCCType type;
} SimpleTLSHandle;

static void *simple_declare(ZeroCCCompiler *c,
                             const char *name,
                             const ZeroCCType *type,
                             int64_t init)
{
    (void)c; (void)name;
    SimpleTLSHandle *h = malloc(sizeof(*h));
    h->type = *type;
    h->value = init;
    return h;
}

static void simple_emit_load(ZeroCCCompiler *c, const void *handle)
{
    const SimpleTLSHandle *h = handle;
    zero_cc_emit_u8(&c->text_section, 0x48);
    zero_cc_emit_u8(&c->text_section, 0xB8);
    zero_cc_emit_u64(&c->text_section, (uint64_t)h->value);
    c->last_expr_is_float = 0;
}

static void simple_emit_store(ZeroCCCompiler *c, const void *handle)
{
    SimpleTLSHandle *h = (SimpleTLSHandle *)handle;
    /* The value is currently in RAX – we cannot directly read it from the
       code stream, so we generate a call to a tiny runtime stub that copies
       RAX into the struct. */
    zero_cc_emit_u8(&c->text_section, 0xE8);
    size_t patch = c->text_section.length;
    zero_cc_emit_u32(&c->text_section, 0);
    ZeroCCReloc *r = &c->relocs[c->reloc_count++];
    r->text_patch_offset = patch;
    snprintf(r->target_symbol, sizeof(r->target_symbol), "__tls_simple_store_%p", h);
    r->is_call = true;
}

static void simple_emit_address(ZeroCCCompiler *c, const void *handle)
{
    const SimpleTLSHandle *h = handle;
    zero_cc_emit_u8(&c->text_section, 0x48);
    zero_cc_emit_u8(&c->text_section, 0xB8);
    zero_cc_emit_u64(&c->text_section, (uint64_t)&h->value);
}

static const ZeroCCTLSBackend simple_tls_backend = {
    .declare       = simple_declare,
    .emit_load     = simple_emit_load,
    .emit_store    = simple_emit_store,
    .emit_address  = simple_emit_address,
    .finalize      = NULL,
};

const ZeroCCTLSBackend *zero_cc_simple_tls_backend = &simple_tls_backend;

/* Runtime stub for the store – placed in the final binary */
static void __tls_simple_store_generic(SimpleTLSHandle *h)
{
    /* RAX is the return value of the function that called us.  On x86‑64
       the calling convention passes the first integer argument in RDI.
       Therefore the stub must be written in assembly to move RDI into the
       struct member.  For illustration we just provide a C stub that would
       be called with the value as a parameter. */
    extern int64_t __builtin_get_rax(void);   // compiler‑builtin (pseudo)
    h->value = __builtin_get_rax();
}
```
**Why this works on bare metal**
* There is no OS TLS, so we keep a **single instance** per variable.  
* The backend can be swapped later with a real per‑core or per‑task storage scheme without changing the compiler front‑end.
* The example shows how to emit a call to a tiny runtime stub for stores – on a real embedded system you would replace the stub with inline assembly that moves the value directly.

---

## 6. Choosing a Backend at Compile‑time
If you want the decision to be baked into the binary (no runtime API), use a macro in your build system:

```c
/* config.h – generated by your build system */
#if defined(PLATFORM_POSIX)
#   define ZERO_CC_TLS_BACKEND  zero_cc_pthread_tls_backend
#elif defined(PLATFORM_WINDOWS)
#   define ZERO_CC_TLS_BACKEND  zero_cc_windows_tls_backend
#else
#   define ZERO_CC_TLS_BACKEND  &zero_cc_default_tls_backend
#endif
```
Then, after creating the compiler instance:

```c
ZeroCCCompiler comp = {0};
zero_cc_tokenize_c99(src, &comp);
zero_cc_set_tls_backend(&comp, ZERO_CC_TLS_BACKEND);
zero_cc_compile_c99(&comp);
```
No additional runtime overhead – the backend is a compile‑time constant.

---

## 7. Choosing a Backend at Run‑time (API)
The library already provides `zero_cc_set_tls_backend`.  Example usage:

```c
ZeroCCCompiler comp = {0};
zero_cc_tokenize_c99(src, &comp);

/* User decides at runtime which backend to use */
if (use_posix) {
    zero_cc_set_tls_backend(&comp, zero_cc_pthread_tls_backend);
} else if (use_windows) {
    zero_cc_set_tls_backend(&comp, zero_cc_windows_tls_backend);
} else {
    zero_cc_set_tls_backend(&comp, NULL);   // falls back to default static‑alias
}

zero_cc_compile_c99(&comp);
```
The API is deliberately tiny; you can expose a higher‑level function in your own library that hides the selection logic.

---

## 8. Testing Your Backend
1. **Unit‑test the backend in isolation** – write a small program that calls `declare`, `emit_load`, etc., and checks the generated machine code (using the existing `zero_cc_buf_*` helpers).
2. **Integration tests** – add new test sources that use:
   * mutable writes (`tls_var = 123;`)
   * address‑of (`int *p = &tls_var; *p = 5;`)
   * block‑scope `_Thread_local` variables.
3. Run the full test suite with `zero_cc_set_tls_backend(&c, my_backend);` to ensure the front‑end continues to function.
4. For POSIX/Windows backends, run the tests on the target OS to verify that per‑thread isolation works (spawn multiple threads, each should see its own copy).

---

## 9. Best Practices & Gotchas
* **Never store pointers to the compiler’s internal `thread_locals[]` array** in generated code unless you are using the default static‑alias backend.  Other backends treat the handle as opaque.
* **Alignment matters** – if your backend places the variable in a data section, make sure the emitted address respects `type->align`.
* **Thread‑local destructors** – if your platform supports a destructor callback (POSIX `pthread_key_create` takes a destructor, Windows `TlsFree` does not), implement it in the `finalize` hook.
* **Avoid heavy work in the callbacks** – they are called during code generation, which should stay fast.  Allocate any large structures **once** in `declare` and reuse them.
* **Static linking** – if you provide a backend in a separate library, make sure the symbols used for the runtime helpers (`__tls_pthread_load_…`) are exported (use `__attribute__((visibility("default")))` on GCC/Clang or a .def file on Windows).
* **Cross‑translation‑unit visibility** – the backend must guarantee that a variable declared in one compilation unit can be referenced from another.  For POSIX/Windows this happens automatically because the TLS key is global.  For the simple static‑alias backend you would need to place the variable in the compiler’s `.data` section and emit a global symbol.

---

## 10. Full Example: A Simple Logger Backend
Below is a minimal backend that **logs every access** (load, store, address) to `stderr`.  It still uses the default static‑alias storage for the actual value.

```c
/* zero_cc_tls_logger.c */
#include "zero_cc_tls.h"
#include <stdio.h>

static void *log_declare(ZeroCCCompiler *c,
                         const char *name,
                         const ZeroCCType *type,
                         int64_t init)
{
    fprintf(stderr, "[TLS] declare %s = %lld (size=%zu)\n",
            name, (long long)init, (size_t)type->size);
    return zero_cc_default_tls_backend.declare(c, name, type, init);
}

static void log_load(ZeroCCCompiler *c, const void *h)
{
    fprintf(stderr, "[TLS] load\n");
    zero_cc_default_tls_backend.emit_load(c, h);
}

static void log_store(ZeroCCCompiler *c, const void *h)
{
    fprintf(stderr, "[TLS] store\n");
    zero_cc_default_tls_backend.emit_store(c, h);
}

static void log_address(ZeroCCCompiler *c, const void *h)
{
    fprintf(stderr, "[TLS] address\n");
    zero_cc_default_tls_backend.emit_address(c, h);
}

static const ZeroCCTLSBackend logger_backend = {
    .declare      = log_declare,
    .emit_load    = log_load,
    .emit_store   = log_store,
    .emit_address = log_address,
    .finalize     = NULL,
};

/* Exported symbol for user code */
const ZeroCCTLSBackend *zero_cc_logger_tls_backend = &logger_backend;
```
Compile it and link it with your program, then:

```c
zero_cc_set_tls_backend(&comp, zero_cc_logger_tls_backend);
```
Running any program that uses `_Thread_local` will now print diagnostic lines to `stderr`, proving that the plug‑in mechanism works.

---

## 11. License
All code in this repository—including the TLS‑backend examples—is released under the **MIT License** (see the `LICENSE` file).  You are free to adapt, extend, or replace any backend implementation for your own projects.

---

*Enjoy building portable, extensible TLS support for `zero_cc`!*