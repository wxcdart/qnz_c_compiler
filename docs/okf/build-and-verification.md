---
type: Guide
title: Build, Test & Verification Guide for Agents
description: Practical commands and rules for building ZeroCC, running the 165-test suite, the optimizer sweep, C99/C11 verifiers, testing JIT vs AOT compilation, and running ABI/OKF verifiers.
resource: workflows/build-and-verification
tags: [build, testing, verification, makefile, sret, c11, vla, okf, opt-sweep]
timestamp: 2026-11-09T00:20:03Z
generated: antigravity-agent
verified: engineering-lead
status: active
---

# Build, Test & Verification Guide for Agents

Follow these workflows whenever modifying ZeroCC.

## 1. Building ZeroCC

> **Note**: The Makefile target is `make all` or `make build/bin/zerocc` (there is no `make build` target). Incremental builds track header dependencies automatically via `-MMD -MP`.

```bash
# Build compiler binary and all test binaries
make all

# Build only the ZeroCC compiler binary
make build/bin/zerocc

# Full clean rebuild (use when changing Makefile or switching branches)
make rebuild
```

### Stale-build guard

`main()` (src/main.c) runs a layout check before anything else: `zero_cc_ast_node_size() != sizeof(ZeroCCASTNode)`. `zero_cc_ast_node_size` is declared in `src/zero_cc/parser/parser.h` and defined in `src/zero_cc/parser/parser.c`; it returns the `ZeroCCASTNode` size compiled into the `parser` object file, which `main` compares against its own `sizeof`. If they differ, an object file was built against an older AST-node layout and would corrupt the AST at runtime, so the binary aborts with a "stale build objects" message. **Implication: any change to the `ZeroCCASTNode` struct layout (adding/removing/reordering fields) requires a full `make clean && make build/bin/zerocc`** — an incremental build that recompiles only some objects leaves the mismatch in place.

### Project-header include convention

All project headers are included **relative to the include roots `-Isrc -Iinclude`** (the single include-root convention, normalized in Sprint 42): `src/zero_cc/core/core.h` is included as `#include "zero_cc/core/core.h"`, and `include/zero_cc.h` as `#include "zero_cc.h"`. Do **not** use `../`-parent-relative (`"../types/types.h"`) or bare-sibling (`"ir.h"`) forms for project headers — they are fragile under file moves. System/external headers keep the angled form (`<stdio.h>`). Two dormant freestanding placeholders are exempt: `"zero_elf.h"` (elf_emitter.h `#else` branch) and `"zero_posix_baremetal.h"` (posix_libc.h, `ZERO_NOLIBC`/`ZERO_FREESTANDING` guard) — both reference headers not yet created.

- **Include order**: prescribed for new/touched files — sibling header first, then system (`<...>`), then project ("zero_cc/..."). The existing tree is mixed (≈half the files lead with project headers); untouched files are not churned.
- **Why `include/` is flat (design decision)**: the common `include/<project>/` subdirectory pattern is deliberately **not** used. `threads.h` emulates the *system* `<threads.h>` and must resolve as a bare root name for test binaries compiled with host gcc + `-Iinclude`; `zero_cc.h` is a single-file umbrella API, and the collision risk the subdirectory pattern guards against only materializes when multiple projects share an include path (this tree does not). **Revisit trigger:** a second public header family (e.g. the `zero_rt` runtime API) → migrate to `include/zero_cc/` + `include/zero_rt/` in one step, keeping a `zero_cc.h` shim for compatibility. Full style reference: `docs/coding_style.md`.

### Developer Tooling & Verification Prerequisites

