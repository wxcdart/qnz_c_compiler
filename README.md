# zero_cc — Standalone Embeddable C99/C11/C23 Compiler

ZeroCC is a lightweight, embeddable C99/C11/C23 compiler (with GNU extensions) with a two-pass architecture: a flat 172-opcode IR that is lowered, optimized, register-allocated, and emitted to any of five target configurations across four architecture families (`x86-64`, `aarch64`, `aarch32`, `riscv64`, `riscv32`). It compiles to machine code for a JIT (`--run`, the default) or to assembly, ELF objects, and — via ZeroCC's own linker — directly-executable freestanding or baremetal binaries linked against its own `zero_rt` runtime or embedded linker scripts, with no host libc involved.

## Features

- **C99/C11/C23** compiler, freestanding, with optional `zero_rt` standard library
- **Preprocessor**: full `#define`/`#if`/`#include`/`#pragma` with `-E` preprocess-only mode, gcc-differential conformance (`make test-preproc` gate: `zerocc -E` vs `gcc -E` over `test/preproc/` — C11 6.10.3.1p2 stringify/paste order, `##` token fusion, GNU `x, ##__VA_ARGS__` comma drop, phase-2 splices)
- **Differential fuzzing** (`make test-fuzz` gate): seeded random-C generator scoped to the supported feature set (integers incl. `__int128`, structs, VLAs, pointers, control flow, recursion, K&R, `sizeof`, designated init, compound literals); each generated program self-checks against a Python model of the same AST and is differential-executed vs a reference gcc at `-O0` AOT + `-O0` JIT; failing seeds ratchet into `test/fuzz/baseline.json`
- **Compile-performance budget** (`make test-perf` gate): ratcheted compile-time budgets over a fixed increasing-size corpus (bench workloads, the temp-id churn stress, cJSON, zlib `deflate.c`, the 255k-line `sqlite3.c`, and a pinned 20-file c-torture sample) — tracks wall time per `-O` level plus per-pass optimizer time via `--profile-opts`, median of 5-7 runs, ratcheted against `test/perf/baseline.json`; guards against compile-speed regressions (and the c-torture sweep's wall time)
- **C23 Conformance**: `constexpr` data declarations, `nullptr`, `true`/`false` keywords, `typeof`/`typeof_unqual`, `#warning` (C23 6.9.1), empty `{}` initializers, `[[attr]]` attributes, designator-based unsized array sizing (`make test-c23` gate vs gcc-15)
- **Multi-project validation**: sqlite3.c amalgamation (255k lines), 15-TU zlib, and cJSON compiled under zerocc at `-O0` and `-O2` (`make test-project-diff`)
- **`__zerocc_syscall(number, a, b, c, d, e, f)`** and **`__zerocc_clone(...)`** — direct OS-syscall and thread-clone builtins (`OP_SYSCALL`, `OP_CLONE`) across all 4 ISAs (the `zero_rt` OS boundary)
- **Multi-ISA Target Architecture**: x86-64, AArch64, AArch32 (ARMv7-A), RISC-V 64, RISC-V 32 (`--target`); every ISA has an assembly-text path (`-S`) and a raw binary emitter (`-c`, `--zerocc-link`), differential-tested under QEMU
- **Modular Multi-ISA Architecture (Sprint 186–214 Refactoring, Phases 1–5 Complete)**:
  - **Phase 1 (TargetTraits)**: Centralized target trait queries (`src/zero_cc/isa/target_traits.h`, `ZeroCCTargetTraits`) for pointer width, stack/code alignment, sret conventions, register bit widths, and feature capabilities (`has_vector`, `has_direct_div`, `has_atomic`, etc.), eliminating scattered ad-hoc `if (c->isa == ...)` checks.
  - **Phase 2 (Table-Driven Register Allocator)**: Decoupled `TargetRegDesc` table in `src/zero_cc/ir/regalloc.h` and `target_traits.h`, replacing hardcoded ISA switch blocks and scratch assignments with declarative per-target register descriptor tables and dynamic FP/vector register pools (widened up to 32 registers).
  - **Phase 3 (Decoupled Calling Convention & ABI Classification)**: Clean ABI classification interface in `src/zero_cc/ir/abi_classify.h` (`zero_cc_abi_type_uses_sret`, `zero_cc_abi_arg_is_memory`, `classify_arg`, `classify_return`) and backend vtable variadic hook (`emit_variadic_setup`), isolating System V / AAPCS / LP64 calling conventions from AST lowering.
  - **Phase 4 (Modular Linker Relocations)**: `TargetLinkerOps` vtable in `src/zero_cc/linker/linker_reloc.h` for per-architecture relocation application, PLT stub generation, and page sizing, replacing cascading switch blocks in `linker.c`.
  - **Phase 5 (ASM Emitter Backend Hooks)**: Sized memory loads/stores, branch conditions, and overflow traps in `src/zero_cc/ir/asm_emitter.c` delegated to `ZeroCCISABackend` hooks (`emit_load_sized_asm`, `emit_store_sized_asm`, etc.), eliminating inline text formatting conditionals.
- **Advanced SIMD Vector Architecture & Auto-Vectorization (Sprints 191–212)**:
  - **Multi-ISA Vector ISA Coverage**: 128-bit SSE2/SSE4.1, 256-bit AVX/AVX2 VEX, 512-bit AVX-512 EVEX with dedicated opmasks (`k0..k7`), AArch64 NEON and SVE/SVE2, and RISC-V RVV 1.0 (`Zve32x`/`Zve64d`/`V`).
  - **Horizontal Vector Reductions**: Loop auto-vectorization for horizontal reduction loops (`sum += a[i]`, min/max) using hardware vector reductions (`OP_V128_HADD`, `OP_RVV_VREDSUM`, `OP_SVE_UADDV`).
  - **Strided & AoS Vectorization**: Support non-unit constant induction strides (`a[2*i]`) and Array-of-Structures layouts (`points[i].x`) via hardware strided memory ops (`OP_V128_LOAD_STRIDED`, `OP_V128_STORE_STRIDED`, RVV `vlse`/`vsse`, AArch64 structured lanes, x86 lane extraction).
  - **SLP (Superword-Level Parallelism)**: Straight-line basic-block vectorization grouping isomorphic contiguous stores, memory copies, arithmetic, and scalar broadcasts without requiring loops (`opt_slp.c`).
- **Advanced Loop Optimizations (Sprints 215–216)**:
  - **Loop Unswitching (Sprint 216)**: Hoists loop-invariant conditional branches (`if (flag)`, `if (mode == 1)`) outside loop bodies by cloning the loop into specialized branches (`opt_loop_unswitch.c`), eliminating per-iteration branch evaluation overhead and exposing straight-line loop bodies.
  - **Loop Peeling (Sprint 216)**: Peels the initial iteration of countable loops into the preheader, resolving first-iteration boundary checks (e.g. `if (i == 0)`) into compile-time constants and eliminating boundary branches.
  - **LICM Store Hoisting / Scalar Replacement of Aggregates (SROA, Sprint 215)**: Hoists invariant memory writes (`*p = val`, `*p += val`, `p->field += val`) outside loop bodies by loading into a register accumulator in the preheader and committing the final store on loop exit (`opt_licm_store_hoist`).
  - **Loop Fusion / Jamming (Sprint 215)**: Merges adjacent loops with identical trip counts, matching bounds, and compatible data dependencies into a single combined loop body (`opt_loop_fusion.c`), eliminating redundant loop index increments and branch overhead while exposing larger instruction windows to GVN, SLP, and SIMD auto-vectorization.
- **Direct ELF Section Emission & Extended Linker Dead-Stripping (`--gc-sections`, Sprint 213)**:
  - Direct ELF object mode (`-c`) emission of separate per-function (`.text.<func>`) and per-data (`.data.<var>`, `.bss.<var>`, `.rodata.<var>`) sections with dynamic relocation re-routing (`-ffunction-sections`, `-fdata-sections`).
  - Extended own-linker mark-and-sweep section garbage collection (`--gc-sections`, `--print-gc-sections`) with custom entry points (`-e`, `--entry`) and retained undefined symbols (`-u`, `--undefined`).
- **Embedded Baremetal CPU Support (Sprint 186)**:
  - **Declarative GNU Linker Script Parsing (`-T <file>`)**: Integrated parser in `src/zero_cc/linker/linker_script.h` supporting `MEMORY` commands (`FLASH`, `RAM` with `ORIGIN` and `LENGTH`), `ENTRY(symbol)`, `_stack_top = ORIGIN(RAM) + LENGTH(RAM)`, and basic expressions.
  - **LMA vs VMA Physical/Virtual Address Mapping**: Load Memory Address (LMA) in Flash vs Virtual Memory Address (VMA) in RAM separation with dedicated ELF Program Headers (`p_paddr` != `p_vaddr`) for ROM-to-RAM runtime copying.
  - **Standard Embedded Boundary Symbols**: Automatic generation and resolution of `_sidata` (Flash load address of initialized data), `_sdata` / `_edata` (RAM runtime bounds of `.data`), `_sbss` / `_ebss` (RAM zero-init bounds of `.bss`), and `_stack_top` (initial stack pointer).
  - **Freestanding Embedded Environment**: `--baremetal`, `-ffreestanding`, and `-nostdlib` compiler and driver flags; predefined macros `__STDC_HOSTED__=0` and `__BAREMETAL__=1`.
  - Verified across all 5 target ISAs via `make test-baremetal` (`tools/verify/baremetal_test.py`).
- **Freestanding Microcontroller Runtime & BSP (`zero_rt_baremetal.a`, Sprint 192)**:
  - **Startup crt0 (`startup.c`)**: Hardware `Reset_Handler` / `_start` entry point copying initialized `.data` from Flash LMA (`_sidata`) to RAM VMA (`_sdata`..`_edata`), zeroing uninitialized `.bss`, invoking `main()`, and halting the CPU on exit.
  - **Memory-Mapped Serial UART Drivers (`uart.c`, `uart.h`)**: Standard MMIO UART drivers for 16550 (RISC-V base `0x10000000`) and ARM PL011 (ARM/AArch32 base `0x09000000`), plus in-memory circular trace buffer (`UART_RING_BUF_SIZE = 2048`).
  - **Freestanding OS Layer & Microcontroller Memory Management (`os.c`, `heap.c`)**: Physical bump-pointer allocator (`brk`) bounded between `_ebss` and `_stack_top`, compact integer/fraction float formatting (`bm_fmt_float`) eliminating 191KB of table bloat, and standalone multiword arithmetic helpers.
  - Automated execution verification under virtual simulation (`make test-baremetal`).
- **RISC-V 32-bit 64-bit Integer (`long long`) Architecture (All 4 Phases Complete, Sprints 170–174)**:
  - **Phase 1 (Core Arithmetic & Ops)**: 64-bit integer expressions (`long long`, `uint64_t`) lowered into `(lo, hi)` 32-bit GPR pairs (`REG_CLASS_GPR64`) with dedicated IR opcodes (`OP_*64`: ADD, SUB, MUL, AND, OR, XOR, NOT, NEG, SHL, SHR, SAR, CMP, MOV, LOAD, STORE, ZEXT, SEXT, TRUNC, MOV_IMM) across raw binary and `-S` assembly emitters.
  - **Phase 2 (RISC-V ILP32 psABI Calling Conventions)**: Aligned even-odd register pair argument passing (`a0:a1`..`a6:a7`), 8-byte stack alignment, `(a0, a1)` function return values (`OP_MK64`/`OP_HI64`), callee parameter reception, variadic `printf("%lld", ...)`, and strict ILP32 `long` (32-bit) vs `long long` (64-bit) scoping.
  - **Phase 3 (64-bit Division & Modulo via libgcc)**: `OP_DIV64`, `OP_MOD64`, `OP_UDIV64`, `OP_UMOD64` lowered to `__divdi3`, `__moddi3`, `__udivdi3`, `__umoddi3` in libgcc with alias-safe argument staging, own-linker libgcc shim, regalloc thief handover, and stack spill slot displacement correction.
  - **Phase 4 (Target Sanitizer Width-8 Checks)**: Full `-fsanitize=undefined` integration: signed-overflow traps (`ADD64`, `SUB64`, `MUL64`), division and remainder by zero, and shift-count bounds checks on RV32 with spill reloading and live range preservation.
- **Floating-point conformance**: `__builtin_issignaling`, `__builtin_{inf,nan,nans}*`, C23 `_FloatN` types and width-suffixed literals, sNaN-preserving 4-byte FP load/store (Sprint 160)
- **Inline assembly (Phases 1–3 complete)**: GNU extended asm (`__asm__ [volatile] [goto] ("..." : outs : ins : clobbers [: labels])`) with `r`/`m`/`=r`/`+r`/fixed-register (`a`/`b`/`d`/`S`/`D`) operands, matching-digit constraints, symbolic `%[name]` / `%l[name]` operands, AT&T/Intel `{att | intel}` dialect alternatives, and `memory`/`cc`/GPR clobbers — lowered to `OP_ASM`, register-allocated (operands `no_spill`, clobbered registers forbidden for operands, live values across clobbers spilled), and supported across AOT `.s` text emission, in-memory JIT, and raw binary emission via the integrated assembler (`make test-asm` 72/72 runs vs gcc; [`docs/okf/inline-assembly-roadmap.md`](docs/okf/inline-assembly-roadmap.md))
- **`-fsanitize=undefined`** — target-side signed-integer-overflow traps, division/remainder by zero, and shift-count bounds checks on all four ISAs (x86-64 `ud2` via OF flag / compare; AArch64 `brk #0` and RISC-V `unimp` via compare-based lowering); both emitters; check width follows usual arithmetic conversions; covers signed `+ - *`, `++`/`--`, `+= -= *=`, `/=`, `%=`, `<<=`, `>>=`, at both width 4 and width 8 (including RV32 64-bit pairs, Sprints 116–117, 161, 163, 166, 174; `make test-san` flag-ON gate incl. cross-ISA qemu trap-firing across all 4 target ISAs; [`docs/okf/sanitizer-roadmap.md`](docs/okf/sanitizer-roadmap.md))
- **`-fstack-protector` / `-fstack-protector-strong` / `-fstack-protector-all`** — GCC/Clang-compatible stack canary guards across all 4 ISAs (`%fs:40` on x86-64, `__stack_chk_guard` on AArch64/RISC-V; Sprint 147)
- **Multi-translation-unit driver & whole-program LTO (`-flto`)**: multiple `.c`, `.o`, and `.a` inputs per invocation (JIT image merge + per-input `-c`/`-S`); `-flto` enables two-phase cross-TU inlining, Fat-LTO `.zc.lto` sections in `.o`/`.a`, constant global propagation, pure const-return folding, inter-procedural constant propagation (IPCP), dead branch/function/global elimination, and static symbol isolation (Sprint 195)
- **Own ELF64/ELF32 linker & `ar` archiver** (`--zerocc-link`, `--ar`): static `ET_EXEC` output across all 5 target configurations (`x86-64`, `aarch64`, `aarch32`, `riscv64`, `riscv32`) with no host libc or crt files, native GNU `ar` archive reader/writer, cross-TU dead code & data elimination (`--gc-sections`, `--print-gc-sections`, `-ffunction-sections`, `-fdata-sections`), linker script `KEEP(*(pattern))` retention, `zero_rt` freestanding runtime auto-linking, and embedded baremetal linker script support
- **Full Self-Hosting & Own-Linker Fixed-Point Convergence (Sprint 184)**: ZeroCC compiles itself end-to-end (all 76 library translation units + `main.c` + host runtime) with `zerocc -c` (`make stage1`), passes all test suites, and links itself with its own static linker (`zerocc --zerocc-link`, `make stage1-ownlink`). Stage 1 and Stage 2 achieve **100% bit-for-bit identical binary convergence** (`53,553,224 bytes`, `make test-self-host-ownlink`).
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
- `__builtin_{add,sub,mul}_overflow{,_p}` with the exact GCC model (own-signedness operands, both-unsigned domain, 128-bit-precision signed mul; Sprints 61 & 159)
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
- **Hermetic static-musl host & in-process dynamic loader (ZeroCC Differentiator)**: `build/bin/zerocc` is 100% statically linked against in-tree vendored musl (`--whole-archive`) with zero host glibc dependency (`ldd`: `"not a dynamic executable"`). Unlike Clang/GCC which lose `dlopen()` when statically linked (static musl stubs fail with `ENOSYS`), ZeroCC embeds its own **in-process ELF64 dynamic linker** (`zero_rt/dlfcn/dynlink.c`):
  - JIT code (`--run`) can `dlopen()` real `.so` libraries (e.g. vendor hardware drivers, proprietary SDKs, graphics/audio plugins) with zero host `ld.so`.
  - **Recursive `DT_NEEDED`**: automatically walks and loads dependency trees with cycle safety and dependency-ordered constructor execution.
  - **`$ORIGIN` RPATH / RUNPATH**: enables self-contained relocatable application and driver bundles.
  - **Direct filesystem resolution (Host Cache Independence)**: immune to host `/etc/ld.so.cache` or `/etc/ld-musl-*.path` state; resolves libraries directly via `access()` across `$ORIGIN`, `LD_LIBRARY_PATH`, and standard system directories (`/lib/x86_64-linux-gnu`, `/usr/lib/x86_64-linux-gnu`, `/lib64`, `/usr/lib64`, `/lib`, `/usr/lib`, `/usr/local/lib`) with instant visibility and no `ldconfig` required. Non-standard paths (e.g. `/opt/vendor/lib`) resolve intuitively via `RPATH`, `LD_LIBRARY_PATH`, or explicit paths.
  - **GNU Symbol Versioning**: parses `.gnu.version`, filters hidden versions for default `dlsym()`, and supports explicit version selection via `dlvsym()`.
  - **Host Symbol Bridging**: loaded `.so` files can call back into static musl symbols via ZeroCC's pre-baked `.rodata` embedded symbol table (`tools/embed_symtab.py`). See [`docs/okf/host-runtime-and-portability.md`](docs/okf/host-runtime-and-portability.md).
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

# Self-hosting bootstrap verification
make stage1-ownlink         # build self-hosted stage 1 with ZeroCC's own linker
make test-self-host-ownlink # verify 100% bit-for-bit identity across bootstrap cycle
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
  -flto / -fno-lto  Enable/disable cross-TU link-time optimization (Fat-LTO in .o/.a)
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
  --target <arch> Target architecture (x86-64, aarch64, aarch32, riscv64, riscv32)
  --baremetal     Baremetal / embedded CPU target mode
  -ffreestanding  Freestanding execution environment (__STDC_HOSTED__=0, __BAREMETAL__=1)
  -nostdlib       Do not link against standard libraries or startup objects
  -T <file>       Linker script defining MEMORY regions (FLASH/RAM) and LMA/VMA mapping
  -fsanitize=undefined  Target-side UB traps (signed overflow for +,-,*, ++/--,
                       compound assign; div/rem by zero; shift bounds) across
                       all ISAs at width 4 and 8 (both emitters; Sprints 116–174)
  --zerocc-link  With -o: link with ZeroCC's own linker (x86-64, aarch64,
                aarch32, riscv64, riscv32); auto-links zero_rt (opt out: --no-zerocc-rt)
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
│       ├─ core/           # Compiler state, preprocessor, pragma state, LTO engine (lto.h), multi-TU merge (multi_tu.h)
│       ├─ driver/         # Driver helpers extracted from main.c: CLI option
│       │                   # parsing (driver_opts.h) & AOT asm emission (driver_asm.h)
│       ├─ lexer/          # Tokenizer (C99/C11/C23 keyword tables)
│       ├─ parser/         # Arena-based AST parser, const eval, static assert
│       ├─ semantic/       # Semantic analysis: types, scopes, diagnostics
│       ├─ ir/             # 172-opcode Instr IR, AST→IR lowering,
│       │                   # linear-scan regalloc (TargetRegDesc), abi_classify,
│       │                   # asm/machine emitters
│       ├─ opt/            # IR passes: constant_fold, dce, copy_prop,
│       │                   # mem2reg, cse, gvn, licm, branchless,
│       │                   # strength_reduction, branch_simplify,
│       │                   # loop_unroll, auto_vectorize, CFG + dominators
│       ├─ isa/            # TargetTraits + Backend vtable + x86_64, aarch64,
│       │                   # aarch32, riscv (64/32)
│       ├─ jit/            # JIT image build/merge, ELF emission, TLS backend
│       ├─ linker/         # ZeroCC's own ELF64/ELF32 static linker, TargetLinkerOps,
│       │                   # linker_script (GNU ld scripts, LMA/VMA) & ar writer (T2)
│       └─ types/          # Type system, symbols, virtual registers
├─ zero_rt/                # Freestanding runtime (heap, strings, stdio, C11/POSIX
│                           # threads, int128, complex __divdc3) compiled by zerocc
│                           # for all ISAs + host self-hosting dlfcn/
├─ third_party/vendor/     # Vendored deps (musl-1.2.6, Ryū, gcc.c-torture corpus)
├─ test/                   # Regression suites (unit, semantic, abi, jit, aot,
│                           # driver, linker, baremetal, licm, inline_audit,
│                           # isa_qemu, isa128, torture, diag, san, abi_interop, bench, ...)
├─ tools/                  # Verification & tooling (see tools/README.md)
├─ docs/                   # OKF knowledge graph (docs/okf/), CHANGELOG, plan,
│                           # coding style, archive
├─ .agents/skills/         # Agent skills (c-torture gate, OKF, debugging, runtime integration)
├─ AGENTS.md               # Contributor / agent workflow & verification rules
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
five implementations — `x86_64.c`, `aarch64.c`, `aarch32.c`, `riscv.c` (64- and 32-bit) —
alongside declarative target traits (`src/zero_cc/isa/target_traits.h`), table-driven
register descriptors (`src/zero_cc/ir/regalloc.h`), and modular linker operations
(`src/zero_cc/linker/linker_reloc.h`):

