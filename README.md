# zero_cc — Standalone Embeddable C99/C11/C23 Compiler

ZeroCC is a lightweight, embeddable C99/C11/C23 compiler (with GNU extensions) with a two-pass architecture: a flat 168-opcode IR that is lowered, optimized, register-allocated, and emitted to any of four target ISAs. It compiles to machine code for a JIT (`--run`, the default) or to assembly, ELF objects, and — via ZeroCC's own linker — directly-executable freestanding binaries linked against its own `zero_rt` runtime, with no host libc involved.

## Features

- **C99/C11/C23** compiler, freestanding, with optional `zero_rt` standard library
- **C23 Conformance**: `constexpr` data declarations, `nullptr`, `true`/`false` keywords, `typeof`/`typeof_unqual`, `#warning` (C23 6.9.1), empty `{}` initializers, `[[attr]]` attributes, designator-based unsized array sizing (`make test-c23` gate vs gcc-15)
- **Multi-project validation**: sqlite3.c amalgamation (255k lines), 15-TU zlib, and cJSON compiled under zerocc at `-O0` and `-O2` (`make test-project-diff`)
- **`__zerocc_syscall(number, a, b, c, d, e, f)`** and **`__zerocc_clone(...)`** — direct OS-syscall and thread-clone builtins (`OP_SYSCALL`, `OP_CLONE`) across all 4 ISAs (the `zero_rt` OS boundary)
- **Multi-ISA**: x86-64, AArch64, RISC-V 64, RISC-V 32 (`--target`)
- **Inline assembly (Phases 1–3 complete)**: GNU extended asm (`__asm__ [volatile] [goto] ("..." : outs : ins : clobbers [: labels])`) with `r`/`m`/`=r`/`+r`/fixed-register (`a`/`b`/`d`/`S`/`D`) operands, matching-digit constraints, symbolic `%[name]` / `%l[name]` operands, AT&T/Intel `{att | intel}` dialect alternatives, and `memory`/`cc`/GPR clobbers — lowered to `OP_ASM`, register-allocated (operands `no_spill`, clobbered registers forbidden for operands, live values across clobbers spilled), and supported across AOT `.s` text emission, in-memory JIT, and raw binary emission via the integrated assembler (`make test-asm` 72/72 runs vs gcc; [`docs/okf/inline-assembly-roadmap.md`](docs/okf/inline-assembly-roadmap.md))
- **`-fsanitize=undefined`** — target-side signed-integer-overflow trap (bare
  `ud2`; x86-64, both emitters; check width follows the usual arithmetic
  conversions — Sprints 116–117; `make test-san` flag-ON gate)
- **`-fstack-protector` / `-fstack-protector-strong` / `-fstack-protector-all`** — GCC/Clang-compatible stack canary guards across all 4 ISAs (`%fs:40` on x86-64, `__stack_chk_guard` on AArch64/RISC-V; Sprint 147)
- **Multi-translation-unit driver**: multiple `.c` inputs per invocation
  (JIT image merge + per-input `-c`/`-S`)
- **Own ELF64/ELF32 linker & `ar` archiver** (`--zerocc-link`, `--ar`): static `ET_EXEC` output across all 4 ISAs (`x86-64`, `aarch64`, `riscv64`, `riscv32`) with no host libc or crt files, native GNU `ar` archive reader/writer, plus `zero_rt` freestanding runtime auto-linking
- **Freestanding C11 `<threads.h>` & POSIX `pthread_*` in `zero_rt`** (Sprint 146): lightweight zero-dependency multithreading (`thrd_create`, `mtx_*`, `cnd_*`, `call_once`, `pthread_create`/`join`/`mutex_*`/`cond_*`) built on `__zerocc_clone` + Ulrich Drepper 3-state futex mutexes across all 4 ISAs
- Preprocessor (`#include`, `#define`, `#if/#ifdef`, `#warning`, `#error`, virtual C11/C23 headers
  `<stdatomic.h>` / `<stdnoreturn.h>` / `<threads.h>` / `<pthread.h>`, `-E` preprocess-only)
- Type system (structs/unions, bitfields, pointers, arrays, VLAs, enums,
  `_Atomic`, `_Generic`, designated/aggregate initializers)
- Control flow (`if/else`, `switch`, loops, `goto`, `break/continue/return`)
- GNU statement expressions (`({ ... })`)
- System V AMD64 / AArch64 / RISC-V calling conventions: by-value structs
  with full System V classification on x86-64 (≤8B in one register, 9–16B
  per-8-byte-unit SSE/GPR class merge, >16B as caller-stack MEMORY
  arguments, `sret` hidden-pointer returns for >16B struct *results*),
  bidirectionally verified against gcc-15 objects (`make test-abi-interop`,
  Sprint 158)
