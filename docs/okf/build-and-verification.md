---
type: Guide
title: Build, Test & Verification Guide for Agents
description: Practical commands and rules for building ZeroCC, running the 140-test suite, the optimizer sweep, C99/C11 verifiers, testing JIT vs AOT compilation, and running ABI/OKF verifiers.
resource: workflows/build-and-verification
tags: [build, testing, verification, makefile, sret, c11, vla, okf, opt-sweep]
timestamp: 2026-10-20T19:06:09Z
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

## 2. Running the Test Suite

```bash
# Run all 140 tests (unit, semantic, abi, jit)
make test

# Run specific category
make test-abi
make test-jit
make test-semantic
make test-unit
```

### Optimizer regression sweep (`make test-opt`)

The normal suite compiles the `test/abi`/`test/semantic`/`test/unit` binaries with the host `gcc`, so it **never** exercises zerocc's optimization pipeline. `make test-opt` closes that gap with `tools/opt_sweep.py`: it compiles and runs every standalone test at `-O0/-O1/-O2/-O3` **using `./build/bin/zerocc` itself**, in **both JIT (`--run`) and AOT (`-o`)** modes, and compares each level's `(exit code, stdout)` against the `-O0` baseline. It also (informationally) diffs against `gcc -O2` when the host compiler can build the test.

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
# Optimizer regression sweep: -O0..-O3 x {JIT, AOT} via zerocc itself (105 tests)
make test-opt

# Verify the CFG layer (-dump-cfg) against an independent Python recomputation
# of blocks/loops from -dump-ir: tiling, jump targets, pred/succ consistency,
# reachability, C-vs-Python loop-set equality. Skips non-standalone tests.
python3 tools/cfg_verifier.py --verify-all

# Dump a function's CFG (stable plain text: blocks, succ/pred, loop headers)
./build/bin/zerocc -dump-cfg -O0 test/cfg/nested_loops.c -o /tmp/x

# Run C11 feature & concurrency verification suite (11/11 tests: _Alignas, _Alignof, anonymous structs/unions, _Generic, _Static_assert, atomics, <threads.h>)
python3 tools/c11/test_c11.py --verbose

# Run C99 VLA & multi-dimensional matrix VLA verification suite
python3 tools/vla/test_vla.py

# Verify System V AMD64 sret (>16B struct return) callee & caller assembly patterns (6/6 checks)
python3 tools/sret_verifier.py --verify-all

# Cross-verify sret (>16B struct return) execution across x86-64, AArch64, and RISC-V under QEMU
python3 tools/sret_verifier.py --target-all
# NOTE (Sprint 41): all three targets PASS. The aarch64 `-S` emitter used to emit
# `sxtw xD, xD` / `uxtw xD, xD` (the source must be a W register: `sxtw xD, wS`);
# aarch64_asm.c `aarch64_emit_extend_asm` now emits `x<dst>, w<src>` for the
# width-4 (sxtw/uxtw) case.
# KNOWN (separate, untested): the aarch64 BINARY (JIT/AOT object) path is still
# broken - `aarch64_emit_extend` (aarch64.c) uses wrong base encodings, and
# `--target aarch64 -o` fails to link (emits invalid `-mabi=lp64d`). The `-S` path
# (what this check uses) is unaffected.

# Verify struct ABI classification and register usage
python3 tools/struct_abi_verifier.py --test test/abi/struct_return_20byte.c

# Validate the docs/okf/ Open Knowledge Format bundle (YAML frontmatter & graph links)
python3 tools/okf.py lint
python3 tools/okf.py index
```

## Related Concepts
- Return to [Root Index](./index.md)
- See [Bundle Log](./log.md)
- See [System V AMD64 ABI & Struct Calling Conventions](./abi-sysv-x86_64.md)
- See [Compilation Pipeline & Two-Pass Architecture](./architecture-pipeline.md)

