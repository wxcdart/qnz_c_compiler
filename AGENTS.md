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

## 2. Build & Verification Rules

- **Build Targets**: Use `make all` or `make build/bin/zerocc` (or `make rebuild` when Makefile/headers change). Note that there is no `make build` target.
- **Direct Compiler Verification**: `build/test/abi/*` binaries built by `make all` are compiled with host `$(CC)` (`gcc`), whereas `test/jit/*` runs `./build/bin/zerocc`. Whenever modifying parser, IR, optimization, or ABI codegen, **always verify the test directly with `./build/bin/zerocc` in both AOT and JIT modes**:
  ```bash
  ./build/bin/zerocc path/to/test.c -o /tmp/test_bin && /tmp/test_bin
  ./build/bin/zerocc --run path/to/test.c
  ```
- **Regression Suite & Verifiers**:
  ```bash
  make test
  make test-opt            # optimizer sweep: -O0..-O3, JIT + AOT, via zerocc itself
  python3 tools/sret_verifier.py --verify-all
  python3 tools/okf.py lint
  ```
- **Cross-ISA Raw-Emitter QEMU Gate (`make test-isa-qemu`)**: differential-tests **every** target ISA's *raw* binary emitter under QEMU (`test/isa_qemu/` battery; raw path via `ZC_RAW_AOT=1`, diffed against a **static-musl** reference, both run under `qemu-*-static`). **Dependency:** `qemu-user-static` + a per-ISA static-musl reference wrapper (`musl-gcc-<arch>`) built by `tools/setup_musl_cross.sh` (uniform musl-static baseline across all 4 ISAs; riscv32 also gets a small rv32 libgcc shim) — the harness preflights these and prints the setup command. **Status:** **x86-64 (11/11)** and **riscv64 (11/11)** are fully green; aarch64 (1/11) and riscv32 (0/11) remain in progress. It reports `Error 1` until all raw emitters are repaired. Run one ISA: `python3 tools/isa_qemu_diff.py --isa riscv64`.
- **Optimizer Changes Require the Sweep**: `make test` compiles test binaries with the host `gcc`, so it does **not** exercise zerocc's optimizer. Any change to an optimization pass, LICM, mem2reg, DCE, CSE/GVN, copy propagation, or register allocation **must** pass `make test-opt` (which runs every test at `-O0/-O1/-O2/-O3` in both JIT and AOT and compares against the `-O0` baseline). A `-O1` pass is not sufficient: later passes (CSE/GVN/LICM at `-O2`, strength reduction at `-O3`) can transform `-O1` output into something none of the earlier passes saw.

## 3. Maintaining the OKF Knowledge Bundle

When completing a feature, bug fix, or architectural change:
1. Update the relevant concept file(s) in `docs/okf/` (or add a new OKF v0.2 single-concept `.md` file linked from `docs/okf/index.md`).
2. Run `python3 tools/okf.py update <page.md> --log "Summary"` to bump timestamps and append to `docs/okf/log.md` (and update `docs/CHANGELOG.md`).
3. Run `python3 tools/okf.py lint && python3 tools/okf.py index` to verify YAML frontmatter schemas, relative markdown links, and graph reachability.
4. To bootstrap the OKF skill and rules in a new repo using this portable script: `python3 tools/okf.py setup --repo /path/to/repo --copy-script`.
