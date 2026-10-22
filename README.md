# zero_cc - Standalone Embeddable C99/C11/C23 Compiler

A lightweight C compiler designed for embedding or standalone use. Modular architecture with separate source files. Features include:

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
- **Compiler diagnostics** — gcc-style caret errors, C99 constraint checking, and "did you mean" suggestions
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
│       ├─ parser/        # AST parser, lowering, IR, and const eval
│       ├─ statements/    # Type definition & static assert handlers
│       ├─ isa/           # Backend implementations
│       │   ├─ x86_64.c
│       │   ├─ aarch64.c
│       │   └─ riscv.c
│       ├─ types/         # Type definitions
│       │   └─ vregs.h    # Virtual register definitions
│       └─ jit/           # JIT compilation & ELF emission
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

## Test Results

| Suite | Status | Count |
|-------|--------|-------|
| **Main regression suite** (`make test`) | ✅ Pass | 147/147 |
| **Optimizer sweep** (`make test-opt`, `-O0..-O3` × JIT + AOT via zerocc) | ✅ Pass | 107/107 |
| **Cross-ISA raw-emitter QEMU gate** (`make test-isa-qemu`, vs static-musl reference) | ✅ Pass | 44/44 (11/11 × x86-64, aarch64, riscv64, riscv32) |
| **`sret` ABI verifier** | ✅ Pass | 6/6 |
| **C11 verification suite** | ✅ Pass | 11/11 |
| **Diagnostics battery** (`tools/diag_battery.py`, vs gcc) | ✅ Pass | 16 cases, 8/8 error categories |
| **C style checker** (`tools/style_check.py`) | ✅ Pass | 0 errors |

## Compiler Diagnostics

Diagnostics aim for gcc/clang-style quality (Sprint 42):

- **Format**: `file:line:col: error: message` with source carets; macro-expansion `note:` chains
- **C99 constraint checking** (previously these invalid programs compiled silently): call argument count vs. prototype (6.7.5.3, variadic-aware), `goto` target must be a defined label (6.8.4.3), struct/union member must exist (6.5.2.1), block closing `}` required (6.8) — no-prototype `f()` calls stay unconstrained per the standard
- **"Did you mean" suggestions** for undeclared identifiers and unknown struct members (Levenshtein ≤ 2, unique-best only — ambiguous matches are not suggested)
- **Multi-error recovery** (clang-style error budget of ~20): one pass reports all faults in a broken file instead of stopping at the first error; further diagnostics are capped with a single "too many errors emitted" note — gated by `tools/diag_battery.py`

## C11/C23 Support

### Implemented Features

| Feature | Status | Backend |
|---------|--------|---------|
| **`_Thread_local`** | ✅ Complete | `tls_backend` |
| **`_Atomic`** | 📋 Planned | `atomic_backend` |
| **`_Alignas` / `_Alignof`** | 📋 Planned | `abi_backend` |
| **`_Noreturn`** | ✅ Parser | - |
| **`_Static_assert`** | ✅ Implemented | - |
| **`_Generic`** | ✅ Simplified | - |
| **`inline` semantics** | 📋 Planned | `isa_backend` |
| **Complex numbers** | 📋 Planned | `isa_backend` |

### Preprocessor Pragmas (Sprint 2.5)

| Pragma | Status | Notes |
|--------|--------|-------|
| **`#pragma pack(n)`** | ✅ Complete | Structure packing control with push/pop |
| **`#pragma message("text")`** | ✅ Complete | Compiler messages |
| **`#pragma GCC diagnostic push/pop`** | ✅ Complete | Diagnostic state stack |
| **`#pragma GCC diagnostic error/warning/ignored`** | ✅ Complete | Warning control |
| **`#pragma GCC push_options` / `pop_options`** | ✅ Complete | Options stack |

### Known Limitations

- `_Thread_local`: Full system TLS support (fs/gs segments on x86-64)
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

## Documentation & Knowledge Graph (`docs/okf/`)

ZeroCC maintains an **Open Knowledge Format (OKF v0.2)** knowledge graph in [`docs/okf/`](docs/okf/index.md), while historical design notes are preserved in [`docs/archive/`](docs/archive/):

| Document | Description |
|----------|-------------|
| [`docs/okf/index.md`](docs/okf/index.md) | Root Knowledge Graph Navigator |
| [`docs/okf/architecture-pipeline.md`](docs/okf/architecture-pipeline.md) | Two-pass compilation pipeline & source file map |
| [`docs/okf/frontend-and-types.md`](docs/okf/frontend-and-types.md) | Type system, structs, VLAs, and C11 features (`_Generic`, `_Alignas`, `<threads.h>`) |
| [`docs/okf/ir-and-regalloc.md`](docs/okf/ir-and-regalloc.md) | 51-opcode `Instr` IR and linear-scan register allocation |
| [`docs/okf/optimization-passes.md`](docs/okf/optimization-passes.md) | Active IR optimization passes (`mem2reg`, `licm`, `auto_vectorize`, etc.) |
| [`docs/okf/abi-sysv-x86_64.md`](docs/okf/abi-sysv-x86_64.md) | System V AMD64 ABI & `>16B` struct return (`sret`) rules |
| [`docs/okf/isa-backends.md`](docs/okf/isa-backends.md) | Multi-ISA backends (x86-64, AArch64, RISC-V) |
| [`docs/okf/build-and-verification.md`](docs/okf/build-and-verification.md) | Build targets, testing, and verification workflows |
| [`docs/CHANGELOG.md`](docs/CHANGELOG.md) | Full release notes and changelog |
| [`docs/ZEROCC_Development_Plan.md`](docs/ZEROCC_Development_Plan.md) | Project development roadmap |
| [`tools/README.md`](tools/README.md) | Verification & tooling suite documentation |

## Verification & Tooling Suite

ZeroCC includes automated verification suites in [`tools/`](tools/) (historical debugging scripts are archived in [`tools/archive/`](tools/archive/)):

```bash
# Run C11 feature & concurrency verification suite (11 tests)
python3 tools/c11/test_c11.py --verbose

# Run C99 VLA & multi-dimensional matrix VLA verification suite
python3 tools/vla/test_vla.py

# Verify System V AMD64 >16B struct return (sret) ABI
python3 tools/sret_verifier.py --verify-all

# Differential diagnostics battery vs gcc (ratchet baseline; run after
# parser/semantic/diagnostic changes)
python3 tools/diag_battery.py

# C style/convention checker (docs/coding_style.md rules)
python3 tools/style_check.py

# Compare ISA compilation across x86-64, AArch64, and RISC-V
python3 tools/cross_isa_comparator.py test/aot/hello_world.c

# Validate the OKF v0.2 Knowledge Graph
python3 tools/okf.py lint && python3 tools/okf.py index
```

See [`tools/README.md`](tools/README.md) for full documentation.

## License

ZeroCC is released under the MIT License. See the `LICENSE` file for details.

## Self-Hosting

ZeroCC is designed to eventually **self-host** (compile itself). Current status:

- **Phase 1**: Compile simple C programs ✅
- **Phase 2**: Compile ZeroCC itself (in progress)
- **Phase 3**: Full C11 conformance with self-hosting

## Acknowledgements

This code was extracted from the [zero.h](https://github.com/earendil-works/zero.h) project and adapted to be a standalone, modular compiler.
