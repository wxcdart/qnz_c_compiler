# zero_cc — Standalone Embeddable C99/C11 Compiler

ZeroCC is a lightweight, embeddable C99/C11 compiler (with C23 `[[...]]`
attribute support) with a two-pass architecture: a flat 133-opcode IR that is
lowered, optimized, register-allocated, and emitted to any of four target ISAs.
It compiles to machine code for a JIT (`--run`, the default) or to assembly,
ELF objects, and — via ZeroCC's own linker — directly-executable freestanding
binaries linked against its own `zero_rt` runtime, with no host libc involved.

## Features

- **C99/C11** compiler, freestanding, with optional `zero_rt` standard library
- **Multi-ISA**: x86-64, AArch64, RISC-V 64, RISC-V 32 (`--target`)
- **Multi-translation-unit driver**: multiple `.c` inputs per invocation
  (JIT image merge + per-input `-c`/`-S`)
- **Own ELF linker** (`--zerocc-link`): static `ET_EXEC` output with no libc
  and no crt files, plus `zero_rt` freestanding runtime auto-linking
- Preprocessor (`#include`, `#define`, `#if/#ifdef`, virtual C11 headers
  `<stdatomic.h>` / `<stdnoreturn.h>` / `<threads.h>`, `-E` preprocess-only)
- Type system (structs/unions, bitfields, pointers, arrays, VLAs, enums,
  `_Atomic`, `_Generic`, designated/aggregate initializers)
- Control flow (`if/else`, `switch`, loops, `goto`, `break/continue/return`)
- GNU statement expressions (`({ ... })`)
- System V AMD64 / AArch64 / RISC-V calling conventions, including by-value
  structs and hidden-pointer (`sret`) returns for `>16B` structs
- JIT execution via `mmap` + `dlsym` host symbol resolution
- **Two-pass code generation**: IR → optimization → linear-scan register
  allocation → machine code
- **Optimization levels** `-O0`…`-O3` (constant folding, DCE, copy
  propagation, mem2reg, CSE, GVN, LICM, branchless, strength reduction,
  auto-vectorization to SSE2/SSE4.1, NEON, and RVV; small-function inlining
  at `-O2`/`-O3`)
- **Compiler diagnostics** — gcc-style caret errors, C99 constraint checking,
  "did you mean" suggestions, multi-error recovery
- **Conformance gate** ratcheted against GCC's `gcc.c-torture` corpus
  (3,940 tests; see `make test-torture`)

## Quick Start

```bash
make            # build build/bin/zerocc + test binaries + zero_rt.a (all ISAs)
make test       # run the full regression suite

# JIT run (default) or AOT compile
build/bin/zerocc prog.c                    # JIT-run prog.c
build/bin/zerocc prog.c -o prog             # AOT executable (system linker)
build/bin/zerocc prog.c -S                  # emit assembly
build/bin/zerocc prog.c -c -o prog.o        # emit an ELF object
build/bin/zerocc prog.c --zerocc-link -o prog  # own linker + zero_rt, no libc
```

### CLI

```
zerocc [options] <source.c> [more.c ...]

  -o <file>       Output file (executable or object with -c)
  -c              Compile to object file (.o) only
  -S              Emit assembly
  -E              Preprocess only, to stdout (or -o file)
  -O0..-O3        Optimization level (-O1 is the default;
                  -O / -Os / -Oz / -Ofast are aliases for 1 / 2 / 3 / 3)
  -fomit-frame-pointer    Omit frame pointers in fixed-frame functions
                          (default at -O1+)
  -fno-omit-frame-pointer Keep frame pointers in every non-leaf function
  -I<dir>  -isystem <dir>  Add include / system include directory
  -D<macro>  -U<macro>     Define / undefine a macro
  -include <file>          Force-include a file before the main source
  -run, --run     Run in JIT mode (default)
  --target <arch> Target architecture (x86-64, aarch64, riscv64, riscv32)
  --zerocc-link  With -o: link with ZeroCC's own linker (x86-64, aarch64,
                riscv64); auto-links zero_rt (opt out: --no-zerocc-rt)
  -MMD / -MF <file> / -MT <target>   Dependency-file generation
  -dump-ir      Dump IR before optimization
  -v, --verbose   Verbose output
  --version     Print version
  -h, --help      Show help
```

Common gcc flags such as `-g`, `-Wall`, `-fPIC`, `-pthread`, `-static`, and
`-std=...` are accepted and ignored (noted with `-v`).

