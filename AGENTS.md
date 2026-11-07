# ZeroCC Agent Instructions & Workspace Rules

ZeroCC is a multi-ISA C99/C11 compiler targeting x86-64, AArch64, and RISC-V with both an in-memory JIT (`--run`) and AOT assembly/ELF backends (`-S`, `-c`, `-o`).

## 1. Knowledge Graph First (`docs/okf/`)

Before scanning the 80+ historical files in `docs/`, **always consult the Open Knowledge Format (OKF v0.2) bundle** at [`docs/okf/index.md`](docs/okf/index.md):

- **Root Navigator**: [`docs/okf/index.md`](docs/okf/index.md)
- **Sequential Evolution Log**: [`docs/okf/log.md`](docs/okf/log.md)
- **Concept Nodes**:
  - [`docs/okf/architecture-pipeline.md`](docs/okf/architecture-pipeline.md) — Pipeline stages & source file map
  - [`docs/okf/frontend-and-types.md`](docs/okf/frontend-and-types.md) — `ZeroCCType` vs `TypeInfo`, structs, function pointers (`is_func_ptr`)
  - [`docs/okf/ir-and-regalloc.md`](docs/okf/ir-and-regalloc.md) — 127-opcode `Instr` IR, `IRCallArgs`, reserved physical registers
  - [`docs/okf/optimization-passes.md`](docs/okf/optimization-passes.md) — Active IR passes (`mem2reg`, `licm`, `auto_vectorize`, etc.) & safety invariants
  - [`docs/okf/abi-sysv-x86_64.md`](docs/okf/abi-sysv-x86_64.md) — System V AMD64 ABI, by-value structs, `sret` (`>16B` struct returns)
  - [`docs/okf/isa-backends.md`](docs/okf/isa-backends.md) — `ZeroCCISABackend` vtable, JIT vs AOT emission, relocations
  - [`docs/okf/build-and-verification.md`](docs/okf/build-and-verification.md) — Build targets, direct compiler testing, and ABI verifiers
  - [`docs/okf/c-torture-gate.md`](docs/okf/c-torture-gate.md) — GCC `c-torture` differential conformance gate (`tools/torture_run.py`, `test/torture/baseline.json`)

## 2. Build & Verification Rules

- **Build Targets**: Use `make all` or `make build/bin/zerocc` (or `make rebuild` when Makefile/headers change). Note that there is no `make build` target.
- **Project-Header Include Convention**: All project headers in `src/` and `include/` are included relative to the include roots (`-Isrc -Iinclude`): `#include "zero_cc/core/core.h"`, `#include "zero_cc.h"`. Do **not** add `../`-parent-relative (`"../types/types.h"`) or bare-sibling (`"ir.h"`) project includes — they are fragile under file moves. System headers keep the angled form.
- **Code Style**: Follow [`docs/coding_style.md`](docs/coding_style.md) — directory layout, include rules & order, naming (`zero_cc_*` / `ZeroCC*` / `ZERO_CC_*`), header-only `static inline` patterns, error-handling shape, and the commit/sprint/OKF documentation discipline.
- **Direct Compiler Verification**: `build/test/abi/*` binaries built by `make all` are compiled with host `$(CC)` (`gcc`), whereas `test/jit/*` runs `./build/bin/zerocc`. Whenever modifying parser, IR, optimization, or ABI codegen, **always verify the test directly with `./build/bin/zerocc` in both AOT and JIT modes**:
  ```bash
  ./build/bin/zerocc path/to/test.c -o /tmp/test_bin && /tmp/test_bin
  ./build/bin/zerocc --run path/to/test.c
  ```
- **Regression Suite & Verifiers**:
  ```bash
  make test
  make test-opt            # optimizer sweep: -O0..-O3, JIT + AOT, via zerocc itself
  make test-san            # zero_san flag-ON soundness sweep: test/san san_ok_*/san_trap_* at -O0..-O3 in JIT + AOT + cross-ISA qemu smoke
  make test-torture        # GCC c-torture conformance gate (ratcheted against test/torture/baseline.json)
  make asan                # host-side ASan/UBSan rebuild of the compiler itself — run after core-pipeline (parser/semantic/IR/optimizer) changes to catch the compiler's own memory bugs before they become user-facing crashes
  python3 tools/sret_verifier.py --verify-all
  python3 tools/type_tag_verifier.py --verify-all
  python3 tools/style_check.py   # C style/convention checker (docs/coding_style.md rules)
  python3 tools/okf.py lint
  ```
- **Diagnostics Battery (`tools/diag_battery.py`)**: differential-tests zerocc's compiler diagnostics against gcc on a corpus of deliberately-broken programs (`test/diag/*.c`), classifying each into an error *category* and ratcheting a per-case baseline (`test/diag/baseline.json`): a run regresses if it loses a category, drops an error count, or makes a previously-rejected file compile silently. Run after any parser/semantic/diagnostic change:
  ```bash
  python3 tools/diag_battery.py                # gate on baseline
  python3 tools/diag_battery.py --update-baseline  # re-record after an intentional improvement (e.g. multi-error recovery)
  ```