- `__int128` / `__uint128_t` support on all 64-bit ISAs (Sprint 109)
- 128-bit ABI completeness: two-unit `__int128` argument ABI, `__int128`
  function-pointer signatures, `__float128` (Sprints 110–112)
- `__builtin_{add,sub,mul}_overflow` real overflow detection (Sprint 61)
- `__attribute__((section("name")))` on objects and functions (Sprints 107–108)
- `__attribute__((aligned(N)))` with runtime SP realign for `N > 16` locals (Sprints 94–96)
- `__builtin_apply` / `__builtin_apply_args` (Sprint 103)
- GNU asm labels `register void *ptr asm ("name")` (Sprint 102)
- C89 implicit-int and gnu89 tail leniency (Sprint 101)
- GNU variably-modified struct/union members (Sprint 100)
- GNU nested functions / trampolines with closure capture (Sprints 90–93)
- `__builtin_return_address` / `__builtin_frame_address` (Sprint 86)
- Computed goto (`goto *ptr`) and labels as values (`&&label`) (Sprint 84)
- Inline `OP_SETJMP` / `OP_LONGJMP` (Sprint 88)
- **Self-contained static-musl host binary & JIT runtime**: `build/bin/zerocc` is fully statically linked against in-tree vendored musl (`--whole-archive`) with zero host glibc dependency (`ldd`: `"not a dynamic executable"`), executing JIT code via `mmap`, a far-call detour trampoline pool, and self-hosting `dlfcn` (`zero_rt/dlfcn/dlfcn.c` resolving against `/proc/self/exe` `.symtab`; see [`docs/okf/host-runtime-and-portability.md`](docs/okf/host-runtime-and-portability.md))
- **Two-pass code generation**: IR → optimization → linear-scan register
  allocation → machine code
- **Optimization levels** `-O0`…`-O3` (constant folding, DCE, copy
  propagation, mem2reg, CSE, GVN, LICM, branchless, strength reduction,
  branch simplification, full loop unrolling + partial loop unrolling
  (factor 2/4) with remainder peeling for runtime-bound loops,
  auto-vectorization to SSE2/SSE4.1, NEON, and RVV; small-function inlining at
  `-O2`/`-O3`)
- **Compiler diagnostics** — gcc-style caret errors, C99 constraint checking,
  "did you mean" suggestions, multi-error recovery
- **Conformance gate** ratcheted against GCC's `gcc.c-torture` corpus
  (3,940 tests; 3,781 PASS / 0 bad at HEAD; see `make test-torture`)

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
  -std=<dialect>  Language standard: c99, gnu99, c11, gnu11 (default),
                  c17, gnu17, c23, gnu23 (drives keywords & __STDC_VERSION__)
  -fomit-frame-pointer    Omit frame pointers in fixed-frame functions
                          (default at -O1+)
  -fno-omit-frame-pointer Keep frame pointers in every non-leaf function
  -fstack-protector[-strong|-all]  Enable stack-smashing canary protection
  -I<dir>  -isystem <dir>  Add include / system include directory
  -D<macro>  -U<macro>     Define / undefine a macro
  -include <file>          Force-include a file before the main source
  -run, --run     Run in JIT mode (default)
  --target <arch> Target architecture (x86-64, aarch64, riscv64, riscv32)
  -fsanitize=undefined  Target-side signed-integer-overflow trap after signed
                       +,-,* (x86-64, both emitters; Sprints 116-117)
  --zerocc-link  With -o: link with ZeroCC's own linker (x86-64, aarch64,
                riscv64, riscv32); auto-links zero_rt (opt out: --no-zerocc-rt)
  --ar <cmd> <lib.a> [objs...]  Built-in GNU ar archiver (rcs, t, x)
  -MMD / -MF <file> / -MT <target>   Dependency-file generation
  -dump-ir      Dump IR before optimization
  -dump-cfg     Dump control-flow graph
  --profile-opts  Print per-pass optimization timings
  -v, --verbose   Verbose output
  --version     Print version
  -h, --help      Show help