- `isa/target_traits.h` — Centralized `ZeroCCTargetTraits` query for pointer size, alignment, and ISA capabilities
- `ir/abi_classify.h` — Decoupled ABI struct and argument classification
- `jit/tls_backend.c` — `_Thread_local` (`.tdata`/`.tbss`, x86-64 initial-exec idiom, JIT data-section fallback)
- per-ISA asm emitters (`*_asm.c`) for AOT `.s`/`.o` output with backend hooks for sized loads/stores
- `linker/` — ZeroCC's own ELF64/ELF32 object reader, GNU ld script parser (`linker_script.h`), modular relocation engine (`linker_reloc.h`), and GNU `ar` archive reader/writer (T2)

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
| **`-flto`** | Two-phase cross-TU inlining (`-O1`+), global constant scalar propagation, 3-round pure const-return & IPCP fixpoint, constant-branch/post-terminator pruning, Fat-LTO `.zc.lto` objects/archives, and demand-driven dead function/global elimination |

Inlining uses conservative candidate filters (no FP signatures, no body
locals, no labels/gotos, no `&param`, no value-param writes — see
`test/inline_audit/`). Per-pass timing is available via `--profile-opts`
(measurements in [`docs/okf/perf-measurements.md`](docs/okf/perf-measurements.md));
compile-speed regressions are gated by the ratcheted
`make test-perf` performance budget (`test/perf/baseline.json`).