- **Type-Tag Verifier (`tools/type_tag_verifier.py`)**: cross-checks frontend semantic type tags against backend IR conventions (C99 width marks, OP_OVF_TRAP check width and adjacency, FP value ops, 128-bit operations, in-place extends, COND_SET, unsigned DIV/MOD), reports the I9 unmarked-narrow-op population (Sprint 119 tag-driven codegen: OVF-deferred / pre-canonicalized / load-store-discipline), and checks I10 (Sprint 120 — complex/vector address-form temps are never the dest of a scalar FP/128 value op, the 115l esz / 113 vector-lane bug class) across 152 corpus files compiled at `-O0`, `-O2`, `-O3`:
  ```bash
  python3 tools/type_tag_verifier.py --verify-all
  ```
- **Cross-ISA Raw-Emitter QEMU Gate (`make test-isa-qemu`)**: differential-tests **every** target ISA's *raw* binary emitter under QEMU (`test/isa_qemu/` battery; raw path via `ZC_RAW_AOT=1`, diffed against a **static-musl** reference, both run under `qemu-*-static`). **Dependency:** `qemu-user-static` + a per-ISA static-musl reference wrapper (`musl-gcc-<arch>`) built by `tools/setup_musl_cross.sh` (uniform musl-static baseline across all 4 ISAs; riscv32 also gets a small rv32 libgcc shim) — the harness preflights these and prints the setup command. **Status:** **All 4 target ISAs (44/44)** are fully green: **x86-64 (11/11)**, **aarch64 (11/11)**, **riscv64 (11/11)**, and **riscv32 (11/11)** pass cleanly under QEMU. Run one ISA: `python3 tools/isa_qemu_diff.py --isa riscv32`.
- **Cross-ISA `__int128` Gate (`make test-isa128`)**: differential-tests the 128-bit (lo,hi) GPR128 emitters and the `__float128` object model — every `test/jit/int128_*.c` test (arith, return-ABI, type/sizeof, argument ABI, function-pointer signatures, float128) compiled at `-O0..-O3` in **both** AOT paths (raw-binary link and `-S` + cross-assembler) for x86-64 (native), aarch64, and riscv64, run under `qemu-*-static` and diffed against a host `gcc -static -O2` reference. Guards the cross-ISA-only 128-bit bugs (large-offset scratch clobbers, multiword borrow ordering, spilled MUL128, call-return high-half protection, the Sprint 110 two-unit **argument** ABI incl. register/stack straddle, the Sprint 111 128-bit function-pointer signatures + O2 inliner high-half loss, and the Sprint 112 `__float128` type/ABI + >64-call FP-arg MC bug) that the x86-64-only `make test-opt` cannot see. **Dependency:** `qemu-{aarch64,riscv64}-static` + `aarch64-linux-gnu-gcc`/`riscv64-linux-gnu-gcc` (same toolchains as `test-isa-qemu`; no musl wrappers). **Status:** **144/144** runs green (6 tests). Subset: `python3 tools/isa128_diff.py --isa aarch64 --levels 0 2 --paths asm`.
- **Optimizer Changes Require the Sweep**: `make test` compiles test binaries with the host `gcc`, so it does **not** exercise zerocc's optimizer. Any change to an optimization pass, LICM, mem2reg, DCE, CSE/GVN, copy propagation, or register allocation **must** pass `make test-opt` (which runs every test at `-O0/-O1/-O2/-O3` in both JIT and AOT and compares against the `-O0` baseline). A `-O1` pass is not sufficient: later passes (CSE/GVN/LICM at `-O2`, strength reduction at `-O3`) can transform `-O1` output into something none of the earlier passes saw.
- **Long gates: background them** (the multi-minute outlier — the full c-torture sweep — must not block the loop; each agent command runs in an ephemeral shell, so the work must be detached from it). **Use tmux only if it exists on the host** (`command -v tmux`): `tmux kill-session -t torture 2>/dev/null; tmux new-session -d -s torture "make test-torture 2>&1 | tail -40 > /tmp/gate.log; echo DONE >> /tmp/gate.log"`. **Without tmux**, the same discipline with a plain background job: `nohup bash -c 'make test-torture 2>&1 | tail -40 > /tmp/gate.log; echo DONE >> /tmp/gate.log' >/dev/null 2>&1 &`. Either way: poll the log file and `test/torture/last_run.json` mtime; the `DONE` sentinel (not the pane) is the completion signal. Two hard rules, independent of the supervisor: **(1) never rebuild `build/bin/zerocc` while the gate runs** — the running gate execs the binary and dies with `PermissionError` on the replaced file; finish/kill the gate, rebuild, relaunch. **(2) interpret results by diffing `test/torture/last_run.json` against `test/torture/baseline.json`** (per-test class + per-config status), never by reading the truncated log tail. Everything shorter than a couple of minutes runs foreground with a timeout. Tear down the session (`tmux kill-session`) when done.

## 3. Maintaining the OKF Knowledge Bundle

When completing a feature, bug fix, or architectural change:
1. Update the relevant concept file(s) in `docs/okf/` (or add a new OKF v0.2 single-concept `.md` file linked from `docs/okf/index.md`).
2. Run `python3 tools/okf.py update <page.md> --log "Summary"` to bump timestamps and append to `docs/okf/log.md` (and update `docs/CHANGELOG.md`).
3. Run `python3 tools/okf.py lint && python3 tools/okf.py index` to verify YAML frontmatter schemas, relative markdown links, and graph reachability.
4. To bootstrap the OKF skill and rules in a new repo using this portable script: `python3 tools/okf.py setup --repo /path/to/repo --copy-script`.
