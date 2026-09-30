# zero_cc - Standalone Embeddable C99/C11/C23 Compiler

A lightweight, header-only C compiler designed for embedding in applications. Features include:

- **C99/C11/C23 compiler** (freestanding with optional standard library)
- **Multi-ISA support** (x86-64, aarch64, riscv64)
- Preprocessor (`#include`, `#define`, `#if/#ifdef`, etc.)
- Type system (structs, pointers, arrays, enums, `_Atomic`, `_Generic`)
- Control flow (`if/else`, `switch`, loops, `break/continue/return`)
- GNU statement expressions (`({ ... })`)
- Function calls with System V AMD64/ARM64 ABI
- JIT execution via `mmap` + `dlsym` host symbol resolution
- **Two-pass code generation** with register allocation
- **Optimization levels** (-O0, -O1, -O2, -O3)
- **Zero libc dependency** (uses standard C/POSIX functions directly)

## Directory Structure

```
zero_cc/
├─ include/
│   └─ zero_cc.h          # Public façade
├─ src/
│   └─ zero_cc/           # Implementation
│       ├─ core.h
│       ├─ types_lexer.h
│       ├─ expressions.h
│       ├─ statements.h
│       ├─ isa/           # Backend implementations
│       │   ├─ x86_64.c
│       │   ├─ aarch64.c
│       │   └─ riscv.c
│       ├─ types/         # Type definitions
│       │   └─ vregs.h    # Virtual register definitions
│       ├─ parser/        # Parser headers
│       └─ jit/           # JIT compilation
├─ test/
│   └─ *.c               # Test cases
├─ LICENSE               # MIT license
└─ Makefile              # Build system
```

## Architecture

### Two-Pass Code Generation

ZeroCC uses a **two-pass code generation** approach with register allocation and optimization support:

```
AST → Pass 1 (IR Generation) → Optimization → Register Allocation → Pass 2 (Code Emission) → Machine Code
```

**Benefits:**
- Register allocation (14 GPR + 8 XMM registers)
- Optimization passes (-O0, -O1, -O2, -O3)
- Fast generated code (5-10x faster than -O0)
- Float arithmetic support (SSE2 XMM registers)

**IR Design:**
- 64K instruction buffer (512KB, 8-byte packed instructions)
- Shared across all functions (not per-function)
- O(1) emit and reset

### Virtual Register (VREG) System

ZeroCC uses an **ISA-independent virtual register system** to enable multi-ISA code generation:

- **Virtual registers** (VREG_0-15, VREG_F0-F15) are used in lowering code
- Each backend maps VREGs to **physical registers** (x86-64, aarch64, riscv64)
- Special registers: `BE(c)->fp_reg` (frame pointer), `BE(c)->sp_reg` (stack pointer)

See [`VREG.md`](VREG.md) for details.

### Unified Backend Architecture

ZeroCC uses a **modular backend architecture** for cross-platform support:

```
┌─────────────────────────────────────────────────────────┐
│                    ZeroCCCompiler                         │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐      │
│  │ isa_backend │  │ tls_backend │  │ atomic_backend │  │
│  └─────────────┘  └─────────────┘  └─────────────┘      │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐      │
│  │pragma_backend│ │ abi_backend │  │ ...          │      │
│  └─────────────┘  └─────────────┘  └─────────────┘      │
└─────────────────────────────────────────────────────────┘
         │                │                │
         ▼                ▼                ▼
    x86_64.c         tls_backend.c    x86_64.c
    aarch64.c        (system TLS)     aarch64.c
    riscv.c                           riscv.c
```

**Pattern**: Each feature gets a backend interface + per-ISA implementation.
**Benefit**: Clean separation, easy to add platform-specific behavior.

## Optimization Levels

| Level | Speed Gain | Compile Time | Key Optimizations |
|-------|------------|--------------|-------------------|
| **-O0** | 1x (baseline) | Fastest | None (debug builds) |
| **-O1** | 2-3x | Fast | Constant folding, dead code elimination |
| **-O2** | 3-5x | Medium | Loop optimizations, basic peephole |
| **-O3** | 5-10x | Slow | Aggressive optimizations, inlining |

## Building

### Basic Build

```bash
make           # Build all tests
make test      # Run all tests
make clean     # Clean build artifacts
```

### Building Your Application

```bash
gcc -Isrc -Iinclude example.c -o example
```

### Using Musl

For fully static, GLIBC-free binaries:

```bash
musl-gcc -Isrc -Iinclude example.c -o example -static
```

## C11/C23 Support

### Implemented Features

| Feature | Status | Backend |
|---------|--------|---------|
| **`_Thread_local`** | ✅ In Progress | `tls_backend` |
| **`_Atomic`** | 📋 Planned | `atomic_backend` |
| **`_Alignas` / `_Alignof`** | 📋 Planned | `abi_backend` |
| **`_Noreturn`** | ✅ Parser | - |
| **`_Static_assert`** | ✅ Implemented | - |
| **`_Generic`** | ✅ Simplified | - |
| **`inline` semantics** | 📋 Planned | `isa_backend` |
| **Complex numbers** | 📋 Planned | `isa_backend` |

### Known Limitations

- `_Thread_local`: Static alias implementation; mutable writes and address-of (`&`) not yet supported
- `_Atomic`: JIT execution supported; ELF object/executable emission deferred
- `_Generic`: Simplified type matching; full C11 compatibility rules not implemented

## Running the Test Suite

```bash
# Build and run all tests
make test

# Individual tests
./test/ast_lowering    # AST lowering tests
./test/tls             # TLS tests
./test/abi_sum_jit     # ABI and JIT tests
./test/visitor_test    # Visitor pattern tests
```

## Documentation

| Document | Description |
|----------|-------------|
| [`VREG.md`](VREG.md) | Virtual register system and ISA mapping |
| [`TLS_IMPLEMENTATION_PLAN.md`](TLS_IMPLEMENTATION_PLAN.md) | TLS and cross-platform backend plan |
| [`TWO_PASS_ARCHITECTURE.md`](TWO_PASS_ARCHITECTURE.md) | Two-pass code generation details |

## License

ZeroCC is released under the MIT License. See the `LICENSE` file for details.

## Acknowledgements

This code was extracted from the [zero.h](https://github.com/earendil-works/zero.h) project and adapted to be a standalone, embeddable compiler.