## Directory Structure

```
zero_cc/
├─ include/                # Embeddable public headers
│   ├─ zero_cc.h           # Public façade
│   ├─ zero_cc_fwd.h       # Forward declarations
│   └─ stdatomic.h / stdnoreturn.h / threads.h   # C11 reference headers
├─ src/
│   ├─ main.c              # Driver: CLI, multi-TU, AOT/JIT, linker dispatch
│   └─ zero_cc/
│       ├─ common/         # Arena allocator, hash tables, POSIX shims
│       ├─ core/           # Compiler state, preprocessor, pragma state
│       ├─ lexer/          # Tokenizer (C99/C11/C23 keyword tables)
│       ├─ parser/         # Arena-based AST parser, const eval, static assert
│       ├─ semantic/       # Semantic analysis: types, scopes, diagnostics
│       ├─ ir/             # 133-opcode Instr IR, AST→IR lowering,
│       │                   # linear-scan regalloc, asm/machine emitters
│       ├─ opt/            # IR passes: constant_fold, dce, copy_prop,
│       │                   # mem2reg, cse, gvn, licm, branchless,
│       │                   # strength_reduction, auto_vectorize, CFG layer
│       ├─ isa/            # Backend vtable + x86_64, aarch64, riscv (64/32)
│       ├─ jit/            # JIT image build/merge, ELF emission, TLS backend
│       ├─ linker/         # ZeroCC's own ELF64 + ar archive linker (T2)
│       └─ types/          # Type system, symbols, virtual registers
├─ zero_rt/                # Freestanding runtime (heap, strings, stdio)
│                           # compiled by zerocc itself for x86-64/aarch64/riscv64
├─ third_party/vendor/     # Vendored deps (Ryū, gcc.c-torture corpus)
├─ test/                   # Regression suites (unit, semantic, abi, jit, aot,
│                           # driver, linker, licm, inline_audit, isa_qemu,
│                           # torture, diag, bench, ...)
├─ tools/                  # Verification & tooling (see tools/README.md)
├─ docs/                   # OKF knowledge graph (docs/okf/), archive, plan
├─ LICENSE                 # MIT license
└─ Makefile                # Build system
```

## Architecture

### Compilation Pipeline

ZeroCC uses a **two-pass architecture** with a flat IR as the pivot:

```
Source → Preprocessor → Lexer → Arena AST Parser → Semantic Analysis
       → AST→IR Lowering → IR Optimization (fixed-point, up to 10 rounds)
       → Linear-Scan Register Allocation → Pass 2 (JIT machine code / AOT .s / .o)
```

**IR design:**
- 133-opcode `Instr` array (integer, float, SIMD, atomic, TLS, and call
  opcodes) with an `IRCallArgs` side table for call-site operands
- Dynamic, growing `IRArena` buffer — O(1) emission, cheap reset per function
- Flat, non-SSA with implicit control flow; a lazy versioned **CFG layer**
  (`opt/cfg.{h,c}`) provides basic blocks, reachability, and natural loops for
  LICM / mem2reg / the vectorizer
- Call sites use the target ABI via the backend vtable (including hidden
  `sret` pointer returns for `>16B` structs)

### Virtual Register (VREG) System

ZeroCC uses an **ISA-independent virtual register system** so one lowering
serves all targets:

- **Virtual registers** `VREG_0`–`VREG_15` (GP) and `VREG_F0`–`VREG_F15`
  (float) are used in lowering; defined in [`src/zero_cc/types/vregs.h`](src/zero_cc/types/vregs.h)
- Each backend maps VREGs to physical registers (x86-64, AArch64, RISC-V 64/32)
- Frame pointer / stack pointer are reserved per backend (`BE(c)->fp_reg`,
  `BE(c)->sp_reg`)

See [`docs/okf/ir-and-regalloc.md`](docs/okf/ir-and-regalloc.md) for the
full register-mapping and allocation details.

### Backend Architecture

Code generation is dispatched through a **`ZeroCCISABackend` vtable**
(`src/zero_cc/isa/isa_backend.h`): one lowering and one optimizer pipeline,
four implementations — `x86_64.c`, `aarch64.c`, `riscv.c` (64- and 32-bit) —
plus cross-cutting backends for feature families:

- `jit/tls_backend.c` — `_Thread_local` (`.tdata`/`.tbss`, x86-64
  initial-exec idiom, JIT data-section fallback)