| Category | Tools & Minimum Versions | Role in ZeroCC Verification |
|----------|--------------------------|-----------------------------|
| **Core Build** | `make` (4.0+), Host `gcc` (13.0+), `python3` (3.8+), `git` | Building compiler binaries, `zero_rt` runtimes, test harnesses, and running test orchestration scripts. |
| **Dialect Conformance** | `gcc-15` (15.0+), `gcc-14` / `gcc-13` | Differential verification across dialects (`-std=gnu23`, `c23`, `c11`, `c99`, `gnu89`) via `make test-c23` (`tools/c23/test_c23.py`) and diagnostic tests (`tools/diag_battery.py`). Overridable via `C23_GCC`. |
| **Cross-ISA Emulation** | `qemu-user-static` (8.0+), `aarch64-linux-gnu-gcc`, `riscv64-linux-gnu-gcc` | User-mode emulation under Linux for cross-ISA verification (`make test-isa128`). |
| **Static Musl Reference** | `musl-gcc-<arch>` (in `~/.local/bin`, via `tools/setup_musl_cross.sh`) | Fully static reference binary generation for raw emitter differential testing (`make test-isa-qemu`). |
| **Workflow / Supervision** | `tmux` | Detaching and supervising long-running gates (such as `make test-torture`) without agent/shell command timeout. |
| **Inspection & Profiling** | `gdb` (15+), `valgrind`, `perf` | JIT machine-code disassembly, stack-frame inspection, and `--profile-opts` pass timing. |

## 2. Running the Test Suite

```bash
# Run all 186 tests (13 unit, 17 semantic, 38 abi, 118 jit)
make test

# Run specific category
make test-abi
make test-jit
make test-semantic
make test-unit

# ASan+UBSan build of the semantic harnesses (LeakSanitizer-clean;
# the harnesses tear down via zero_cc_compiler_free, see types/symbols.h)
make asan-semantic   # 17/17 as of Sprint 100

# The full suite under ASan+UBSan+LeakSanitizer.  Green (180/180) as of
# Sprint 122, which closed the last leak classes the instrumented driver
# exposed: instance-teardown frees (temp_type_tags / fn_hash / tls section /
# driver include-dirs + preprocessor defines), pool-resident fn_params
# arrays (type-pool-owned, freed at end-of-compile), the struct/union
# member-array ownership registry (recorded per-compile by
# zero_cc_free_all_struct_arrays, released once in zero_cc_compiler_free -
# the structs/typedefs tables persist across compiles, so per-compile frees
# were wrong in both directions: dangle across compiles, or leak an array
# retired by a redefinition overwrite), and the LICM per-loop work tables
# (opt_loop.c loop_analysis_free).  Every in-process compile is destroyed
# via zero_cc_compiler_free, so this gate also catches new per-compile leaks.
make asan
```

### Optimizer regression sweep (`make test-opt`)

The normal suite compiles the `test/abi`/`test/semantic`/`test/unit` binaries with the host `gcc`, so it **never** exercises zerocc's optimization pipeline. `make test-opt` closes that gap with `tools/opt_sweep.py`: it compiles and runs every standalone test at `-O0/-O1/-O2/-O3` **using `./build/bin/zerocc` itself**, in **both JIT (`--run`) and AOT (`-o`)** modes, and compares each level's `(exit code, stdout)` against the `-O0` baseline. It also (informationally) diffs against `gcc -O2` when the host compiler can build the test. **Baseline probe (Sprint 134):** a non-zero baseline rc with a *building* gcc reference triggers an explicit zerocc compile probe — `zerocc --run` on an uncompilable source exits rc=1 like any process, so pre-Sprint 134 a persistent compile failure was adopted as the stable baseline and the test reported `[OK]` (how the stale `test/jit/c23_features.c` hid for ~50 sprints). Intentional non-zero program exits (successful compile) remain legitimate. **Sweep integrity (Sprint 138):** two further hollow-baseline sources were found while verifying the c-torture strictness burn-down. (1) The sweep's zerocc and gcc compile commands did not pass `-Iinclude -Isrc`, so the 58 **embedding tests** (those including `zero_cc.h` to drive the compiler API) *failed to compile* — their "baseline" was a masqueraded compile failure and every `-O` level "matched" it without running anything; the sweep now passes the include paths. (2) The embedding tests still cannot link standalone (they call zerocc internals like `arena_alloc` that only the zerocc library/process provides), so the sweep now **explicitly skips them** with a documented reason — their real coverage is `make test` (host-gcc build linked against the zerocc library). Post-fix sweep state: the ~93 standalone tests are swept in all applicable modes with **zero** "AOT baseline unavailable" / "gcc reference unavailable" skips. (The same investigation completed the virtual `<elf.h>` — it lacked `EM_RISCV`, `EM_386`, `STB_LOCAL`, `STB_WEAK`, `SHN_UNDEF`, which the embedder headers in `zero_cc.h` use.)

