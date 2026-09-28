# zero_cc - Standalone Embeddable C99/C11/C23 Compiler

A lightweight, header-only C compiler designed for embedding in applications. Features include:

- C99 freestanding compiler (with optional C11/C23 extensions)
- Preprocessor (`#include`, `#define`, `#if/#ifdef`, etc.)
- Type system (structs, pointers, arrays, enums)
- Control flow (`if/else`, `switch`, loops, `break/continue/return`)
- GNU statement expressions (`({ ... })`)
- Function calls with System V AMD64/ARM64 ABI
- JIT execution via `mmap` + `dlsym` host symbol resolution
- ELF64 object and executable emission
- **AST lowering to AMD64 machine code (`zero_cc_lower_ast`)**
- Zero libc dependency (uses standard C/POSIX functions directly, with a small inline utility for zero_strlcpy).

## Directory Structure

```
zero_cc/
├─ include/
│   └─ zero_cc.h          # Public façade (includes inline zero_strlcpy utility)
├─ src/
│   └─ zero_cc/           # Implementation headers
│       ├─ core.h
│       ├─ types_lexer.h
│       ├─ expressions.h
│       ├─ statements.h
│       ├─ jit_linker.h
│       ├─ zero_arm64_emitter.h
│       └─ zero_mmap_adapter.h   # mmap/mprotect/munmap wrappers
├─ third_party/
│   └─ vendor/            # Vendored dependencies (musl, quickjs, bearssl)
├─ test/
│   ├─ basic.c            # Preprocessor sanity check
│   ├─ jit_test.c         # JIT compilation and execution
│   ├─ elf_test.c         # ELF object emission
│   └─ exe_test.c         # ELF executable emission and execution
├─ LICENSE                # MIT license
└─ meson.build            # Minimal Meson build file
```

## Building

Since zero_cc is header-only, there is no compilation step for the library itself.
To use it, simply add `include/` to your compiler's include path and `#include "zero_cc.h"`.

### Example

```c
#include <stdio.h>
#include "zero_cc.h"

int main(void) {
    const char *src = "int answer(void) { return 42; }\n"
                      "int main(void) { return answer(); }\n";
    ZeroCCCompiler *comp = calloc(1, sizeof(ZeroCCCompiler));
    char *prep = zero_cc_preprocess(src, NULL);
    zero_cc_tokenize_c99(prep, comp);
    zero_cc_compile_c99(comp);
    int result = zero_cc_execute_jit(comp, NULL);
    printf("JIT result: %d\n", result);
    /* Cleanup */
    zero_cc_buf_free(&comp->text_section);
    zero_cc_buf_free(&comp->rodata_section);
    zero_cc_buf_free(&comp->data_section);
    free(comp->tokens);
    free(comp);
    return result == 42 ? 0 : 1;
}
```

Compile with:

```bash
gcc -Isrc -Iinclude example.c -o example
```

### Using Musl

If you wish to build your application with **musl libc** for a fully static, GLIBC-free binary, use the musl toolchain:

```bash
musl-gcc -Isrc -Iinclude example.c -o example -static
```

The vendored musl source is available under `third_party/vendor/musl/` for reference or rebuilding.

## Running the Test Suite

A small test suite is provided in the `test/` directory. Build and run each test:

```bash
# Preprocessor test
gcc -Isrc -Iinclude test/basic.c -o test/basic
./test/basic

# JIT test
gcc -Isrc -Iinclude test/jit_test.c -o test/jit_test
./test/jit_test

# ELF object test
gcc -Isrc -Iinclude test/elf_test.c -o test/elf_test
./test/elf_test
readelf -h test.o   # verify ELF object

# ELF executable test
gcc -Isrc -Iinclude test/exe_test.c -o test/exe_test
./test/exe_test
```

## C11/C23 Support

Zero_cc now includes tokenizer support for the following C11 and C23 keywords:

- `_Static_assert`
- `_Alignas`, `_Alignof`
- `_Atomic`
- `_Noreturn`
- `_Thread_local`
- `typeof`, `typeof_unqual`
- `true`, `false`, `nullptr` (as keywords)

### Implemented Features

**Milestone 1 – `_Static_assert`**
- File, struct and statement scope, optional message, C23 one-argument form
- Compile-time constant evaluator with `sizeof`/`_Alignof`
- Negative test for failing assertions

**Milestone 2 – `_Alignas` / `_Alignof`**
- Alignment queries and overrides for variables and struct members
- Struct layout respects `_Alignas`
- Type-only parser for constant contexts

**Milestone 3 – `_Atomic`**
- Parser for `_Atomic T` and `_Atomic(T)`, `is_atomic` flag in `ZeroCCType`
- `sizeof/_Alignof` return underlying type size/alignment
- Atomic builtins `__atomic_load_n`, `__atomic_store_n`, `__atomic_add_fetch` and memory-order constants are self-hosted via preprocessor macros, making the compiler self-hostable for Milestone 3
- Pointer l-value code generation for `*p = expr` is supported
- JIT tests verify compilation and execution of atomic types and static asserts

**Milestone 4 – `_Noreturn`**
- Parser records `_Noreturn` on function definitions and prototypes
- Flag stored in `ZeroCCFunction.noreturn`

**Milestone 5 – `_Thread_local`**
- Implemented as a static‑alias storage: tokenizer, parser and code‑generation now handle top‑level `_Thread_local` declarations and reads (no mutable or address‑of support yet).

