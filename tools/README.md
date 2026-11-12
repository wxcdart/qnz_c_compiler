# ZeroCC Verification & Tooling Suite

This directory contains the active verification suites, test runners, and knowledge-graph tools for ZeroCC. Historical one-off debugging scripts and migration helpers are preserved in [`tools/archive/`](./archive/).

All tools are **stdlib-only** (see [`tools/requirements.txt`](./requirements.txt)) — run them with `python3 tools/<subdir>/<script>.py` from the repository root. No virtualenv or third-party packages are required.

## Layout

| Subpackage | Purpose |
| :--- | :--- |
| [`tools/verify/`](./verify/) | Regression gates: optimizer sweep, zero_san, c-torture, diagnostics battery, test runner, driver/linker tests, style check, type-tag verifier, sret verifier, ISA QEMU diffs, `__int128` diff, and more |
| [`tools/analysis/`](./analysis/) | Codegen inspection: ELF verification, RISC-V encoding/relocation checks, vtable auditing, struct ABI/return analysis, SIMD regression, literal verifier, QEMU test runner |
| [`tools/okf/`](./okf/) | Open Knowledge Format (OKF v0.2) linter, search, index, and updater |
| [`tools/toolchain/`](./toolchain/) | Toolchain setup: static-musl cross builds, embed_symtab generator, dev environment setup |
| [`tools/suites/`](./suites/) | Test suite generators (e.g. struct test generator) |
| [`tools/c11/`](./c11/) | C11 feature & concurrency verification suite |
| [`tools/c23/`](./c23/) | C23 differential conformance gate |
| [`tools/vla/`](./vla/) | C99 Variable-Length Array verification suite |

## Active Tools & Verifiers

### `tools/verify/` — Regression Gates

| Tool | Purpose | Example Usage |
| :--- | :--- | :--- |
| [`test_runner.py`](./verify/test_runner.py) | Main test suite runner invoked by `make test` | `python3 tools/verify/test_runner.py` |
| [`test_wrapper.py`](./verify/test_wrapper.py) | Single-test execution wrapper used by `make test-single` | `python3 tools/verify/test_wrapper.py jit/basic` |
| [`opt_sweep.py`](./verify/opt_sweep.py) | Optimizer regression sweep: `-O0..-O3` x JIT + AOT via zerocc itself | `python3 tools/verify/opt_sweep.py` |
| [`san_sweep.py`](./verify/san_sweep.py) | `zero_san` (-fsanitize=undefined) flag-ON soundness sweep | `python3 tools/verify/san_sweep.py` |
| [`torture_run.py`](./verify/torture_run.py) | GCC `gcc.c-torture` differential conformance gate | `python3 tools/verify/torture_run.py --quick` |
| [`diag_battery.py`](./verify/diag_battery.py) | Differential diagnostics vs gcc on `test/diag/*.c` | `python3 tools/verify/diag_battery.py` |
| [`driver_tests.py`](./verify/driver_tests.py) | Multi-file driver tests (JIT, AOT, duplicate-def, missing-main) | `python3 tools/verify/driver_tests.py` |
| [`linker_tests.py`](./verify/linker_tests.py) | Own-linker (T2) differential battery | `python3 tools/verify/linker_tests.py` |
| [`abi_interop.py`](./verify/abi_interop.py) | External ABI interop: zerocc.o <-> gcc.o linking | `python3 tools/verify/abi_interop.py` |
| [`asm_diff.py`](./verify/asm_diff.py) | Inline-assembly Phase 1 differential gate (`-S` text path) | `python3 tools/verify/asm_diff.py` |
| [`cfg_verifier.py`](./verify/cfg_verifier.py) | CFG layer verification vs independent Python recomputation | `python3 tools/verify/cfg_verifier.py --verify-all` |
| [`loop_unroll_verifier.py`](./verify/loop_unroll_verifier.py) | `-O3` loop unrolling verifier | `python3 tools/verify/loop_unroll_verifier.py` |
| [`mem2reg_verifier.py`](./verify/mem2reg_verifier.py) | mem2reg pass verifier | `python3 tools/verify/mem2reg_verifier.py` |
| [`extension_verifier.py`](./verify/extension_verifier.py) | GNU extensions verifier (statement expressions, case ranges, ...) | `python3 tools/verify/extension_verifier.py` |
| [`regress_exec_diff.py`](./verify/regress_exec_diff.py) | Execution regression differential | `python3 tools/verify/regress_exec_diff.py <listfile>` |
| [`style_check.py`](./verify/style_check.py) | C style/convention checker | `python3 tools/verify/style_check.py` |
| [`sret_verifier.py`](./verify/sret_verifier.py) | System V AMD64 `>16B` struct return (`sret`) verifier | `python3 tools/verify/sret_verifier.py --verify-all` |
| [`type_tag_verifier.py`](./verify/type_tag_verifier.py) | Frontend type-tag vs backend IR width-mark cross-check | `python3 tools/verify/type_tag_verifier.py --verify-all` |
| [`isa_qemu_diff.py`](./verify/isa_qemu_diff.py) | **Raw binary emitter** differential under QEMU (all 4 ISAs) | `python3 tools/verify/isa_qemu_diff.py --isa aarch64` |
| [`isa128_diff.py`](./verify/isa128_diff.py) | Cross-ISA `__int128` differential gate (raw + `-S` paths) | `python3 tools/verify/isa128_diff.py --isa aarch64` |
| [`licm_test_suite.py`](./verify/licm_test_suite.py) / [`licm_verifier.py`](./verify/licm_verifier.py) | LICM hoisting & safety verification | `python3 tools/verify/licm_test_suite.py` |