```bash
# Full optimizer sweep (the definitive gate for any optimizer/pass change)
make test-opt

# Targeted runs
python3 tools/opt_sweep.py --only break_continue
python3 tools/opt_sweep.py --jit-only
python3 tools/opt_sweep.py --aot-only --no-gcc
python3 tools/opt_sweep.py --timeout 20 --json > /tmp/sweep.json
```

Any change to an optimization pass, LICM, mem2reg, DCE, CSE/GVN, copy propagation, or register allocation **must** pass `make test-opt`. A `-O1`-only pass is not sufficient: later passes at `-O2`/`-O3` transform `-O1` output into code none of the earlier passes saw. Tests that are timing- or nondeterminism-sensitive are flagged `NONDET`/`skipped` rather than hard-failed.

### zero_san regression sweep (`make test-san`)

`make test` builds test binaries with the host `gcc` and `make test-opt` runs without the sanitizer flag, so neither exercises `-fsanitize=undefined` at all. `make test-san` (`tools/san_sweep.py`, `test/san/`) is the flag-ON soundness gate for the zero_san v1 signed-overflow check (see [zero_san roadmap](./sanitizer-roadmap.md) and [Two-Pass IR & Register Allocation](./ir-and-regalloc.md), `OP_OVF_TRAP`):

- `san_ok_*.c` — valid C that must **never** false-trap: run with `-fsanitize=undefined` at `-O0..-O3` in both JIT (`--run`) and AOT (`-o`), expecting exit 0 — and also run **without** the flag to prove the instrumentation is behavior-preserving on defined code. Covers mixed-width usual arithmetic conversions (`int + long` must not check at 32-bit), unsigned wrap, 64-bit values beyond the 16-bit range, register-pressure/spill cases, loops, and CSE-shaped duplicate adds.
- `san_trap_*.c` — a real signed overflow that must **SIGILL (rc 132)** at every `-O` level in both modes — guards the pass interaction (a hoisted/dropped/orphaned check would let the program exit 0 instead).
- Cross-ISA smoke: `san_ok_cross_smoke.c` compiled with the flag for aarch64/riscv64/riscv32 (raw-AOT default object path) and run under `qemu-*-static` (skipped, not failed, when qemu is missing) — guards the invariant that x86 check bytes never leak into other ISAs' binaries.

```bash
make test-san
python3 tools/san_sweep.py --no-cross --json > /tmp/san.json
```

Any change to `OP_OVF_TRAP` lowering/emission, the width-extension post-passes, or the extend emitters **must** pass `make test-san` in addition to the usual gates (the c-torture gate and `make test-opt` run without the flag, so a flag-only miscompile is invisible to them).

## 3. Verifying Individual C Files with ZeroCC Directly

Note that `build/test/abi/*` binaries built by `make all` are compiled by the host `$(CC)` (`gcc`), whereas `test/jit/*` and `tools/sret_verifier.py` invoke `./build/bin/zerocc`. **Always test new ABI features directly with `./build/bin/zerocc` in both AOT and JIT modes**:

```bash
# 1. AOT Executable mode (-o)
./build/bin/zerocc test/abi/struct_return_funcptr.c -o /tmp/test_bin && /tmp/test_bin

# 2. JIT Execution mode (--run)
./build/bin/zerocc --run test/abi/struct_return_funcptr.c

# 3. Inspect generated assembly (-S) or IR (-dump-ir)
./build/bin/zerocc -S test/abi/struct_return_funcptr.c -o /tmp/test.s
```

## 4. Automated C99/C11, ABI & OKF Verification Tools (`tools/`)

Active verification suites live at the top level of `tools/` (historical one-off debugging scripts are archived in `tools/archive/`):