```

Common gcc flags such as `-g`, `-Wall`, `-fPIC`, `-pthread`, and `-static` are
accepted for build-system compatibility (noted with `-v`).

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
│       ├─ driver/         # Driver helpers extracted from main.c: CLI option
│       │                   # parsing (driver_opts.h) & AOT asm emission (driver_asm.h)
│       ├─ lexer/          # Tokenizer (C99/C11/C23 keyword tables)
│       ├─ parser/         # Arena-based AST parser, const eval, static assert
│       ├─ semantic/       # Semantic analysis: types, scopes, diagnostics
│       ├─ ir/             # 172-opcode Instr IR, AST→IR lowering,
│       │                   # linear-scan regalloc, asm/machine emitters
│       ├─ opt/            # IR passes: constant_fold, dce, copy_prop,
│       │                   # mem2reg, cse, gvn, licm, branchless,
│       │                   # strength_reduction, branch_simplify,
│       │                   # loop_unroll, auto_vectorize, CFG + dominators
│       ├─ isa/            # Backend vtable + x86_64, aarch64, riscv (64/32)
│       ├─ jit/            # JIT image build/merge, ELF emission, TLS backend
│       ├─ linker/         # ZeroCC's own ELF64/ELF32 static linker & ar writer (T2)
│       └─ types/          # Type system, symbols, virtual registers
├─ zero_rt/                # Freestanding runtime (heap, strings, stdio, C11/POSIX
│                           # threads, int128, complex __divdc3) compiled by zerocc
│                           # for all 4 ISAs + host self-hosting dlfcn/
├─ third_party/vendor/     # Vendored deps (musl-1.2.6, Ryū, gcc.c-torture corpus)
├─ test/                   # Regression suites (unit, semantic, abi, jit, aot,
│                           # driver, linker, licm, inline_audit, isa_qemu,
│                           # isa128, torture, diag, san, abi_interop, bench, ...)
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
- 172-opcode `Instr` array (integer, float, SIMD, atomic, TLS, and call
  opcodes); call-site and inline-asm operands travel **in the instruction
  stream** as `OP_CALL_ARG` / `OP_ASM_ARG` / `OP_ASM_LABEL` pseudo-
  instructions (the former `IRCallArgs` / `IRAsmArgs` side tables were
  removed, Sprints 151c & 154)
- Dynamic, growing `IRArena` buffer — O(1) emission, cheap reset per function
- Flat, non-SSA with implicit control flow; a lazy versioned **CFG layer**
  (`opt/cfg.{h,c}`) provides basic blocks, reachability, dominators/post-dominators,
  and natural loops for LICM / mem2reg / loop unrolling / the vectorizer
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
- `linker/` — ZeroCC's own ELF64/ELF32 object reader, static linker, and GNU `ar` archive reader/writer (T2)

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
| **-O3** | + `branchless`, `strength_reduction`, `branch_simplify`, `loop_unroll` (constant-trip `1..8` full unroll + runtime-bound partial unroll factor 2/4 with remainder peel), `auto_vectorize` (SSE2/SSE4.1, NEON, RVV) |

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

The riscv64/aarch64 `-o` system-linker paths use the installed cross gcc
(`riscv64-linux-gnu-gcc` / `aarch64-linux-gnu-gcc`, `-static`). The riscv32
path needs an explicit static-musl prefix (the Debian rv64 cross gcc ships no
rv32 musl); the driver discovers it as `$ZC_RV32_MUSL` or, by default, the
in-tree committed `third_party/vendor/musl-cross/riscv32` (Sprint 157 — no
setup needed on any machine; rebuild in place with
`tools/setup_musl_cross.sh --arch riscv32 ...`).

### Own linker & archiver (T2)

`--zerocc-link` links the per-TU objects with **ZeroCC's own linker** instead
of the system linker across all 4 ISAs (x86-64, AArch64, RISC-V 64-bit, and
RISC-V 32-bit). The result is a directly-executable **static `ET_EXEC`**
(ELF64 or ELF32) — no libc, no crt files — and by default it auto-links the
matching `zero_rt` runtime archive:

```bash
build/bin/zerocc a.c b.c --zerocc-link -o app      # freestanding executable
build/bin/zerocc a.c lib.a --zerocc-link -o app    # archive member extraction
build/bin/zerocc a.c --zerocc-link --no-zerocc-rt -o app  # no runtime
build/bin/zerocc --ar rcs libfoo.a a.o b.o         # built-in GNU ar archiver
```

### zero_rt

`zero_rt/` is ZeroCC's own freestanding runtime — thread-safe `brk`-backed
heap, string builtins, `atexit`/`exit` with `zeroc_main_wrapper` (and stack
canary `__stack_chk_guard`/`__stack_chk_fail`), C11 `<threads.h>` + POSIX
`pthread_*` multithreading (`__zerocc_clone` + Ulrich Drepper 3-state futex
mutexes), 128-bit integer math, clean-room complex division (`__divdc3`), and
an unbuffered stdio layer with vendored Ryū float formatting — **compiled by
zerocc itself** for x86-64, AArch64, RISC-V 64-bit, and RISC-V 32-bit
(`make all` builds all four archives using `zerocc --ar`). Plus `zero_rt/dlfcn/`
supplies the host-side self-hosting `dlfcn` for the static-musl compiler binary.
See [`docs/okf/zero-rt.md`](docs/okf/zero-rt.md) and
[`docs/okf/host-runtime-and-portability.md`](docs/okf/host-runtime-and-portability.md).

Gates: `make test-linker` (own linker & archiver across all 4 ISAs, gcc-differential)
and `make test-isa-qemu` (raw binary emitter for all 4 ISAs under QEMU, diffed
against a static-musl reference). See
[`docs/okf/own-linker.md`](docs/okf/own-linker.md).

## Test Results

Current status (verified 2026-11-11 at HEAD, Sprints 129–158):

| Suite | Status | Count |
|-------|--------|-------|
| **Main regression suite** (`make test`: unit, semantic, ABI, JIT, AOT, asm reference) | ✅ Pass | 226/226 |
| **External ABI interop gate** (`make test-abi-interop`: bidirectional `zerocc.o ↔ gcc.o` — zerocc main + gcc main, 15 calling-convention scenarios at `-O0`/`-O1`/`-O2`, incl. MEMORY struct args, SSE/SSEup 16B structs, `sret`, variadic struct args) | ✅ Pass | 15/15 |
| **Optimizer sweep** (`make test-opt`, `-O0..-O3` × JIT + AOT via zerocc) | ✅ Pass | 183 tests: 125 ok, 0 failed (58 embedding tests skipped by design — covered by `make test`; 1 pre-existing nondeterministic) |
| **C23 differential gate** (`make test-c23`: 24 cases across gnu23/c11/c99 vs gcc-15) | ✅ Pass | 24/24 |
| **Inline-asm Phases 1–3 gate** (`make test-asm`: `test/asm/*.c` across `-S` text, JIT, and raw AOT at `-O0..-O3` vs host-gcc reference) | ✅ Pass | 72/72 |
| **Project-differential gate** (`make test-project-diff`: cJSON, zlib, sqlite3.c at -O0/-O2) | ✅ Pass | 36/36 TUs |
| **`zero_san` flag-ON sweep** (`make test-san`: `san_ok_*` clean + `san_trap_*` SIGILL at `-O0..-O3`, JIT + AOT, cross-ISA qemu smoke) | ✅ Pass | 101 ok + 3 cross |
| **Multi-file driver** (`make test-driver`, multi-TU JIT/AOT + per-input `-c`/`-S`) | ✅ Pass | 37/37 |
| **Own linker & archiver** (`make test-linker`, `--zerocc-link` + `--ar` across all 4 ISAs + gcc differential) | ✅ Pass | 45/45 |
| **Cross-ISA raw-emitter QEMU gate** (`make test-isa-qemu`, vs static-musl reference) | ✅ Pass | 59/59 (all 4 ISAs) |
| **Cross-ISA `__int128` raw-emitter QEMU gate** (`make test-isa128`, vs static-musl reference) | ✅ Pass | 144/144 (all 3 ISAs × 6 tests × 4 levels × 2 paths) |
| **GCC c-torture conformance gate** (`make test-torture`, ratcheted baseline, gcc-15 reference) | ✅ Pass | 3,781 PASS / 3,940 corpus (0 MISCOMPILE / 0 REJECT; SKIP-SUPPORT 87, SKIP-C89 33, SKIP-REFSEM 7, SKIP-TARGET: arm 2, ia64 1, mips 4, powerpc 2, s390x 1, windows 1, float16/bfloat16/float128x runtime 3, SKIP-UB/PTR/BOGUS 3, EXPECTED-REJECT 4, NO-REF 1, REF-BUG 11) |
| **Type-tag verifier** (`tools/type_tag_verifier.py --verify-all`, invariants I1–I10 at `-O0/2/3`) | ✅ Pass | 160 files, RESULT OK |
| **Loop unroll verifier** (`tools/loop_unroll_verifier.py`, `-O3` constant-trip full unrolling + runtime-bound partial unrolling w/ remainder peel, `expect-unroll`/`expect-partial` markers) | ✅ Pass | 25/25 |
| **ASan/UBSan rebuild** (`make asan`) | ✅ Pass | 223/223 |
| **`sret` ABI verifier** (`tools/sret_verifier.py --verify-all`) | ✅ Pass | 6/6 |
| **C11 verification suite** (`tools/c11/test_c11.py`) | ✅ Pass | 11/11 |
| **VLA verification suite** (`tools/vla/test_vla.py`) | ✅ Pass | 11/11 (incl. VLA-in-loop / VLA-in-block runtime tests — C99 6.7.5.2 legal, gcc-differentially verified) |
| **Diagnostics battery** (`tools/diag_battery.py`, vs gcc) | ✅ Pass | 16 cases, 8/8 error categories |
| **C style checker** (`tools/style_check.py`) | ✅ Pass | 0 errors |

## C23 Support

ZeroCC implements the following C23 features:

| Feature | Status | Notes |
|---------|--------|-------|
| `nullptr` / `nullptr_t` | ✅ Complete | C23 6.4.4.6 / 6.7.2; keyword and `nullptr_t` predefined type |
| `true` / `false` | ✅ Complete | C23 6.4.4.5; keywords in C23 dialect without `<stdbool.h>` |
| `constexpr` data declarations | ✅ Complete | C23 6.7.2.4; initialized data objects (scalars, arrays, structs, pointers) |
| `typeof` / `typeof_unqual` | ✅ Complete | C23 6.7.2.5; also GNU `__typeof__` / `__typeof` and dependent param types |
| `#warning` directive | ✅ Complete | C23 6.9.1; raw diagnostic text, splicing, `#pragma GCC diagnostic` control |
| `#error` directive | ✅ Complete | Standard C preprocessor diagnostic |
| Unsized-array designator sizing | ✅ Complete | C99 6.7.8p12 / C23 6.7.10; `T a[] = { [N] = ... }` sized from max index + 1 |
| Empty `{}` initializers | ✅ Complete | C23 6.7.10 for all storage classes |
| `[[attr]]` attributes | ✅ Complete | Parser recognition via `skip_c23_attributes()` |
| `__STDC_NO_COMPLEX__` | ❌ Not defined | `_Complex float/double` and GNU integer complex are supported; the macro is intentionally absent |
| `__typeof__` / `__typeof` | ✅ Complete | GNU spellings accepted everywhere `typeof` works |
| **Differential C23 Gate** | ✅ Verified | `make test-c23` (`tools/c23/test_c23.py`) 24/24 cases diffed vs gcc-15 across dialects |

## C11 Support

| Feature | Status | Notes |
|---------|--------|-------|
| **`_Thread_local`** | ✅ Complete | `tls_backend`; static TLS (`.tdata`/`.tbss`, x86-64 initial-exec + `R_X86_64_TPOFF32`; JIT data-section fallback) — see [`docs/okf/tls-static.md`](docs/okf/tls-static.md) |
| **`_Atomic`** | ✅ Implemented | Hardware atomic IR ops (`OP_ATOMIC_*`), `__sync_*`/`__atomic_*` builtins, `<stdatomic.h>` virtual header |
| **`_Alignas` / `_Alignof`** | ✅ Implemented | Struct fields, file-scope objects (any N, `.data`/`.bss` section alignment), and locals (any N on x86-64 via a runtime `and $-N, %rsp` SP realign prologue, GCC-style; VLA/variadic fns and other ISAs fall back to 16 with a warning) |
| **`_Noreturn`** | ✅ Implemented | `<stdnoreturn.h>` virtual header |
| **`_Static_assert`** | ✅ Implemented | Constant-evaluated at compile time |
| **`_Generic`** | ✅ Simplified | Full C11 matching rules not implemented |
| **`inline` semantics** | ✅ Implemented | Inlining at `-O2`/`-O3` with conservative candidate filters |
| **`<threads.h>`** | ✅ Complete | Virtual header + freestanding `zero_rt` runtime (`thrd_*`/`mtx_*`/`cnd_*`/`call_once` + POSIX `pthread_*` over `OP_CLONE` & futexes on all 4 ISAs; Sprint 146) |
| **`_Complex` (C99 + GNU)** | ✅ Implemented | `_Complex float`/`double` (C99 6.2.5) and GNU integer `_Complex`/`__complex__` (`int`/`long`/`short`/`char`): `+ - * /` (clean-room `__divdc3` in `zero_rt`), `==`/`!=`, `~` conjugate, `__real__`/`__imag__`; FP passed in `xmm0/xmm1`, integer ≤8B in 1 GPR / 16B in 2 GPRs — [`docs/okf/frontend-and-types.md`](docs/okf/frontend-and-types.md) |

### Preprocessor Pragmas

| Pragma | Status | Notes |
|--------|--------|-------|
| **`#pragma pack(n)`** | ✅ Complete | Structure packing control with push/pop |
| **`#pragma message("text")`** | ✅ Complete | Compiler messages |
| **`#pragma GCC diagnostic push/pop`** | ✅ Complete | Diagnostic state stack |
| **`#pragma GCC diagnostic error/warning/ignored`** | ✅ Complete | Warning control |
| **`#pragma GCC push_options` / `pop_options`** | ✅ Complete | Options stack |

## Developer Tooling & Prerequisites

ZeroCC development and verification rely on a combination of host build tools, reference compilers for differential conformance testing, and user-space emulation for cross-ISA verification.

### Tooling Matrix

| Tool | Minimum | Current System | Role | Tier |
|------|---------|----------------|------|------|
| **GNU Make** | 4.0+ | GNU Make 4.3 | Build orchestration for compiler, runtimes, and tests (`make all`, `make test`) | **Core** |
| **Host C Compiler** | GCC 13+ / Clang 14+ | GCC 13.3.0 | Builds the `zerocc` binary (via in-tree static-musl wrapper) and host-side test binaries (`make asan`) | **Core** |
| **Python 3** | 3.8+ | Python 3.12.3 | Test runners, ABI verifiers, OKF knowledge graph (`tools/*.py`) | **Core** |
| **Git** | 2.25+ | Git 2.43.0 | Source tree and revision control | **Core** |
| **GCC 15** | 15.0+ | GCC 15.2.0 (`gcc-15`) | **Mandatory c-torture reference** (`make test-torture` hard-fails without it; each test's `dg-options`/`-std=` dialect passes through), **differential C23 conformance gate** (`make test-c23`) across dialects (`-std=gnu23`, `c23`, `c11`, `c99`, `gnu89`), and the **reference oracle for the x86-64 ISA gates** (`make test-isa128` ref builds + x86_64 `-S` link; the `musl-gcc-x86_64` wrapper wraps it — rebuild with `--ccbin gcc-15`); overridable via `TORTURE_GCC` / `C23_GCC` | **Differential (required)** |
| **GCC 14 / 13** | Any recent | GCC 14.2 / 13.3 | Multi-version baseline probes and diagnostics validation (`tools/diag_battery.py`) | **Differential** |
| **QEMU User Static** | 8.0+ | QEMU 8.2.2 (`qemu-*-static`) | Cross-ISA binary execution for x86-64, AArch64, RISC-V 64, RISC-V 32 under Linux | **Cross-ISA** |
| **Cross Toolchains** | GCC 13+ | `aarch64-linux-gnu-gcc`, `riscv64-linux-gnu-gcc` | Reference cross-compilation & cross-assembling for `make test-isa128` / `test-isa-qemu` (the installed version — no cross `gcc-15` builds exist, and the C99/C11+int128 corpus does not constrain the reference version) | **Cross-ISA** |
| **Static Musl Toolchains** | musl 1.2.6+ | `musl-gcc-<arch>` in `~/.local/bin` | Static reference binary generation for raw emitter differential gate (`make test-isa-qemu`, built via `tools/setup_musl_cross.sh`) | **Cross-ISA** |
| **tmux** | Any | tmux 3.4 | Supervises long-running background gates (`make test-torture`) without shell timeouts | **Recommended** |
| **gdb** | Any modern | GNU gdb 15.1 | Debugging JIT code pages, assembly inspection, and crash triaging (`scripts/gdb_zerocc.py`) | **Optional** |
| **valgrind / perf** | Any | Valgrind 3.22 / Linux perf | Memory safety profiling and optimization pass profiling (`--profile-opts`) | **Optional** |

> **Two separate reference roles** — don't conflate:
> - **GCC 15 (host, x86-64 only)** → drives the **C23 differential gate** (`make test-c23`, 24 feature-tagged cases across dialects) and the **c-torture gate** (`make test-torture`). The ISA gate `test-isa128` also uses host gcc-15 as its **oracle** (it builds an x86-64 static reference to compute expected 128-bit values while the target runs under QEMU).
> - **Cross GCC 13.3 (aarch64-linux-gnu-gcc, riscv64-linux-gnu-gcc)** → drives the **cross-ISA differential gates** (`make test-isa-qemu`, `make test-isa128` `-S` path). The ISA corpora (`test/isa_qemu/`, `test/jit/int128_*.c`) are **C99/C11 + int128/float128 only** — no C23 syntax — so gcc-13.3 is a correct oracle. Cross gcc-15 (Bootlin) exists but is **deferred until the ISA corpora grow C23 test cases** (see [OKF decision note](docs/okf/build-and-verification.md#decision-sprint-142)).

> **Reference tracking policy** — we follow the [GCC C status page](https://gcc.gnu.org/projects/c-status.html) for C standard support. The primary reference compiler tracks the **latest stable GCC with complete support for the highest ratified C standard** (currently GCC 15 for C23). When a new C standard is ratified and GCC ships complete support in a stable release, we migrate the differential gates to that version (ratcheting the c-torture baseline). Experimental/pre-release support (e.g., GCC 16's C2y draft support) is **not** used as the primary reference; it may be added as an optional informational probe (`make test-gcc-trunk`) but never replaces the ratified baseline.

### Environment Setup

```bash
# Core build & Python tooling
sudo apt update && sudo apt install -y build-essential python3 python3-pip git tmux gdb valgrind

# Reference GCC 15 for C23 differential conformance testing
sudo apt install -y gcc-15  # or via ppa:ubuntu-toolchain-r/test if not in default repos

# Cross-compilation toolchains and QEMU emulation
sudo apt install -y qemu-user-static gcc-aarch64-linux-gnu gcc-riscv64-linux-gnu

# Build static musl cross-wrappers for raw-emitter differential verification
tools/setup_musl_cross.sh --arch aarch64 --ccbin aarch64-linux-gnu-gcc
tools/setup_musl_cross.sh --arch riscv64 --ccbin riscv64-linux-gnu-gcc
tools/setup_musl_cross.sh --arch riscv32 --ccbin riscv64-linux-gnu-gcc --extra "-march=rv32gc -mabi=ilp32" --host riscv32-linux-musl
```

## Building

ZeroCC is a single static binary linked against in-tree vendored musl (`third_party/vendor/musl/musl-1.2.6`) with zero host-libc runtime dependencies (`ldd build/bin/zerocc` → `"not a dynamic executable"`).

```bash
# One-command dev environment setup (Ubuntu/Debian/Fedora/Arch/macOS)
./tools/setup_dev_env.sh

# Or manually:
# 1. Host C compiler: GCC >= 13 or Clang >= 14 (set CC= if non-default)
# 2. Reference GCC 15 (mandatory for C23 gate + c-torture)
#    Ubuntu:  sudo apt install gcc-15
#    Fedora:  sudo dnf install gcc15
#    Arch:    sudo pacman -S gcc15
#    macOS:   brew install gcc@15
# 3. Cross toolchains (for ISA gates) - three options:
#    a) System packages (default):
#       Ubuntu:  sudo apt install gcc-aarch64-linux-gnu gcc-riscv64-linux-gnu qemu-user-static
#       Fedora:  sudo dnf install gcc-aarch64-linux-gnu gcc-riscv64-linux-gnu qemu-user-static
#       Arch:    sudo pacman -S aarch64-linux-gnu-gcc riscv64-linux-gnu-gcc qemu-user-static
#    b) Bootlin gcc-15.3 (uniform C23 cross reference):
#       USE_BOOTLIN=1 ./tools/setup_dev_env.sh
#    c) Docker (no root, works everywhere including CI):
#       USE_DOCKER=1 ./tools/setup_dev_env.sh
#       (auto-falls back to Bootlin if running inside a container)
# 4. Build:
make -j$(nproc) build/bin/zerocc

# Quick sanity:
./build/bin/zerocc --run -O0 test/jit/basic.c
```

### Test Gates

```bash
make test              # Full regression suite (190 tests)
make test-opt          # Optimizer sweep -O0..-O3, JIT + AOT (150 tests)
make test-c23          # C23 differential gate vs gcc-15 (24 cases)
make test-asm          # Inline-asm Phases 1–3 gate: test/asm/*.c across text, JIT, and raw AOT (48 runs)
make test-isa-qemu     # Raw-emitter QEMU gate (all 4 ISAs, needs cross toolchains + QEMU)
make test-isa128       # Cross-ISA __int128 gate
make test-torture      # GCC c-torture conformance gate (needs gcc-15)
make asan              # ASan+UBSan build of compiler itself
make test-san          # zero_san flag-ON soundness sweep
```

## Embedding

Embed the library in your own program (compile the `src/zero_cc/` sources with your own `main`; there is no prebuilt compiler archive):

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
| [`docs/okf/frontend-and-types.md`](docs/okf/frontend-and-types.md) | Type system, structs, VLAs, C11/C23 features, virtual headers |
| [`docs/okf/multi-tu.md`](docs/okf/multi-tu.md) | Multi-translation-unit driver & JIT image merge (T1) |
| [`docs/okf/own-linker.md`](docs/okf/own-linker.md) | ZeroCC's own ELF64/ELF32 linker, GNU `ar` archiver & relocation set (T2) |
| [`docs/okf/zero-rt.md`](docs/okf/zero-rt.md) | `zero_rt` freestanding runtime (heap, stdio, C11/POSIX threads, stack canary, complex div) |
| [`docs/okf/host-runtime-and-portability.md`](docs/okf/host-runtime-and-portability.md) | Static-musl host toolchain, self-hosting `dlfcn`, in-process dynamic loader (`dynlink.c`), baremetal & non-Linux portability |
| [`docs/okf/ir-and-regalloc.md`](docs/okf/ir-and-regalloc.md) | 168-opcode `Instr` IR & linear-scan register allocation |
| [`docs/okf/optimization-passes.md`](docs/okf/optimization-passes.md) | Active IR optimization passes & safety invariants |
| [`docs/okf/build-and-verification.md`](docs/okf/build-and-verification.md) | Build targets, direct compiler testing & ABI verifiers |
| [`docs/okf/abi-sysv-x86_64.md`](docs/okf/abi-sysv-x86_64.md) | System V AMD64 ABI: struct argument/return classification (≤8B register, 9–16B SSE/GPR unit merge, >16B MEMORY args + `sret`) |
| [`docs/okf/isa-backends.md`](docs/okf/isa-backends.md) | Multi-ISA backend vtable, JIT vs AOT emission, relocations |
| [`docs/okf/global-data-relocs.md`](docs/okf/global-data-relocs.md) | Data section, global initializers & pointer relocations |
| [`docs/okf/tls-static.md`](docs/okf/tls-static.md) | Static TLS (`_Thread_local`) support |
| [`docs/okf/scalar-float-x86.md`](docs/okf/scalar-float-x86.md) | Scalar float pipeline (x86-64) |
| [`docs/okf/c-torture-gate.md`](docs/okf/c-torture-gate.md) | GCC `c-torture` differential conformance gate |
| [`docs/okf/inline-assembly-roadmap.md`](docs/okf/inline-assembly-roadmap.md) | Clang-style integrated inline assembly roadmap (Phase 1 AOT, Phase 2 JIT, Phase 3 Extensions) |
| [`docs/okf/perf-measurements.md`](docs/okf/perf-measurements.md) | Measured optimizer costs & JIT-vs-GCC data |
| [`docs/okf/sanitizer-roadmap.md`](docs/okf/sanitizer-roadmap.md) | Target-side sanitizer roadmap — Phase 1 (signed-overflow) implemented in Sprint 116; later phases parked |
| [`docs/CHANGELOG.md`](docs/CHANGELOG.md) | Full release notes and changelog |
| [`docs/ZEROCC_Development_Plan.md`](docs/ZEROCC_Development_Plan.md) | Project development roadmap |
| [`docs/coding_style.md`](docs/coding_style.md) | C style & convention rules |
| [`tools/README.md`](tools/README.md) | Verification & tooling suite documentation |

## Verification & Tooling Suite

ZeroCC includes automated verification suites in [`tools/`](tools/)
(historical debugging scripts are archived in [`tools/archive/`](tools/archive/)):

```bash
python3 tools/c23/test_c23.py              # C23 differential conformance gate vs gcc-15
python3 tools/project_diff.py              # Real-world project compilation gate (cJSON, zlib, sqlite3.c)
python3 tools/c11/test_c11.py --verbose    # C11 feature & concurrency suite
python3 tools/vla/test_vla.py              # VLA & matrix VLA suite
python3 tools/sret_verifier.py --verify-all  # sret ABI verifier
python3 tools/abi_interop.py                 # Bidirectional zerocc.o <-> gcc.o external ABI interop gate (Sprint 158)
python3 tools/type_tag_verifier.py --verify-all  # Per-temp IR type-tag invariants (Sprints 118–120)
python3 tools/loop_unroll_verifier.py      # -O3 loop unrolling verifier: constant-trip full + runtime-bound partial (Sprints 150/156)
python3 tools/diag_battery.py              # Differential diagnostics vs gcc (ratchet baseline)
python3 tools/style_check.py               # C style/convention checker
python3 tools/simd_regression_suite.py     # Auto-vectorization regression suite
python3 tools/cross_isa_comparator.py test/aot/hello_world.c  # Multi-ISA asm comparison
python3 tools/isa_qemu_diff.py --isa riscv64   # Raw-emitter QEMU differential gate
python3 tools/isa_qemu_diff.py --isa riscv64 --vec   # SIMD vector raw-emitter QEMU gate
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
  `zero_rt` is compiled **by zerocc itself** across all 4 target ISAs
  (x86-64, AArch64, RISC-V 64-bit, and RISC-V 32-bit), archived with
  `zerocc --ar`, and `--zerocc-link` produces static executables with no host
  libc (see [`docs/okf/zero-rt.md`](docs/okf/zero-rt.md)). zerocc compiles its
  own individual TUs (static-locals, string-pointer arrays, and other
  self-hosting blockers were closed in Sprint 41)
- **Phase 3 — full self-compilation bootstrap**: in progress — next
  milestones are the Windows (PE/COFF + Win64 ABI) and macOS (Mach-O)
  linker/runtime phases in the development plan

## Acknowledgements
The `gcc.c-torture` conformance corpus is vendored from `gcc-mirror`, the
host static toolchain vendors `musl-1.2.6`, and float formatting in `zero_rt`
uses the vendored Ryū algorithm.
