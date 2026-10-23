---
type: Index
title: ZeroCC Knowledge Graph (OKF Bundle)
description: Root navigator and knowledge graph entry point for AI coding agents working on ZeroCC, a multi-ISA C99/C11 compiler targeting x86-64, AArch64, and RISC-V.
resource: okf/index
tags: [zerocc, compiler, index, navigation, okf]
timestamp: 2026-10-23T04:25:11Z
generated: antigravity-agent
verified: engineering-lead
status: active
---

# ZeroCC Open Knowledge Format (OKF) Bundle

Welcome to the **ZeroCC Knowledge Graph**. Start here to navigate architectural rules, ABI invariants, IR design, optimization passes, and verification workflows without scanning the entire 80+ file `docs/` archive.

- **Changelog / Evolution**: [Bundle Log](./log.md) | [Full Project Changelog](../CHANGELOG.md)
- **Project Roadmap**: [Development Plan](../ZEROCC_Development_Plan.md)

---

## 1. Core Architecture & Compilation Pipeline

- **[Pipeline & Two-Pass Architecture](./architecture-pipeline.md)**
  - Lexer → Arena AST Parser → Semantic Analysis → AST-to-IR Lowering → IR Optimizations → Linear Scan Register Allocation → JIT / AOT Emission.
- **[Frontend, Type System & Semantic Analysis](./frontend-and-types.md)**
  - `ZeroCCType` vs semantic `TypeInfo`, struct/union layout, function pointers (`is_func_ptr`), C99/C11 features (`_Generic`, `_Atomic`, `_Thread_local`, VLA, designated initializers).
- **[Multi-Translation-Unit Driver (T1) & JIT Image Merge](./multi-tu.md)**
  - Multi-input driver modes, the multi-TU JIT image merge (`zero_cc_tu_merge`), global-registry snapshot discipline, extern-object load/store paths, and unnamed prototype parameters.
- **[ZeroCC Own Linker (T2)](./own-linker.md)**
  - ELF64-LE object reader, classic `ar` archive reader with greedy member extraction, symbol resolution, the x86-64 static relocation set, the generated `_start` stub, and the `make test-linker` differential gate.
- **[Zero_RT — ZeroCC's Own Freestanding Runtime (Phase 0: Linux x86-64)](./zero-rt.md)**
  - The `zero_rt/` runtime (brk heap, strings, atexit/exit, unbuffered stdio with vendored Ryū float formatting) compiled by zerocc itself, auto-linked by `--zerocc-link` (`--no-zerocc-rt` to opt out), and the `zeroc_main_wrapper` `_start` routing that runs atexit on a plain `return` from `main`.

---

## 2. IR, Register Allocation & Optimization Passes

- **[Two-Pass IR & Register Allocation](./ir-and-regalloc.md)**
  - `Instr` 51-opcode representation, `IRCallArgs` call-site side table, virtual-to-physical register mapping, reserved registers (`%rsp`, `%rbp`, `%rcx`, `%r10`, `%r11`), and loop live-range extensions.
- **[Optimization Passes & Safety Invariants](./optimization-passes.md)**
  - Active passes (`constant_fold`, `copy_prop`, `cse`, `gvn`, `dce`, `mem2reg`, `licm`, `auto_vectorize`), sub-8-byte stack store invalidation in `mem2reg`, and two-pass `IRArena` rebuild in `licm`.

---

## 3. ABI & Multi-ISA Backends

- **[System V AMD64 ABI & Struct Calling Conventions](./abi-sysv-x86_64.md)**
  - Integer (`%rdi`..`%r9`) and FP (`%xmm0`..`%xmm7`) register passing, stack arguments, variadic `va_start` spilling, struct by-value (`<=8B`, `9-16B`, `>16B`), and hidden pointer (`sret`) returns for `>16B` structs (direct & indirect calls).
- **[Multi-ISA Backends (x86-64, AArch64, RISC-V)](./isa-backends.md)**
  - `ZeroCCISABackend` vtable dispatch, JIT machine code emission vs AOT `.s` assembly emission, parallel call argument setup (`push`/`pop` & `%r11` indirect call scratch), and ELF/JIT relocation resolution.
- **[Global Variables, Data Section & Pointer Relocations](./global-data-relocs.md)**
  - File-scope initializers (scalars, IEEE-754 floats, aggregate/designated init, char arrays from strings), `ZeroCCDataReloc`, JIT data-page patching, and AOT `.rela.<sec>` emission.
- **[Scalar Float Pipeline (x86-64) & WSL2 RX-Page 128-Byte Read Limitation](./scalar-float-x86.md)**
  - Double-width scalar FP model, `VLOADSS/SD`/`VSTORESS/SD` emitters (movss/movsd + widen/narrow), XMM-vs-VEC classing of scalar V-loads, and the WSL2 GPF on 16-byte reads from RX pages (why float negation is `0.0 - x`).

---

## 4. Build, Testing & Verification Workflows

- **[Build, Test & Verification Guide](./build-and-verification.md)**
  - Makefile targets (`make all`, `make test`, `make rebuild`), running single tests, verifying AOT (`-o`) vs JIT (`--run`), and automated C99/C11 & ABI verifiers (`tools/c11/test_c11.py`, `tools/vla/test_vla.py`, `tools/sret_verifier.py`, `tools/struct_abi_verifier.py`).
- **[Performance Measurements & the JIT-vs-GCC Claim](./perf-measurements.md)**
  - Measured per-pass optimizer costs (`--profile-opts`) before/after the Sprint 35 CFG migration, the retracted "~78× faster than GCC" claim with the re-measured JIT-vs-GCC matrix, the mem2reg cost analysis, and the 16-bit temp-id OOB bugs the measurement exposed.