### `tools/analysis/` — Codegen Inspection

| Tool | Purpose | Example Usage |
| :--- | :--- | :--- |
| [`elf_verify.py`](./analysis/elf_verify.py) | ELF64 relocatable object (`.o`) section, symbol, and relocation verifier | `python3 tools/analysis/elf_verify.py` |
| [`elf_min.py`](./analysis/elf_min.py) | Minimal read-only ELF32/ELF64 reader shared by raw-emitter tools | `python3 -c "import elf_min; e = elf_min.load('file.o')"` |
| [`riscv_enc_check.py`](./analysis/riscv_enc_check.py) | RISC-V raw-emitter instruction encoding oracle | `python3 tools/analysis/riscv_enc_check.py --zerocc` |
| [`reloc_pair_check.py`](./analysis/reloc_pair_check.py) | RISC-V ELF relocation pairing verifier | `python3 tools/analysis/reloc_pair_check.py --zerocc --test test/isa_qemu/01_arith.c` |
| [`vtable_check.py`](./analysis/vtable_check.py) | Multi-ISA backend vtable slot conformance auditor | `python3 tools/analysis/vtable_check.py --diff riscv64 riscv32` |
| [`cross_isa_comparator.py`](./analysis/cross_isa_comparator.py) | Multi-ISA assembly comparison tool | `python3 tools/analysis/cross_isa_comparator.py test/aot/hello_world.c` |
| [`qemu_test_runner.py`](./analysis/qemu_test_runner.py) | Cross-ISA execution runner using QEMU user-mode emulation | `python3 tools/analysis/qemu_test_runner.py --isa aarch64 test/aot/` |
| [`simd_regression_suite.py`](./analysis/simd_regression_suite.py) | Auto-vectorization (SSE2/SSE4.1, NEON, RVV) regression suite | `python3 tools/analysis/simd_regression_suite.py` |
| [`literal_verifier.py`](./analysis/literal_verifier.py) | Literal verifier | `python3 tools/analysis/literal_verifier.py` |
| [`prologue_epilogue_verifier.py`](./analysis/prologue_epilogue_verifier.py) | Prologue/epilogue emission verifier | `python3 tools/analysis/prologue_epilogue_verifier.py` |
| [`struct_abi_verifier.py`](./analysis/struct_abi_verifier.py) | System V AMD64 struct parameter & return classification verifier | `python3 tools/analysis/struct_abi_verifier.py --test test/abi/struct_return_20byte.c` |
| [`struct_return_verifier.py`](./analysis/struct_return_verifier.py) | Struct return register (`RAX`, `RAX:RDX`, `sret`) codegen checker | `python3 tools/analysis/struct_return_verifier.py` |
| [`struct_asm_analyzer.py`](./analysis/struct_asm_analyzer.py) | Disassembly analyzer for struct copies and stack frame offsets | `python3 tools/analysis/struct_asm_analyzer.py` |

