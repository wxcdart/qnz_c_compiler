# ZeroCC Verification & Tooling Suite

This directory contains the active verification suites, test runners, and knowledge-graph tools for ZeroCC. Historical one-off debugging scripts and migration helpers are preserved in [`tools/archive/`](./archive/).

## Active Tools & Verifiers

| Tool | Purpose | Example Usage |
| :--- | :--- | :--- |
| [`okf.py`](./okf.py) | Open Knowledge Format (OKF v0.2) linter, search, index, and updater | `python3 tools/okf.py lint && python3 tools/okf.py index` |
| [`test_runner.py`](./test_runner.py) | Main test suite runner invoked by `make test` (140 unit, semantic, ABI, JIT, AOT, LICM tests) | `python3 tools/test_runner.py` |
| [`test_wrapper.py`](./test_wrapper.py) | Single-test execution wrapper used by `make test-single` | `python3 tools/test_wrapper.py jit/basic` |
| [`c11/test_c11.py`](./c11/test_c11.py) | C11 verification suite (`_Alignas`, `_Alignof`, anonymous structs/unions, `_Generic`, `_Static_assert`, atomics, `<threads.h>`) | `python3 tools/c11/test_c11.py --verbose` |
| [`vla/test_vla.py`](./vla/test_vla.py) | C99 Variable-Length Array (1D & multi-dimensional matrix VLA) verification suite | `python3 tools/vla/test_vla.py` |
| [`sret_verifier.py`](./sret_verifier.py) | System V AMD64 ABI hidden-pointer (`>16B` struct return `sret`) assembly & execution verifier | `python3 tools/sret_verifier.py --verify-all` |
| [`struct_abi_verifier.py`](./struct_abi_verifier.py) | System V AMD64 struct parameter & return classification verifier (`<=8B`, `9-16B`, `>16B`) | `python3 tools/struct_abi_verifier.py --test test/abi/struct_return_20byte.c` |
| [`struct_return_verifier.py`](./struct_return_verifier.py) | Struct return register (`RAX`, `RAX:RDX`, `sret`) codegen checker | `python3 tools/struct_return_verifier.py` |
| [`struct_asm_analyzer.py`](./struct_asm_analyzer.py) | Disassembly analyzer for struct copies and stack frame offsets | `python3 tools/struct_asm_analyzer.py` |
| [`struct_test_generator.py`](./struct_test_generator.py) | Automated test generator for struct by-value and return ABI edge cases | `python3 tools/struct_test_generator.py` |
| [`licm_verifier.py`](./licm_verifier.py) / [`licm_test_suite.py`](./licm_test_suite.py) | Loop-Invariant Code Motion (LICM) hoisting and safety verifier | `python3 tools/licm_test_suite.py` |
| [`simd_regression_suite.py`](./simd_regression_suite.py) | Auto-vectorization (SSE2/SSE4.1, NEON, RVV) regression suite | `python3 tools/simd_regression_suite.py` |
| [`cross_isa_comparator.py`](./cross_isa_comparator.py) | Multi-ISA assembly comparison tool (`x86-64`, `aarch64`, `riscv64`) | `python3 tools/cross_isa_comparator.py test/aot/hello_world.c` |
| [`qemu_test_runner.py`](./qemu_test_runner.py) | Cross-ISA execution runner using QEMU user-mode emulation (superseded for raw-emitter testing by `isa_qemu_diff.py`) | `python3 tools/qemu_test_runner.py --isa aarch64 test/aot/` |
| [`isa_qemu_diff.py`](./isa_qemu_diff.py) | **Raw binary emitter** differential test across all 4 ISAs under QEMU (`make test-isa-qemu`); needs qemu-user-static + a static-musl reference per ISA (`setup_musl_cross.sh`) | `python3 tools/isa_qemu_diff.py --isa aarch64` |
| [`setup_musl_cross.sh`](./setup_musl_cross.sh) | Build a per-ISA **static-musl** cross toolchain (`musl-gcc-<arch>` + specs, and a rv32 libgcc shim) — the uniform reference for `isa_qemu_diff.py` | `tools/setup_musl_cross.sh --arch aarch64 --ccbin aarch64-linux-gnu-gcc` |
| [`elf_verify.py`](./elf_verify.py) | ELF64 relocatable object (`.o`) section, symbol, and relocation verifier | `python3 tools/elf_verify.py` |

## Quick Verification Commands

```bash
# 1. Run full compiler regression suite (140 tests)
make test

# 2. Run C11 feature & concurrency verification suite (11 tests)
python3 tools/c11/test_c11.py --verbose

# 3. Verify System V AMD64 >16B struct return (sret) ABI
python3 tools/sret_verifier.py --verify-all

# 4. Validate OKF v0.2 Knowledge Graph
python3 tools/okf.py lint && python3 tools/okf.py index
```