**Milestone 6 – `_Generic`**
- Primary expression parser handles `_Generic` with type-only evaluation of controlling expression
- Pointer and base type matching supported; default branch works

**Milestone 7 – `__has_include`**
- Preprocessor `#if __has_include(...)` evaluates to 1/0 via virtual headers / filesystem check

**Milestone 8 – Convenience Headers**
- `<stdalign.h>`, `<stdnoreturn.h>`, `<assert.h>`, `<stdatomic.h>` provided as virtual headers with C11 macros

### Known Limitations

- `_Atomic` types are supported for JIT execution with self‑hosted builtins. The ELF object/executable emitters currently emit the same layout as the underlying type; true atomic instruction selection / relocations for ELF is deferred.
- `_Generic` provides simplified type matching; full C11 generic selection compatibility rules are not yet implemented.
- `_Thread_local` is implemented as a static alias; mutable writes and address‑of (`&`) are not supported.
- **Pluggable backend architecture** – the compiler now isolates TLS handling, code‑generation, symbol resolution, allocation, and OS‑specific emission behind small callback tables.  This makes it easy to add new ISAs (e.g. RISC‑V), alternative TLS implementations (pthread, Windows TLS), custom allocators, or bare‑metal object emitters without touching the front‑end.  See the **Backend Documentation** section below.

### Backend Documentation

### Backend Architecture Overview

Zero‑CC now uses a clean, layered backend architecture to avoid circular includes and make it easy to plug in target‑specific implementations.

* **`zero_cc_backends.h`** – defines **only the abstract backend interfaces** (TLS, Emit, Link, Memory, ABI, Atomic, Generic, Stdlib, Diagnostics) and the setter function **prototypes**. It does **not** include the compiler definition. All backend structs use `struct ZeroCCCompiler *` / `struct ZeroCCType *` forward declarations.
* **`src/zero_cc/types_lexer.h`** – owns the concrete `ZeroCCCompiler` struct (with tag `struct ZeroCCCompiler`) and **forward‑declares** each backend type before including `zero_cc_backends.h`. This allows the compiler to hold pointers to the backends without creating a dependency cycle.
* **Setter implementations** live in `src/zero_cc/backends/setters.c` (which includes `types_lexer.h` after the full definition).
* **Backend implementations** live in `src/zero_cc/backends/` (e.g. `minimal.c`). They include only `zero_cc_backends.h` and register themselves with `zero_cc_init_backends`.
* **TPHT headers** (`zero_tpht.h`, `zero_chained_tpht.h`, `zero_tinyptr.h`, `zero_posix.h`) are vendored under `src/zero_cc/structures/` from the `qnz_gcc` project for high‑performance hash tables.
* **TPHT integration** – the TPHT‑backed hash table implementation (`hash_table_tpht.c`) now includes proper `<stdlib.h>` and `<stdio.h>` headers, manual TPHT block allocation, and passes all unit tests (`hash_table_tpht_test`, `hash_table_tpht_stress_test`, `hash_table_tpht_duplicate_test`). Fixed: hash function diffusion (splitmix64), fingerprint array sizing, `size` counter double-count on updates, and stress-test capacity assumptions.

This separation guarantees a one‑way include relationship:
```
front‑end -> backend interface -> concrete back‑ends
```
and prevents the “request for member … in something not a structure or union” errors that arise from circular includes.

When adding a new backend, simply implement the appropriate struct in `src/zero_cc/backends/` and call its setter (or use `zero_cc_init_backends`) after allocating a `ZeroCCCompiler`.

---


- **TLS Backends** – detailed guide on adding custom thread‑local storage implementations: [TLS_BACKENDS.md](TLS_BACKENDS.md)
- **General Backend Architecture** – overview of the code‑generation, symbol, allocation, and OS backends (also documented in the source headers).

## License

Zero_cc is released under the MIT License. See the `LICENSE` file for details.

## Acknowledgements

This code was extracted from the [zero.h](https://github.com/earendil-works/zero.h) project and adapted to be a standalone, embeddable compiler.

- `_Atomic` types are supported for JIT execution with self-hosted builtins. The ELF object/executable emitters currently emit the same layout as the underlying type; true atomic instruction selection / relocations for ELF is deferred.
- `_Generic` provides simplified type matching; full C11 generic selection compatibility rules are not yet implemented.
- `_Thread_local` is implemented as a static alias; mutable writes and address‑of (`&`) are not supported.

Most of these features are implemented in the parser and expression evaluator, with full semantic analysis and a complete C11-compliant type system being a work in progress.

To add support for a new feature:

1. Extend the tokenizer (`ZeroCCTokType` in `types_lexer.h`) with new token types if needed.
2. Add keyword-to-token mappings in the tokenizer function.
3. Implement semantic actions in `expressions.h` and `statements.h` (or declaration handlers).
4. Update the type system to represent new type categories (e.g., `TYPE_ATOMIC`).
5. Adjust builtin macros in `core.h` (`zero_cc_cpp_define_builtin_macros`) to define appropriate feature test macros (`__STDC_VERSION__`).
6. Add test cases in `test/` to verify the new features.

## License

Zero_cc is released under the MIT License. See the `LICENSE` file for details.

## Acknowledgements

This code was extracted from the [zero.h](https://github.com/earendil-works/zero.h) project and adapted to be a standalone, embeddable compiler.