- per-ISA asm emitters (`*_asm.c`) for AOT `.s`/`.o` output
- `linker/` — ZeroCC's own ELF64-LE object reader and static linker (T2)

**Pattern**: each feature gets a backend interface + per-ISA implementation,
so new ISAs or features plug in without touching lowering.

## Optimization Levels

The optimizer runs the pass pipeline to a fixed point (up to 10 rounds),
stopping when a round makes no change (`src/zero_cc/opt/opt_pass.c`):

| Level | Active passes (in order) |
|-------|--------------------------|
| **-O0** | None (debug builds; also the `test-opt` reference baseline) |
| **-O1** (default) | `constant_fold`, `flag_dce`, `dce`, `copy_prop`, `mem2reg` |
| **-O2** | + `cse`, `gvn`, `licm`; small static/`inline` function inlining |
| **-O3** | + `branchless`, `strength_reduction`, `branch_simplify`, `auto_vectorize` (SSE2/SSE4.1, NEON, RVV) |

Inlining uses conservative candidate filters (no FP signatures, no body
locals, no labels/gotos, no `&param`, no value-param writes — see
`test/inline_audit/`). Per-pass timing is available via `--profile-opts`
(measurements in [`docs/okf/perf-measurements.md`](docs/okf/perf-measurements.md)).

## Cross-Compilation, Own Linker & zero_rt

### Cross-compiling

```bash
build/bin/zerocc prog.c --target aarch64 -o prog.aarch64
build/bin/zerocc prog.c --target riscv64 -c -o prog.rv64.o
build/bin/zerocc prog.c --target riscv32 -o prog.rv32
```

### Own linker (T2)

`--zerocc-link` links the per-TU objects with **ZeroCC's own linker** instead
of the system linker (x86-64, AArch64, RISC-V 64-bit). The result is a
directly-executable **static `ET_EXEC`** — no libc, no crt files — and by
default it auto-links the matching `zero_rt` runtime archive:

```bash
build/bin/zerocc a.c b.c --zerocc-link -o app      # freestanding executable
build/bin/zerocc a.c lib.a --zerocc-link -o app    # archive member extraction
build/bin/zerocc a.c --zerocc-link --no-zerocc-rt -o app  # no runtime
```

### zero_rt

`zero_rt/` is ZeroCC's own freestanding runtime — `brk`-backed heap, string
builtins, `atexit`/`exit` with `zeroc_main_wrapper`, and an unbuffered stdio
layer with vendored Ryū float formatting — **compiled by zerocc itself** for
x86-64, AArch64, and RISC-V 64-bit (`make all` builds all three archives).
It is the first customer of the own linker; see
[`docs/okf/zero-rt.md`](docs/okf/zero-rt.md).

Gates: `make test-linker` (own linker, gcc-differential) and
`make test-isa-qemu` (raw binary emitter for all 4 ISAs under QEMU, diffed
against a static-musl reference). See
[`docs/okf/own-linker.md`](docs/okf/own-linker.md).

## Test Results

Current status (verified against `make test` and friends):

| Suite | Status | Count |
|-------|--------|-------|
| **Main regression suite** (`make test`: unit, semantic, ABI, JIT, AOT, LICM) | ✅ Pass | 156/156 |
| **Optimizer sweep** (`make test-opt`, `-O0..-O3` × JIT + AOT via zerocc) | ✅ Pass | 117/117 |
| **Multi-file driver** (`make test-driver`, multi-TU JIT/AOT + per-input `-c`/`-S`) | ✅ Pass | 37/37 |
| **Own linker** (`make test-linker`, `--zerocc-link` + gcc differential) | ✅ Pass | 21/21 |
| **Cross-ISA raw-emitter QEMU gate** (`make test-isa-qemu`, vs static-musl reference) | ✅ Pass | 44/44 (11/11 × x86-64, aarch64, riscv64, riscv32) |
| **GCC c-torture conformance gate** (`make test-torture`, ratcheted baseline) | ✅ Pass | 1,714 PASS / 3,940 corpus |
| **`sret` ABI verifier** (`tools/sret_verifier.py --verify-all`) | ✅ Pass | 6/6 |
| **C11 verification suite** (`tools/c11/test_c11.py`) | ✅ Pass | 11/11 |
| **VLA verification suite** (`tools/vla/test_vla.py`) | ✅ Pass | 11/11 |
| **Diagnostics battery** (`tools/diag_battery.py`, vs gcc) | ✅ Pass | 16 cases, 8/8 error categories |
| **C style checker** (`tools/style_check.py`) | ✅ Pass | 141 files, 0 errors |

