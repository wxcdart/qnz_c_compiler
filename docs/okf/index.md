---
type: Index
title: ZeroCC Knowledge Graph (OKF Bundle)
description: Root navigator and knowledge graph entry point for AI coding agents working on ZeroCC, a multi-ISA C99/C11 compiler targeting x86-64, AArch64, and RISC-V.
resource: okf/index
tags: [zerocc, compiler, index, navigation, okf]
timestamp: 2026-10-15T22:55:00Z
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

---

## 4. Build, Testing & Verification Workflows

- **[Build, Test & Verification Guide](./build-and-verification.md)**
  - Makefile targets (`make all`, `make test`, `make rebuild`), running single tests, verifying AOT (`-o`) vs JIT (`--run`), and automated ABI verifiers (`tools/sret_verifier.py`, `tools/struct_abi_verifier.py`).
