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

### Known Limitations

- `_Atomic` types are supported for JIT execution only. The ELF object/executable emitters do not yet implement atomic relocations or special section handling. `sizeof(_Atomic T)` and `_Alignof(_Atomic T)` are correctly reported as the size/alignment of `T` for both JIT and ELF paths, but actual atomic code generation for ELF emission is a TODO.

Semantic analysis for these features is currently a work in progress. The tokenizer correctly recognizes these keywords and returns the appropriate token types, enabling further implementation in the parser and expression evaluator.

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