## Compiler Diagnostics

Diagnostics aim for gcc/clang-style quality:

- **Format**: `file:line:col: error: message` with source carets; macro-expansion `note:` chains
- **C99 constraint checking**: call argument count vs. prototype (6.7.5.3, variadic-aware), `goto` target must be a defined label (6.8.4.3), struct/union member must exist (6.5.2.1), block closing `}` required (6.8) — no-prototype `f()` calls stay unconstrained per the standard
- **"Did you mean" suggestions** for undeclared identifiers and unknown struct members (Levenshtein ≤ 2, unique-best only)
- **Multi-error recovery** (clang-style error budget of ~20): one pass reports all faults in a broken file; further diagnostics are capped with a "too many errors emitted" note — gated by `tools/diag_battery.py`

## C11 Support

| Feature | Status | Notes |
|---------|--------|-------|
| **`_Thread_local`** | ✅ Complete | `tls_backend`; static TLS (`.tdata`/`.tbss`, x86-64 initial-exec + `R_X86_64_TPOFF32`; JIT data-section fallback) — see [`docs/okf/tls-static.md`](docs/okf/tls-static.md) |
| **`_Atomic`** | ✅ Implemented | Hardware atomic IR ops (`OP_ATOMIC_*`), `__sync_*`/`__atomic_*` builtins, `<stdatomic.h>` virtual header |
| **`_Alignas` / `_Alignof`** | ✅ Implemented | Compile-time alignment for struct fields and locals |
| **`_Noreturn`** | ✅ Implemented | `<stdnoreturn.h>` virtual header |
| **`_Static_assert`** | ✅ Implemented | Constant-evaluated at compile time |
| **`_Generic`** | ✅ Simplified | Full C11 matching rules not implemented |
| **`inline` semantics** | ✅ Implemented | Inlining at `-O2`/`-O3` with conservative candidate filters |
| **`<threads.h>`** | ✅ Virtual header | `thrd_*`/`mtx_*` aliases over the pthread surface |
| **Complex numbers** | 📋 Planned | — |

### Preprocessor Pragmas

| Pragma | Status | Notes |
|--------|--------|-------|
| **`#pragma pack(n)`** | ✅ Complete | Structure packing control with push/pop |
| **`#pragma message("text")** | ✅ Complete | Compiler messages |
| **`#pragma GCC diagnostic push/pop`** | ✅ Complete | Diagnostic state stack |
| **`#pragma GCC diagnostic error/warning/ignored`** | ✅ Complete | Warning control |
| **`#pragma GCC push_options` / `pop_options`** | ✅ Complete | Options stack |

## Building

```bash
make           # Build zerocc, zero_rt.a (all ISAs), and all tests
make test      # Run the full regression suite (156 tests)
make test-opt  # Optimizer sweep at -O0..-O3, JIT + AOT, via zerocc itself
make test-driver   # Multi-TU driver tests (tools/driver_tests.py)
make test-linker   # Own-linker differential tests (tools/linker_tests.py)
make test-isa-qemu # Raw-emitter QEMU gate for all 4 ISAs (needs qemu-user-static + tools/setup_musl_cross.sh)
make test-torture  # GCC c-torture conformance gate (tools/torture_run.py)
make asan / make test-asan   # ASan+UBSan build / run
make clean     # Clean build artifacts
```

**Embedding** the library in your own program (compile the `src/zero_cc/` sources with your own `main`; there is no prebuilt compiler archive):

```bash
gcc -Wall -Isrc -Iinclude -fcommon your_main.c $(find src/zero_cc -name '*.c') -o your_app
musl-gcc -Wall -Isrc -Iinclude -fcommon your_main.c $(find src/zero_cc -name '*.c') -static  # glibc-free host
```

## Documentation & Knowledge Graph (`docs/okf/`)

ZeroCC maintains an **Open Knowledge Format (OKF v0.2)** knowledge graph in
[`docs/okf/`](docs/okf/index.md); historical design notes are preserved in
[`docs/archive/`](docs/archive/):