```bash
# Optimizer regression sweep: -O0..-O3 x {JIT, AOT} via zerocc itself (126 tests)
make test-opt

# zero_san (-fsanitize=undefined) flag-ON soundness sweep: test/san san_ok_*/san_trap_*
# at -O0..-O3 x {JIT, AOT} + cross-ISA qemu smoke (see the test-san section above)
make test-san

# Type-tag verifier (Sprint 118 Slice 1 + 119 Slice 2 + 120 Slice 3): cross-checks frontend
# semantic type tags against lowered IR side-channel width marks, OP_OVF_TRAP invariants,
# FP/128-bit ops, extend ops, COND_SET, and unsigned division; reports the I9 unmarked-
# narrow-op population (OVF-deferred / pre-canonicalized / load-store-discipline) and checks
# I10 (complex/vector address-form temps are never the dest of a scalar FP/128 value op -
# the Sprint 115l esz / 113 vector-lane bug class) across 160 corpus files at -O0/2/3
python3 tools/type_tag_verifier.py --verify-all

# Verify the CFG layer (-dump-cfg) against an independent Python recomputation
# of blocks/loops from -dump-ir: tiling, jump targets, pred/succ consistency,
# reachability, C-vs-Python loop-set equality. Skips non-standalone tests.
python3 tools/cfg_verifier.py --verify-all

# Dump a function's CFG (stable plain text: blocks, succ/pred, loop headers)
./build/bin/zerocc -dump-cfg -O0 test/cfg/nested_loops.c -o /tmp/x

# Run C11 feature & concurrency verification suite (11/11 tests: _Alignas, _Alignof, anonymous structs/unions, _Generic, _Static_assert, atomics, <threads.h>)
python3 tools/c11/test_c11.py --verbose

# Run C23 differential conformance gate (Sprint 134): feature-tagged C23 cases
# (std_version, true/false, constexpr, nullptr, designator sizing, #warning,
# c23_misc) compiled & run under zerocc at -O0..-O3 in JIT + AOT, diffed
# against a reference gcc (C23_GCC override, else gcc-15/14/gcc) with the
# matching -std flag; negative cases check accept/reject agreement.
# Wired as `make test-c23`.
python3 tools/c23/test_c23.py --all

# Run the inline-assembly Phase 1 differential gate (Sprint 135): every
# test/asm/*.c compiled under zerocc at -O0..-O3 on the AOT TEXT path (-S),
# assembled + linked with the host gcc, run, and diffed (rc + stdout)
# against a host-`gcc -O2` reference. Guards the OP_ASM lowering, the
# IRAsmArgs side table (incl. the arena-rebuild/DCE-remap invariant), the
# no_spill LRA, clobber spill/forbidden-mask, and template interpolation.
# Wired as `make test-asm`.
python3 tools/asm_diff.py

# Run C99 VLA & multi-dimensional matrix VLA verification suite
python3 tools/vla/test_vla.py

# Verify System V AMD64 sret (>16B struct return) callee & caller assembly patterns (6/6 checks)
python3 tools/sret_verifier.py --verify-all

# Verify prologue/epilogue emission: selective CSR omission, fallthrough jump
# elimination, Red Zone, frameless leaves, and omit-frame-pointer shapes
# (balanced SP adjustments, SP-based CSR symmetry) on all 4 target ISAs
python3 tools/prologue_epilogue_verifier.py

# Own-linker (T2) differential battery: link zerocc objects + .a archives with
# --zerocc-link into a static ET_EXEC, run it, and compare against a
# gcc -no-pie link of the same objects (21/21 as of Sprint 100)
make test-linker

# C style/convention checker (ruff-style; codifies docs/coding_style.md):
# include conventions, tab-free indentation, EOF newline, header guards,
# <module>_<action> public-function prefix. Errors gate, warnings report.
# --fix repairs trailing whitespace + missing EOF newline in place.
python3 tools/style_check.py

# Diagnostics battery: differential-tests zerocc's compiler diagnostics
# against gcc on the broken-program corpus in test/diag/*.c. Classifies each
# diagnostic into an error category and ratchets test/diag/baseline.json
# (regression = lost category, dropped error count, or a previously-rejected
# file compiling silently). Run after parser/semantic/diagnostic changes;
# --update-baseline re-records after an intentional improvement.
python3 tools/diag_battery.py

# Cross-verify sret (>16B struct return) execution across x86-64, AArch64, and RISC-V under QEMU
python3 tools/sret_verifier.py --target-all
# NOTE (Sprint 41): all three targets PASS. The aarch64 `-S` emitter used to emit
# `sxtw xD, xD` / `uxtw xD, xD` (the source must be a W register: `sxtw xD, wS`);
# aarch64_asm.c `aarch64_emit_extend_asm` now emits `x<dst>, w<src>` for the
# width-4 (sxtw/uxtw) case.
# Cross-target output defaults to direct RAW binary object emission via
# elf_writer (ELF64/ELF32), with ZC_TEXT_AOT=1 available as a text escape hatch.
# All 4 target ISAs pass 59/59 cleanly under QEMU (see isa-backends.md).

# Differential-test EVERY target ISA's RAW binary emitter under QEMU (make target below).
make test-isa-qemu

# Cross-ISA __int128 gate: test/jit/int128_*.c x {x86-64, aarch64, riscv64} x
# -O0..-O3 x {raw AOT, -S + cross-as} under QEMU, diffed vs gcc -static -O2.
make test-isa128

### Cross-ISA Raw-Binary-Emitter Differential Test (`make test-isa-qemu`)

`tools/isa_qemu_diff.py` is the differential harness that was missing for the
**raw** binary emitters (the `-o`/`-c` AOT paths route aarch64/riscv through the
*text* emitter, so only x86-64's raw path was ever exercised). For each test in
`test/isa_qemu/` and each ISA it: builds a static executable via zerocc's **raw
binary path** (`ZC_RAW_AOT=1` forces raw for cross-targets; x86-64 is raw by
default), builds a reference with a **static-musl** cross toolchain
(`musl-gcc-<arch>`, from `tools/setup_musl_cross.sh`), runs **both** under the
matching `qemu-*-static`, and diffs stdout + exit code.

**Dependency (required, preflighted):** `qemu-user-static`
(`qemu-x86_64-static`, `qemu-aarch64-static`, `qemu-riscv64-static`,
`qemu-riscv32-static`) **and** a per-ISA static-musl reference wrapper
(`musl-gcc-<arch>`) installed by `tools/setup_musl_cross.sh` (which builds static
musl with the system cross gcc — `aarch64-linux-gnu-gcc`,
`riscv64-linux-gnu-gcc`, host `gcc` — and, for riscv32, a small rv32 libgcc shim).
A uniform musl-static reference is used across all 4 ISAs so the differential
baseline is consistent. The harness lists exactly what is missing if absent and
prints the `setup_musl_cross.sh` command to build each reference.

**Current state** (raw emitters): **All 4 target ISAs (59/59 runs)** pass cleanly with 0 failures, 0 timeouts, and 0 mismatches: **x86-64 15/15**, **aarch64 15/15**, **riscv64 15/15**, and **riscv32 14/14** (15 tests in `test/isa_qemu/`; `15_nested_tramp.c` is `isa-skip: riscv32` because RISC-V32 rejects address-taking of capturing nested functions by design). For aarch64, stack frame setup, relocation pairing (`ADR_PREL_PG_HI21`/`ADD_ABS_LO12_NC`), byte/halfword stores, variable shifts, and FP instruction encodings are green. For riscv32, true ELF32 object generation, frame setup, shift/extension width fixups, and struct return registers are green. Run a single ISA with `python3 tools/isa_qemu_diff.py --isa <arch>`.

### Cross-ISA __int128 Differential Gate (`make test-isa128`)

`tools/isa128_diff.py` is the differential guard for the 128-bit (lo,hi) GPR128
value model and the `__float128` object model. For every `test/jit/int128_*.c` test (arith,
return-ABI, arg-ABI, fnptr-signatures, float128, type/
sizeof) and each ISA (`x86-64` native, `aarch64`, `riscv64`) it compiles at
`-O0..-O3` in **both** AOT paths — the raw-binary link and `-S` +
cross-assembler + static link — runs under `qemu-*-static` (x86-64 runs
native), and diffs `(stdout, exit)` against a host `gcc -static -O2`
reference (the reference itself is the ground truth: a source whose expected
constants are wrong fails the gate as REF-FAIL). Every Sprint 109 128-bit
regression — the RISC-V `t5`/AArch64 `x16` large-offset scratch clobbers,
the multiword borrow `sltu`/carry ordering, all-spilled and mixed MUL128, the
AArch64 mul-before-umulh ordering, and the 128-bit call-return high-half
protection — was cross-ISA only and invisible to `make test-opt` (x86-64
only). Sprint 110's `__int128` **argument** ABI (two-unit register pairs,
caller-stack overflow, register/stack straddle), Sprint 111's 128-bit
**function-pointer signatures** + O2 inliner high-half fixes, and Sprint 112's
`__float128` type/ABI (plus the >64-call FP-arg MC-path bug the float128 test's
143-call `main` exercises) are guarded here too. Current state:
**144/144 runs pass** (6 tests x 3 ISAs x 4 levels x 2
paths).

**Dependency (required, preflighted):** `build/bin/zerocc`, host `gcc`
(reference), `qemu-aarch64-static`/`qemu-riscv64-static`, and the
cross compilers `aarch64-linux-gnu-gcc`/`riscv64-linux-gnu-gcc` (same
toolchains as `test-isa-qemu`; no musl wrappers needed). Run a subset with
`python3 tools/isa128_diff.py --isa aarch64 riscv64 --levels 0 2 --paths asm`.

# Verify struct ABI classification and register usage
python3 tools/struct_abi_verifier.py --test test/abi/struct_return_20byte.c

# RISC-V raw binary emitter encoding oracle (T1: spec round-trip + celebi independent oracle)
python3 tools/riscv_enc_check.py --zerocc

# RISC-V ELF relocation pairing & section symbol verifier (T2: HI20/LO12 pairs + section addends)
python3 tools/reloc_pair_check.py --zerocc

# Multi-ISA backend vtable slot auditor (T3: ZeroCCISABackend conformance vs active BE(c)-> calls)
python3 tools/vtable_check.py --diff riscv64 riscv32

# Validate the docs/okf/ Open Knowledge Format bundle (YAML frontmatter & graph links)
python3 tools/okf.py lint
python3 tools/okf.py index

# GCC c-torture conformance gate (3,940 vendored tests vs gcc reference,
# ratcheting test/torture/baseline.json; see c-torture-gate.md)
make test-torture
python3 tools/torture_run.py --quick

# Project-differential gate (Verification & Scale #1): real-world TUs from
# test/projects/ (cJSON, zlib, the sqlite3.c amalgamation) must compile
# under zerocc at -O0/-O2, differential against host gcc acceptance
# (54 TUs at -O0/-O2/-O3 as of Sprint 124).
make test-project-diff
python3 tools/project_diff.py --levels 0 2 3

# Project run + behavior-diff gate (Verification & Scale #1, phases 2/3):
# builds each project's own test program with BOTH host gcc and zerocc,
# runs both on identical inputs, diffs stdout/stderr/exit-code (+ produced
# file bytes). cjson (demo + 19 Unity suites) and zlib (example + minigzip
# round-trip) match gcc AOT; cjson also passes under --jit. The sqlite
# on-disk SQL suite is SKIPPED (not failed) while a pre-existing pager
# write-path fd-corruption bug is open.
make test-project-runs
python3 tools/project_runs.py
python3 tools/project_runs.py --jit
```

## Related Concepts
- Return to [Root Index](./index.md)
- See [Bundle Log](./log.md)
- See [Frontend, Type System & Semantic Analysis](./frontend-and-types.md) (fnptr member star topology, t2ti memo)
- See [GCC c-torture Conformance Gate](./c-torture-gate.md)
- See [System V AMD64 ABI & Struct Calling Conventions](./abi-sysv-x86_64.md)
- See [Compilation Pipeline & Two-Pass Architecture](./architecture-pipeline.md)