### `tools/okf/` — Knowledge Graph

| Tool | Purpose | Example Usage |
| :--- | :--- | :--- |
| [`okf.py`](./okf/okf.py) | OKF v0.2 linter, search, index, and updater | `python3 tools/okf/okf.py lint && python3 tools/okf/okf.py index` |

### `tools/toolchain/` — Toolchain Setup

| Tool | Purpose | Example Usage |
| :--- | :--- | :--- |
| [`setup_musl_cross.sh`](./toolchain/setup_musl_cross.sh) | Build per-ISA static-musl cross toolchain (`musl-gcc-<arch>`) | `tools/toolchain/setup_musl_cross.sh --arch aarch64 --ccbin aarch64-linux-gnu-gcc` |
| [`musl_host_toolchain.sh`](./toolchain/musl_host_toolchain.sh) | Build static-musl host toolchain (used by Makefile) | invoked by `make` |
| [`embed_symtab.py`](./toolchain/embed_symtab.py) | Pre-baked `.symtab` generator for zero_rt dlfcn | invoked by `make build/bin/zerocc` |
| [`setup_dev_env.sh`](./toolchain/setup_dev_env.sh) | Development environment setup (Ubuntu/Debian/Fedora/Arch/macOS) | `tools/toolchain/setup_dev_env.sh` |

### `tools/suites/` — Test Suite Generators

| Tool | Purpose | Example Usage |
| :--- | :--- | :--- |
| [`struct_test_generator.py`](./suites/struct_test_generator.py) | Automated test generator for struct by-value and return ABI edge cases | `python3 tools/suites/struct_test_generator.py` |

### Standalone Test Suites

| Tool | Purpose | Example Usage |
| :--- | :--- | :--- |
| [`c11/test_c11.py`](./c11/test_c11.py) | C11 feature & concurrency verification suite | `python3 tools/c11/test_c11.py --verbose` |
| [`c23/test_c23.py`](./c23/test_c23.py) | C23 differential conformance gate | `python3 tools/c23/test_c23.py --all` |
| [`vla/test_vla.py`](./vla/test_vla.py) | C99 Variable-Length Array verification suite | `python3 tools/vla/test_vla.py` |

## Path Resolution (`find_repo_root`)

Every tool locates the repository root via [`find_repo_root()`](./__init__.py) instead of hardcoded `os.path.dirname` chains. It walks up from the tool's directory until it finds a repository marker (`AGENTS.md`, `Makefile`, `docs/okf/index.md`). This means tools keep working regardless of how deeply they're nested or whether the `tools/` folder is moved.

## Quick Verification Commands

```bash
# 1. Run full compiler regression suite (227 tests)
make test

# 2. Run C11 feature & concurrency verification suite (11 tests)
python3 tools/c11/test_c11.py --verbose

# 3. Verify System V AMD64 >16B struct return (sret) ABI
python3 tools/verify/sret_verifier.py --verify-all

# 4. Validate OKF v0.2 Knowledge Graph
python3 tools/okf/okf.py lint && python3 tools/okf/okf.py index

# 5. Run zero_san flag-ON soundness sweep (raw + asm paths, all 4 ISAs)
python3 tools/verify/san_sweep.py

# 6. Run optimizer sweep (-O0..-O3 x JIT + AOT via zerocc)
python3 tools/verify/opt_sweep.py
```

## Adding a New Tool

1. Place the script in the appropriate subpackage (`verify/`, `analysis/`, `okf/`, `toolchain/`, or `suites/`).
2. Use `find_repo_root()` for repo-root path resolution (see `tools/__init__.py`).
3. Keep it stdlib-only — add to `tools/requirements.txt` only if a third-party dependency is truly necessary.
4. Add a row to the table above and update all references in `Makefile`, `AGENTS.md`, and `docs/okf/build-and-verification.md`.