## Cross-Compilation, Own Linker & zero_rt

### Cross-compiling

```bash
build/bin/zerocc prog.c --target aarch64 -o prog.aarch64
build/bin/zerocc prog.c --target aarch32 -o prog.arm
build/bin/zerocc prog.c --target riscv64 -c -o prog.rv64.o
build/bin/zerocc prog.c --target riscv32 -o prog.rv32
build/bin/zerocc prog.c --target aarch32 -T test/baremetal/flash_ram.ld -o prog.elf
```

The riscv64/aarch64/aarch32 `-o` system-linker paths use the installed cross gcc
(`riscv64-linux-gnu-gcc` / `aarch64-linux-gnu-gcc` / `arm-linux-gnueabihf-gcc`, `-static`). The riscv32
path needs an explicit static-musl prefix (the Debian rv64 cross gcc ships no
rv32 musl); the driver discovers it as `$ZC_RV32_MUSL` or, by default, the
in-tree committed `third_party/vendor/musl-cross/riscv32` (Sprint 157 — no
setup needed on any machine; rebuild in place with
`tools/setup_musl_cross.sh --arch riscv32 ...`).

### Own linker & archiver (T2)

`--zerocc-link` links the per-TU objects with **ZeroCC's own linker** instead
of the system linker across all 5 target configurations (`x86-64`, `aarch64`,
`aarch32`, `riscv64`, `riscv32`). The result is a directly-executable **static `ET_EXEC`**
(ELF64 or ELF32) — no libc, no crt files — and by default it auto-links the
matching `zero_rt` runtime archive:

