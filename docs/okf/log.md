---
type: Log
title: ZeroCC OKF Knowledge Bundle Log
description: Sequential changelog tracking how and when the ZeroCC OKF knowledge bundle evolves alongside compiler sprints.
resource: okf/log
tags: [zerocc, changelog, log, okf]
timestamp: 2026-10-19T02:24:18Z
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

## 2026-10-16 — Updated `frontend-and-types.md`
- Recorded Known Defect: 2-D/3-D array declarations silently miscompiled (parser single-bracket + NODE_INDEX missing pointer decay); plan in docs/MULTIDIM_VLA_PLAN.md

## 2026-10-16 — Updated `optimization-passes.md`
- Implement 32-bit packed integer SIMD auto-vectorization across x86-64 SSE2/SSE4.1, AArch64 NEON, and RISC-V RVV 1.0

## 2026-10-17 — Updated `frontend-and-types.md`
- Multi-dimensional arrays and matrix VLAs (Phase 0 and Phase 1) fully supported

## 2026-10-17 — Updated `frontend-and-types.md`
- Implement C11 core keywords (_Alignas, _Alignof, anonymous structs/unions, _Generic), C11 <threads.h> & atomics, and global variable value load/assignment/ELF object linkage

## 2026-10-17 — Updated `optimization-passes.md`
- Add LICM call-spanning safety invariant to prevent hoisting constants across loops with function calls

## 2026-10-17 — Updated `build-and-verification.md`
- Update build-and-verification guide with 140-test suite, C11/VLA verifiers, and curated tools/ vs tools/archive/ layout

## 2026-10-17 — Updated `index.md`
- List C11 and VLA verifier suites in root OKF index

## 2026-10-17 — Updated `frontend-and-types.md`
- Unify ZeroCCType and TypeInfo construction and AST type queries via canonical type_info.h bridge

## 2026-10-17 — Updated `architecture-pipeline.md`
- Convert ast_lowering.c, ast_lowering_stmt.c, ast_lowering_expr.c, ast_lowering_mc.c, asm_emitter.c, and opt_pass.c from unity-included .c files into standalone .o translation units

## 2026-10-17 — Updated `architecture-pipeline.md`
- Split src/zero_cc/parser/ into parser/ (AST construction), ir/ (IR, lowering, regalloc, codegen), and opt/ (optimization passes)

## 2026-10-17 — Updated `ir-and-regalloc.md`
- Mirror scratch-register spill reload/store in asm_emitter.c, enforce callee-saved-only allocation for spans_call intervals, and emit callee-save stores in emit_prologue_mc

## 2026-10-17 — Updated `ir-and-regalloc.md`
- Unified register allocator spill/reload across JIT/AOT and enabled prologue callee-saved register stores (OP_CALLEE_SAVE_STORES)

## 2026-10-17 — Updated `frontend-and-types.md`
- Verified standalone C99/C11 compilation across 29/29 test files: multi-level pointers, opaque struct typedefs, struct array members, nested brace initializers, multi-declarator extern, and arbitrary va_arg types

## 2026-10-17 — Updated `global-data-relocs.md`
- Add global variables & data reloc concept page: file-scope initializers, ZeroCCDataReloc (JIT patch + AOT .rela.<sec>), ELF writer multi-section reloc support

## 2026-10-17 — Updated `frontend-and-types.md`
- Document float/double struct member tags, NODE_MEMBER s.str/aux union aliasing gotcha, goto/label/switch implementation (CMP/COND_SET/JNE dispatch, break via label_patcher)

## 2026-10-17 — Updated `isa-backends.md`
- Document OP_JE/OP_JNE jump-if-zero/nonzero semantics and JIT global data reloc patching

## 2026-10-17 — Updated `scalar-float-x86.md`
- Added scalar-float-x86 page: double-width FP model, VLOADSS/SD emitters, XMM/VEC classing, WSL2 128-bit RX-read GPF workaround

## 2026-10-17 — Updated `frontend-and-types.md`
- Sprint 22: signedness tags end-to-end, narrow-int width extension at value boundaries, both param parser paths

## 2026-10-17 — Updated `isa-backends.md`
- Sprint 22: OP_SEXT/ZEXT emitters all ISAs, x86 byte-register ModRM trap (verified against GNU as), GAS syntax limits

## 2026-10-18 — Updated `ir-and-regalloc.md`
- C2a: temp-id exhaustion guard (silent miscompilation -> clean diagnostic); temp-id space documented; C2b (16-bit widening) pending

## 2026-10-18 — Updated `ir-and-regalloc.md`
- C2b: temp ids widened to 16-bit (TEMP_NONE=0xFFFF sentinel), LRA 256-cap removed, base-register markers moved to TEMP_NONE, OP_CALL side table is sole arg source

## 2026-10-18 — Updated `frontend-and-types.md`
- Resolve C5 multi-segment member chains through pointer members and width-safe pointer deref stores

## 2026-10-18 — Updated `scalar-float-x86.md`
- Resolve C6 scalar float stack load/store (OP_FLOAD/OP_FSTORE) and FP call arg emission in AOT -S path across x86-64, AArch64, and RISC-V

## 2026-10-18 — Updated `architecture-pipeline.md`
- Resolve H2: ensure compiler diagnostics and syntax errors halt compilation cleanly across JIT and AOT modes

## 2026-10-18 — Updated `frontend-and-types.md`
- Enforce 127-char identifier limit with clean fatal diagnostic (M1 resolved)

## 2026-10-18 — Updated `architecture-pipeline.md`
- Resolve H1: raise stack limit to 64MB and guard expression recursion depth with clean diagnostic

## 2026-10-18 — Updated `isa-backends.md`
- Add OP_CVTSI2SD/OP_CVTTSD2SI asm emission and -S .data section emission across ISAs

## 2026-10-18 — Updated `ir-and-regalloc.md`
- C8 invariants: materialized FP call returns, XMM spans_call forced spill, no-steal-from-spilled guards, class-aware FP spill scratch (fp_scratch_reg1/2 = 15/14), XMM-VEC pool cross-marking

## 2026-10-18 — Updated `optimization-passes.md`
- C11 invariant: auto_vectorize rejects loops whose body contains OP_CALL/OP_CALL_INDIRECT/OP_RET (has_call guard); documented residuals (mid-body branches, increment-rewrite breadth)

## 2026-10-18 — Updated `scalar-float-x86.md`
- Sprint 30: -S float parity (VLOADSS/VSTORESS width, 3-operand FP, ucomisd compares, CMP_ZERO); C12 ucomisd flag mapping; C13 REX-before-prefix invariant

## 2026-10-19 — Updated `frontend-and-types.md`
- Replace Scope TPHT with dynamically resizing separate-chaining hash table, eliminating scope symbol limits

## 2026-10-19 — Updated `ir-and-regalloc.md`
- Document spill philosophy: linear-scan speed vs graph-coloring trade-offs, scratch registers, and mem2reg division of labor

## 2026-10-19 — Updated `optimization-passes.md`
- Document mem2reg loop stack traffic impact and cross-block optimization roadmap
