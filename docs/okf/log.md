---
type: Log
title: ZeroCC OKF Knowledge Bundle Log
description: Sequential changelog tracking how and when the ZeroCC OKF knowledge bundle evolves alongside compiler sprints.
resource: okf/log
tags: [zerocc, changelog, log, okf]
timestamp: 2026-10-16T05:23:53Z
generated: antigravity-agent
verified: engineering-lead
status: active
---

# ZeroCC OKF Bundle Log

This file tracks sequential updates to the [ZeroCC OKF Knowledge Graph](./index.md).

## 2026-10-15 — Initial OKF v0.2 Bundle Creation & Sprint 17 (`sret` + Function Pointers)
- **Initialized OKF Bundle (`docs/okf/`)**:
  - Created [index.md](./index.md) as the root navigator for AI coding agents.
  - Created single-concept nodes covering the full compiler architecture:
    - [architecture-pipeline.md](./architecture-pipeline.md)
    - [frontend-and-types.md](./frontend-and-types.md)
    - [ir-and-regalloc.md](./ir-and-regalloc.md)
    - [optimization-passes.md](./optimization-passes.md)
    - [abi-sysv-x86_64.md](./abi-sysv-x86_64.md)
    - [isa-backends.md](./isa-backends.md)
    - [build-and-verification.md](./build-and-verification.md)
- **Recorded Sprint 17 System V AMD64 `sret` Invariants**:
  - Documented hidden return pointer (`%rdi`) handling for structs `>16` bytes across direct calls, member access on call expressions, struct reassignment, tail returns, and indirect function pointer calls (`Task 4.3`).
  - Documented size-aware tail chunk copy (`32`/`16`/`8`-bit load/store) to prevent stack clobbering on non-multiple-of-8 structs (such as 20-byte structs).
  - Documented `tools/okf.py` for validating YAML frontmatter, relative markdown graph links, ranked search, and portable skill setup.

## 2026-10-16 — Updated `isa-backends.md`
- Documented Multi-ISA sret (AArch64 x8 and RISC-V a0) calling conventions and codegen

## 2026-10-16 — Updated `isa-backends.md`
- Multi-ISA SIMD vector emission across x86-64 SSE, AArch64 NEON, and RISC-V RVV

## 2026-10-16 — Updated `optimization-passes.md`
- Auto-vectorization pass safety invariants and loop transformation across ISAs

## 2026-10-16 — Updated `build-and-verification.md`
- Multi-ISA sret cross-verification across x86-64, AArch64, and RISC-V under QEMU

## 2026-10-16 — Updated `optimization-passes.md`
- Expand auto-vectorization pass to support packed 64-bit double precision SIMD loops (OP_VADDPD, OP_VSUBPD, OP_VMULPD, OP_VDIVPD, vec_width=2) across x86-64, AArch64, and RISC-V.

## 2026-10-16 — Updated `optimization-passes.md`
- Add backend peephole optimization: eliminate redundant register self-moves in AOT assembly and JIT machine code, and suppress synthetic OP_NOP assembly comments.