| Document | Description |
|----------|-------------|
| [`docs/okf/index.md`](docs/okf/index.md) | Root knowledge-graph navigator |
| [`docs/okf/architecture-pipeline.md`](docs/okf/architecture-pipeline.md) | Compilation pipeline & source file map |
| [`docs/okf/frontend-and-types.md`](docs/okf/frontend-and-types.md) | Type system, structs, VLAs, C11 features, virtual headers |
| [`docs/okf/multi-tu.md`](docs/okf/multi-tu.md) | Multi-translation-unit driver & JIT image merge (T1) |
| [`docs/okf/own-linker.md`](docs/okf/own-linker.md) | ZeroCC's own ELF linker & relocation set (T2) |
| [`docs/okf/zero-rt.md`](docs/okf/zero-rt.md) | `zero_rt` freestanding runtime |
| [`docs/okf/ir-and-regalloc.md`](docs/okf/ir-and-regalloc.md) | 133-opcode `Instr` IR & linear-scan register allocation |
| [`docs/okf/optimization-passes.md`](docs/okf/optimization-passes.md) | Active IR optimization passes & safety invariants |
| [`docs/okf/abi-sysv-x86_64.md`](docs/okf/abi-sysv-x86_64.md) | System V AMD64 ABI & `>16B` struct return (`sret`) rules |
| [`docs/okf/isa-backends.md`](docs/okf/isa-backends.md) | Multi-ISA backend vtable, JIT vs AOT emission, relocations |
| [`docs/okf/global-data-relocs.md`](docs/okf/global-data-relocs.md) | Data section, global initializers & pointer relocations |
| [`docs/okf/tls-static.md`](docs/okf/tls-static.md) | Static TLS (`_Thread_local`) support |
| [`docs/okf/scalar-float-x86.md`](docs/okf/scalar-float-x86.md) | Scalar float pipeline (x86-64) |
| [`docs/okf/c-torture-gate.md`](docs/okf/c-torture-gate.md) | GCC `c-torture` differential conformance gate |
| [`docs/okf/perf-measurements.md`](docs/okf/perf-measurements.md) | Measured optimizer costs & JIT-vs-GCC data |
| [`docs/CHANGELOG.md`](docs/CHANGELOG.md) | Full release notes and changelog |
| [`docs/ZEROCC_Development_Plan.md`](docs/ZEROCC_Development_Plan.md) | Project development roadmap |
| [`docs/coding_style.md`](docs/coding_style.md) | C style & convention rules |
| [`tools/README.md`](tools/README.md) | Verification & tooling suite documentation |

## Verification & Tooling Suite

ZeroCC includes automated verification suites in [`tools/`](tools/)
(historical debugging scripts are archived in [`tools/archive/`](tools/archive/)):

```bash
python3 tools/c11/test_c11.py --verbose    # C11 feature & concurrency suite
python3 tools/vla/test_vla.py              # VLA & matrix VLA suite
python3 tools/sret_verifier.py --verify-all  # sret ABI verifier
python3 tools/diag_battery.py              # Differential diagnostics vs gcc (ratchet baseline)
python3 tools/style_check.py               # C style/convention checker
python3 tools/simd_regression_suite.py     # Auto-vectorization regression suite
python3 tools/cross_isa_comparator.py test/aot/hello_world.c  # Multi-ISA asm comparison
python3 tools/isa_qemu_diff.py --isa riscv64   # Raw-emitter QEMU differential gate
python3 tools/okf.py lint && python3 tools/okf.py index  # Validate the OKF knowledge graph
```

See [`tools/README.md`](tools/README.md) for the full tool table.

## License

ZeroCC is released under the MIT License. See the `LICENSE` file for details.

## Self-Hosting

ZeroCC is designed to eventually **self-host** (compile itself). Current
status:

- **Phase 1 — compile simple C programs**: ✅ done
- **Phase 2 — freestanding runtime & self-hosted linking**: ✅ done —
  `zero_rt` is compiled **by zerocc itself** for x86-64, AArch64, and RISC-V
  64-bit, and `--zerocc-link` produces static executables with no host libc
  (see [`docs/okf/zero-rt.md`](docs/okf/zero-rt.md)). zerocc compiles its own
  individual TUs (static-locals, string-pointer arrays, and other
  self-hosting blockers were closed in Sprint 41)
- **Phase 3 — full self-compilation bootstrap**: in progress — next
  milestones are the Windows (PE/COFF + Win64 ABI) and macOS (Mach-O)
  linker/runtime phases in the development plan

## Acknowledgements
The `gcc.c-torture`
conformance corpus is vendored from `gcc-mirror`, and float formatting in
`zero_rt` uses the vendored Ryū algorithm.