```bash
build/bin/zerocc a.c b.c --zerocc-link -o app      # freestanding executable
build/bin/zerocc a.c lib.a --zerocc-link -o app    # archive member extraction
build/bin/zerocc a.c --zerocc-link --no-zerocc-rt -o app  # no runtime
build/bin/zerocc a.c --zerocc-link -T script.ld -o app.elf # baremetal with linker script
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

Gates: `make test-linker` (own linker & archiver across all ISAs, gcc-differential),
`make test-baremetal` (linker scripts, LMA/VMA mapping, boundary symbols), and
`make test-isa-qemu` (raw binary emitter for all ISAs under QEMU, diffed
against a static-musl reference). See
[`docs/okf/own-linker.md`](docs/okf/own-linker.md).

## Test Results

Current status (last full verification 2026-11-21, Sprints 170–195; counts drift as tests are added — `docs/CHANGELOG.md` has the per-sprint figures):

| Suite | Status | Count |
|-------|--------|-------|
| **Main regression suite** (`make test`: unit, semantic, ABI, JIT, AOT, asm reference) | ✅ Pass | 237/237 |
| **External ABI interop gate** (`make test-abi-interop`: bidirectional `zerocc.o ↔ gcc.o` — zerocc main + gcc main, 15 calling-convention scenarios at `-O0`/`-O1`/`-O2`, incl. MEMORY struct args, SSE/SSEup 16B structs, `sret`, variadic struct args) | ✅ Pass | 15/15 |
| **Optimizer sweep** (`make test-opt`, `-O0..-O3` × JIT + AOT via zerocc) | ✅ Pass | 135 ok, 0 failed (embedding tests skipped by design — covered by `make test`) |
| **Baremetal CPU verification gate** (`make test-baremetal`: linker script `-T`, LMA/VMA mapping, boundary symbols across all 5 target ISAs) | ✅ Pass | 5/5 ISAs |
| **Self-hosting bootstrap fixed-point gate** (`make test-self-host`, `make test-self-host-ownlink`: Stage 1 == Stage 2 bit-for-bit identity across all 76 TUs) | ✅ Pass | 100% bit-for-bit identical (53,553,224 bytes) |
| **Self-hosting compilation audit** (`make audit-self-host`: all 76 translation units in `src/` compiled by `zerocc -c`) | ✅ Pass | 76/76 TUs |
| **Preprocessor differential gate** (`make test-preproc`: `zerocc -E` vs `gcc -E` over `test/preproc/` corpus diffed line-by-line, ratcheted baseline) | ✅ Pass | 18/18 files |
| **C23 differential gate** (`make test-c23`: 24 cases across gnu23/c11/c99 vs gcc-15) | ✅ Pass | 24/24 |
| **Differential fuzzing gate** (`make test-fuzz`: seeded random-C self-checking programs vs gcc-15 at `-O0` AOT + `-O0` JIT, ratcheted `test/fuzz/` baseline) | ✅ Pass | 106/106 (14 UB discards) |
| **Compile-performance budget gate** (`make test-perf`: ratcheted compile-time budgets over an increasing-size corpus incl. `sqlite3.c`, per `-O` level wall + per-pass optimizer time, ratcheted `test/perf/` baseline) | ✅ Pass | 20/20 case-levels within budget |
| **Inline-asm Phases 1–3 gate** (`make test-asm`: `test/asm/*.c` across `-S` text, JIT, and raw AOT at `-O0..-O3` vs host-gcc reference) | ✅ Pass | 72/72 |
| **Project-differential gate** (`make test-project-diff`: cJSON, zlib, sqlite3.c at -O0/-O2) | ✅ Pass | 36/36 TUs |
| **`zero_san` flag-ON sweep** (`make test-san`: `san_ok_*` clean + `san_trap_*` SIGILL at `-O0..-O3`, JIT + AOT, cross-ISA qemu smoke on **both** raw-AOT and `-S`+cross-assembler paths) | ✅ Pass | 720 ok / 0 FAIL (174 cross-ISA runs, both paths × all 4 ISAs) |
| **Multi-file driver** (`make test-driver`, multi-TU JIT/AOT + per-input `-c`/`-S` + `-flto`) | ✅ Pass | 49/49 |
| **Own linker & archiver** (`make test-linker`, `--zerocc-link` + `--ar` across all ISAs + gcc differential + Fat-LTO `.o`/`.a`) | ✅ Pass | 54/54 |
| **Cross-ISA raw-emitter QEMU gate** (`make test-isa-qemu`, vs static-musl reference) | ✅ Pass | 80/80 (all 5 ISAs: x86-64, aarch64, aarch32, riscv64, riscv32; 0 skips) |
| **Cross-ISA `__int128` raw-emitter QEMU gate** (`make test-isa128`, vs static-musl reference) | ✅ Pass | 144/144 (all 3 ISAs × 6 tests × 4 levels × 2 paths) |
| **Cross-ISA dynamic loading gate** (`make test-dlfcn-isa`, vs static-musl reference across all 4 ISAs) | ✅ Pass | 4/4 (x86-64 native, AArch64, RISC-V 64, RISC-V 32 under QEMU) |
| **GCC c-torture conformance gate** (`make test-torture`, ratcheted baseline, gcc-15 reference) | ✅ Pass | 3,781 PASS / 3,940 corpus (0 MISCOMPILE / 0 REJECT; SKIP-SUPPORT 87, SKIP-C89 33, SKIP-REFSEM 7, SKIP-TARGET: arm 2, ia64 1, mips 4, powerpc 2, s390x 1, windows 1, float16/bfloat16/float128x runtime 3, SKIP-UB/PTR/BOGUS 3, EXPECTED-REJECT 4, NO-REF 1, REF-BUG 11) |
| **Type-tag verifier** (`tools/verify/type_tag_verifier.py --verify-all`, invariants I1–I10 at `-O0/2/3`) | ✅ Pass | 168 files, RESULT OK |
| **Loop unroll verifier** (`tools/verify/loop_unroll_verifier.py`, `-O3` constant-trip full unrolling + runtime-bound partial unrolling w/ remainder peel, `expect-unroll`/`expect-partial` markers) | ✅ Pass | 25/25 |
| **ASan/UBSan rebuild** (`make asan`) | ✅ Pass | 233/233 |
| **`sret` ABI verifier** (`tools/verify/sret_verifier.py --verify-all`) | ✅ Pass | 6/6 |
| **C11 verification suite** (`tools/c11/test_c11.py`) | ✅ Pass | 11/11 |
| **VLA verification suite** (`tools/vla/test_vla.py`) | ✅ Pass | 11/11 (incl. VLA-in-loop / VLA-in-block runtime tests — C99 6.7.5.2 legal, gcc-differentially verified) |
| **Diagnostics battery** (`tools/verify/diag_battery.py`, vs gcc) | ✅ Pass | 18 cases, 9/9 error categories |
| **C style checker** (`tools/verify/style_check.py`) | ✅ Pass | 193 files, 0 errors, 0 warnings (Sprint 195) |

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
| **`_Alignas` / `_Alignof`** | ✅ Implemented | Struct fields, file-scope objects (any N, `.data`/`.bss` section alignment), and locals (`N > 16` up to 4096 via runtime SP realign prologue on x86-64, AArch64, AArch32, RISC-V 64/32 across raw MC and `-S` text paths; VLA/variadic fns fall back to 16 with a warning) |
| **`_Noreturn`** | ✅ Implemented | `<stdnoreturn.h>` virtual header |
| **`_Static_assert`** | ✅ Implemented | Constant-evaluated at compile time |
| **`_Generic`** | ✅ Complete | Full C11 qualification stripping/nested compatibility (`const`/`volatile`/`restrict`/`_Atomic`, multi-level pointers, array qualifiers per 6.7.3p9, fn-pointer params, `const_eval`, `typeof`) |
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
| **GCC 14 / 13** | Any recent | GCC 14.2 / 13.3 | Multi-version baseline probes and diagnostics validation (`tools/verify/diag_battery.py`) | **Differential** |
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
tools/toolchain/setup_musl_cross.sh --arch aarch64 --ccbin aarch64-linux-gnu-gcc
tools/toolchain/setup_musl_cross.sh --arch riscv64 --ccbin riscv64-linux-gnu-gcc
tools/toolchain/setup_musl_cross.sh --arch riscv32 --ccbin riscv64-linux-gnu-gcc --extra "-march=rv32gc -mabi=ilp32" --host riscv32-linux-musl
```

### Tools Layout

ZeroCC's verification tooling lives in `tools/` organized by concern:

| Subpackage | Contents |
| :--- | :--- |
| `tools/verify/` | Regression gates: san_sweep, opt_sweep, baremetal_test, torture_run, diag_battery, test_runner, driver_tests, linker_tests, self_host_audit, self_host_cycle, preproc_diff, perf_budget, project_diff/runs, abi_interop, asm_diff, cfg_verifier, loop_unroll_verifier, style_check, sret_verifier, type_tag_verifier, isa_qemu_diff, isa128_diff, dlfcn_isa_diff, regress_exec_diff, extension_verifier |
| `tools/analysis/` | Codegen inspection: elf_verify, elf_min, riscv_enc_check, reloc_pair_check, vtable_check, cross_isa_comparator, struct_abi_verifier, struct_return_verifier, struct_asm_analyzer, prologue_epilogue_verifier, simd_regression_suite, literal_verifier, qemu_test_runner |
| `tools/okf/` | Knowledge graph tooling: okf.py |
| `tools/toolchain/` | Toolchain setup: setup_musl_cross.sh, musl_host_toolchain.sh, embed_symtab.py, setup_dev_env.sh |
| `tools/suites/` | Test suite generators: struct_test_generator.py |
| `tools/c11/`, `tools/c23/`, `tools/vla/`, `tools/fuzz/` | Standalone dialect & fuzzing verification suites |

All tools are stdlib-only (see `tools/requirements.txt`) and are run as standalone scripts:
`python3 tools/<subdir>/<script>.py`. Each tool locates the repository root via
`find_repo_root()` (`tools/__init__.py`) instead of hardcoded relative paths,
so moving or nesting `tools/` deeper never breaks path resolution. See
`tools/README.md` for the full tool table and guidance on adding new tools.

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

Gate counts live in [Test Results](#test-results) and the changelog, not here.
Run the cheap gates foreground; background the long ones (`make test-torture`).

```bash
make test               # Full regression suite (unit, semantic, ABI, JIT, AOT) — host-gcc-built test binaries
make test-opt           # Optimizer sweep -O0..-O3, JIT + AOT via zerocc itself (REQUIRED for any optimizer/regalloc change)
make test-san           # zero_san flag-ON soundness sweep (+ cross-ISA qemu smoke)
make test-baremetal     # Embedded baremetal CPU gate (linker scripts -T, Flash/RAM LMA/VMA, boundary symbols across all 5 ISAs)
make test-torture       # GCC c-torture conformance gate, ratcheted baseline (needs gcc-15; long — background it)
make test-c23           # C23 differential gate vs gcc-15
make test-preproc       # Preprocessor (-E) differential gate vs gcc-15
make test-fuzz          # Differential fuzzing gate vs gcc-15
make test-perf          # Compile-performance budget gate (ratcheted baseline)
make test-asm           # Inline-asm gate: test/asm/*.c across -S text, JIT, and raw AOT
make test-abi-interop   # Bidirectional zerocc.o <-> gcc.o external ABI interop
make test-project-diff  # cJSON / zlib / sqlite3.c compile gate at -O0/-O2
make test-project-runs  # Real-project execution gate
make test-loop-unroll   # -O3 loop unroll verifier (full + partial)
make test-driver        # Multi-TU driver, JIT/AOT + per-input -c/-S
make test-linker        # Own linker & archiver across all ISAs
make test-isa-qemu      # Raw-emitter QEMU gate (all ISAs; needs cross toolchains + QEMU)
make test-isa128        # Cross-ISA __int128 gate
make test-dlfcn-isa     # Cross-ISA dynamic loading gate (all ISAs under QEMU)
make audit-self-host    # Self-hosting compilation audit of all 76 TUs with zerocc -c
make test-self-host     # 3-stage self-hosting fixed-point bootstrap gate (Stage 1 == Stage 2)
make test-self-host-ownlink # Own-linker 3-stage self-hosting fixed-point bootstrap gate
make asan               # ASan+UBSan build of the compiler itself (run after core-pipeline changes)
```

**Rules of thumb:** `make test` builds its test binaries with host `gcc`, so it
does *not* exercise zerocc's optimizer or codegen — verify parser/IR/ABI changes
directly with `build/bin/zerocc` in both AOT and JIT modes, and run
`make test-opt` after any optimizer change. Never rebuild `build/bin/zerocc`
while a long gate is running. See [`AGENTS.md`](AGENTS.md) and
[`docs/okf/build-and-verification.md`](docs/okf/build-and-verification.md).

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
| [`docs/okf/ir-and-regalloc.md`](docs/okf/ir-and-regalloc.md) | 172-opcode `Instr` IR & linear-scan register allocation |
| [`docs/okf/optimization-passes.md`](docs/okf/optimization-passes.md) | Active IR optimization passes & safety invariants |
| [`docs/okf/build-and-verification.md`](docs/okf/build-and-verification.md) | Build targets, direct compiler testing & ABI verifiers |
| [`docs/okf/abi-sysv-x86_64.md`](docs/okf/abi-sysv-x86_64.md) | System V AMD64 ABI: struct argument/return classification (≤8B register, 9–16B SSE/GPR unit merge, >16B MEMORY args + `sret`) |
| [`docs/okf/isa-backends.md`](docs/okf/isa-backends.md) | Multi-ISA backend vtable, JIT vs AOT emission, relocations |
| [`docs/okf/global-data-relocs.md`](docs/okf/global-data-relocs.md) | Data section, global initializers & pointer relocations |
| [`docs/okf/tls-static.md`](docs/okf/tls-static.md) | Static TLS (`_Thread_local`) support |
| [`docs/okf/scalar-float-x86.md`](docs/okf/scalar-float-x86.md) | Scalar float pipeline (x86-64) |
| [`docs/okf/c-torture-gate.md`](docs/okf/c-torture-gate.md) | GCC `c-torture` differential conformance gate |
| [`docs/okf/target-abstraction-roadmap.md`](docs/okf/target-abstraction-roadmap.md) | Multi-ISA target modularity refactoring roadmap (Phases 1–5: TargetTraits, table-driven regalloc, decoupled ABI classification, modular linker relocations, clean ASM emitter) |
| [`docs/okf/inline-assembly-roadmap.md`](docs/okf/inline-assembly-roadmap.md) | Clang-style integrated inline assembly roadmap (Phase 1 AOT, Phase 2 JIT, Phase 3 Extensions) |
| [`docs/okf/perf-measurements.md`](docs/okf/perf-measurements.md) | Measured optimizer costs & JIT-vs-GCC data |
| [`docs/okf/sanitizer-roadmap.md`](docs/okf/sanitizer-roadmap.md) | Target-side sanitizer roadmap — Phase 1 (signed-overflow) implemented in Sprint 116, extended in Sprint 161; cross-ISA extension shipped in Sprint 163 (§4c); later phases parked |
| [`docs/CHANGELOG.md`](docs/CHANGELOG.md) | Full release notes and changelog |
| [`docs/ZEROCC_Development_Plan.md`](docs/ZEROCC_Development_Plan.md) | Project development roadmap |
| [`docs/coding_style.md`](docs/coding_style.md) | C style & convention rules |
| [`tools/README.md`](tools/README.md) | Verification & tooling suite documentation |

## Verification & Tooling Suite

ZeroCC includes automated verification suites in [`tools/`](tools/)
(historical debugging scripts are archived in [`tools/archive/`](tools/archive/)):

```bash
python3 tools/verify/torture_run.py               # GCC c-torture differential gate (ratcheted baseline)
python3 tools/verify/san_sweep.py                 # zero_san flag-ON soundness sweep
python3 tools/verify/opt_sweep.py                 # Optimizer sweep -O0..-O3, JIT + AOT
python3 tools/verify/baremetal_test.py            # Embedded baremetal CPU gate (-T, Flash/RAM LMA/VMA, symbols)
python3 tools/c23/test_c23.py                     # C23 differential conformance gate vs gcc-15
python3 tools/verify/preproc_diff.py              # Preprocessor differential gate vs gcc-15
python3 tools/fuzz/fuzz_diff.py                   # Differential fuzzing gate vs gcc-15
python3 tools/verify/perf_budget.py               # Compile-performance budget gate (ratcheted baseline)
python3 tools/verify/project_diff.py              # Real-world project compilation gate (cJSON, zlib, sqlite3.c)
python3 tools/verify/self_host_cycle.py           # Self-hosting 3-stage bootstrap fixed-point gate
python3 tools/c11/test_c11.py --verbose           # C11 feature & concurrency suite
python3 tools/vla/test_vla.py                     # VLA & matrix VLA suite
python3 tools/verify/sret_verifier.py --verify-all  # sret ABI verifier
python3 tools/verify/abi_interop.py               # Bidirectional zerocc.o <-> gcc.o external ABI interop gate (Sprint 158)
python3 tools/verify/type_tag_verifier.py --verify-all  # Per-temp IR type-tag invariants (Sprints 118–120)
python3 tools/verify/loop_unroll_verifier.py     # -O3 loop unrolling verifier: constant-trip full + runtime-bound partial (Sprints 150/156)
python3 tools/verify/diag_battery.py             # Differential diagnostics vs gcc (ratchet baseline)
python3 tools/verify/style_check.py              # C style/convention checker
python3 tools/analysis/simd_regression_suite.py    # Auto-vectorization regression suite
python3 tools/analysis/cross_isa_comparator.py test/aot/hello_world.c  # Multi-ISA asm comparison
python3 tools/verify/isa128_diff.py --isa aarch64 # Cross-ISA __int128 differential gate
python3 tools/verify/isa_qemu_diff.py --isa riscv64   # Raw-emitter QEMU differential gate
python3 tools/verify/isa_qemu_diff.py --isa riscv64 --vec   # SIMD vector raw-emitter QEMU gate
python3 tools/okf/okf.py lint && python3 tools/okf/okf.py index  # Validate the OKF knowledge graph
```

See [`tools/README.md`](tools/README.md) for the full tool table.

## Contributing

- Read [`AGENTS.md`](AGENTS.md) (build/verification rules) and
  [`docs/coding_style.md`](docs/coding_style.md) (layout, include order,
  naming `zero_cc_*` / `ZeroCC*` / `ZERO_CC_*`); run `python3 tools/verify/style_check.py`.
- Project headers are included relative to the include roots
  (`-Isrc -Iinclude`), e.g. `#include "zero_cc/core/core.h"` — no `../` or
  bare-sibling project includes.
- Consult the OKF graph ([`docs/okf/index.md`](docs/okf/index.md)) before
  scanning `docs/`. After a feature or fix, update the relevant OKF page and run
  `python3 tools/okf/okf.py update <page.md> --log "..."`, then
  `python3 tools/okf/okf.py lint && python3 tools/okf/okf.py index`.
- Work lands in numbered sprints with a `docs/CHANGELOG.md` entry each.

## Known Limitations

ZeroCC prioritizes standard C99/C11/C23 semantics, small binary size, and zero-dependency hermetic compilation. Certain advanced features, platform-specific extensions, and niche standard constructs are intentionally bounded or documented as current limitations:

### Language & Standards Conformance
- **Imaginary Numeric Literals**: GNU-style imaginary constant suffixes (e.g. `200i`) are not recognized by the lexer; standard C99 `_Complex` arithmetic and initialization expressions must be used.
- **Nested Block Struct Layout Redefinition**: Dynamic layout calculations for tagged `struct`/`union` types redefined with variable-length arrays inside nested block scopes are currently unhandled.

### Target Architecture & Code Generation
- **AArch32 & 32-bit Embedded Gaps**: Core integer/64-bit arithmetic, AAPCS32 calling conventions, VFPv3 scalar floats, `.rodata` `R_ARM_MOVW_ABS_NC`/`R_ARM_MOVT_ABS` relocations, nested-function trampolines, and baremetal ELF32 (`-T`) on AArch32 are verified (`16/16` in `make test-isa-qemu`); open 32-bit/baremetal enhancements are raw-binary variadic register spilling on RV32 and hardware `__attribute__((interrupt))` ISR prologues/epilogues.
- **SIMD / Vector Scope**: Auto-vectorization targets 128-bit SSE2/SSE4.1 (x86-64), NEON (`ldr q`/`str q`, AArch64), and RVV 1.0 (RISC-V, with decoupled `REG_CLASS_VEC` register allocation). Vendor intrinsics headers (`<immintrin.h>`, `<arm_neon.h>`) and wider vector ISA extensions (256-bit AVX/AVX2/AVX-512, ARM SVE/SVE2) are not yet implemented.

### Linker & Toolchain
- **Static Executable Output Only**: ZeroCC's own linker (`--zerocc-link`) produces static `ET_EXEC` ELF binaries. Shared library generation (`-shared`, `ET_DYN`) and linking against dynamic libraries (`.so`) at link time are unsupported. (Runtime dynamic loading is handled via the in-process dynamic loader `dynlink.c`).
- **Linker Script Grammar Subset**: The built-in linker script parser (`src/zero_cc/linker/linker_script.h`) supports `MEMORY` regions (`ORIGIN`, `LENGTH`), `ENTRY`, basic section placement (`.text`, `.rodata`, `.data`, `.bss`), boundary symbols (`_sidata`, `_sdata`, `_edata`, `_sbss`, `_ebss`, `_stack_top`), and simple arithmetic expressions. Advanced GNU `ld` script syntax (`PHDRS`, `SECTIONS` overlays, input section sorting, wildcards) is not supported.
- **Object Format Support**: Linux ELF64 and ELF32 are natively supported. Non-ELF container formats (Windows PE/COFF, macOS Mach-O) are not yet supported in ZeroCC's own linker.

### Freestanding Runtime (`zero_rt`)
- **Minimal Standard Library Surface**: `zero_rt` is a freestanding runtime designed for zero-dependency execution (thread-safe `brk` heap, unbuffered stdio with Ryū float formatting, strings, 128-bit math, complex division, and futex threads). Heavyweight POSIX APIs (signals, robust thread attributes, locale, terminal control, BSD sockets) require building against host musl libc.

### Optimizer & IR
- **Non-SSA Linear IR**: ZeroCC's IR is a linear 172-opcode instruction array backed by a lazy, versioned CFG and dominator tree. Global SSA-dependent transformations (such as Sparse Conditional Constant Propagation or Partial Redundancy Elimination) are not implemented.
- **Conservative Inlining & Unrolling**: Small-function inlining at `-O2`/`-O3` conservatively rejects functions containing floating-point signatures, local variables, address-taken parameters, or labels/gotos. Loop unrolling at `-O3` skips loops with function calls, atomics, volatile accesses, or VLA allocations.

## Future Roadmap

Tracked in [`docs/ZEROCC_Development_Plan.md`](docs/ZEROCC_Development_Plan.md). Key milestones:

- **SIMD Vendor Intrinsics & Wider Vector Targets**: `<immintrin.h>` (x86 SSE/SSE2) and `<arm_neon.h>` vendor intrinsics headers, loop unrolling vs auto-vectorization pass inversion, and wider vector targets (256-bit AVX2 / AArch64 SVE). *(Core NEON 128-bit MC loads/stores and RVV 1.0 decoupled `REG_CLASS_VEC` regalloc are completed in Sprint 191).*
- **Cross-Translation-Unit Link-Time Optimization (LTO) — COMPLETE (Sprint 195)**: Two-phase whole-program LTO (`-flto`) with Fat-LTO `.zc.lto` ELF sections inside `.o`/`.a`, cross-TU inlining (`-O1`+), global constant propagation, 3-round IPCP & pure constant-return fixpoint, dead branch/post-terminator pruning, demand-driven worklist lowering, dead function/global elimination, `--gc-sections` integration, and `--run` JIT LTO. *(Built on Sprint 190's `--gc-sections` dead-stripping).*
- **Platform Portability: Windows & macOS**: PE/COFF + Win64 ABI (`T3-Win`) and Mach-O (`T4-Mac`) object/executable backends and runtime support.
- **Standalone GPU Compiler Repository (`zero_gpu`, PTX & SPIR-V)**: Developed as an independent, standalone project in its own repository (`zero_gpu`), specialized for GPU compute kernels emitting NVIDIA PTX and Khronos SPIR-V, decoupled from ZeroCC's CPU/baremetal pipeline. ZeroCC applications drive GPU acceleration through standard host runtime APIs (`libcuda.so`, `libvulkan.so`, `libOpenCL.so`) via `zero_rt`'s dynamic loader.

## License

ZeroCC is released under the MIT License. See the `LICENSE` file for details.

## Self-Hosting

ZeroCC is fully **self-hosting** and reproducible:

- **Phase 1 — compile simple C programs**: ✅ done
- **Phase 2 — freestanding runtime & self-hosted linking**: ✅ done —
  `zero_rt` is compiled **by zerocc itself** across target ISAs
  (x86-64, AArch64, RISC-V 64-bit, and RISC-V 32-bit), archived with
  `zerocc --ar`, and `--zerocc-link` produces static executables with no host
  libc (see [`docs/okf/zero-rt.md`](docs/okf/zero-rt.md)).
- **Phase 3 — full self-compilation bootstrap & fixed-point convergence**: ✅ done (Sprint 184) —
  ZeroCC compiles all 76 translation units in `src/` (`make audit-self-host`), builds `zerocc-stage1`,
  and compiles `zerocc-stage2` with Stage 1 == Stage 2 **100% bit-for-bit identical** (`make test-self-host`).
- **Phase 4 — own-linker self-hosting**: ✅ done (Sprint 184) —
  ZeroCC's own static linker (`--zerocc-link`) links all 77 compiler objects + libc into a standalone static
  binary with zero host linker dependencies, achieving **100% bit-for-bit identical fixed point**
  (`53,553,224 bytes`, `make test-self-host-ownlink`).

## Acknowledgements
The `gcc.c-torture` conformance corpus is vendored from `gcc-mirror`, the
host static toolchain vendors `musl-1.2.6`, and float formatting in `zero_rt`
uses the vendored Ryū algorithm.
