---
type: Concept
title: Optimization Passes & Safety Invariants
description: Active IR optimization pipeline (Constant Folding, CopyProp, CSE, GVN, DCE, Mem2Reg, LICM, Auto-Vectorization) and their correctness invariants.
resource: middleend/optimization-passes
tags: [optimizations, mem2reg, licm, vectorization, gvn, cse, dce]
timestamp: 2026-10-16T05:11:49Z
generated: antigravity-agent
verified: engineering-lead
status: active
---

# Optimization Passes & Safety Invariants

Optimization passes are registered in `src/zero_cc/parser/opt_pass.c` (`g_opt_passes`) and execute sequentially over `ZeroCCCompiler`'s `IRArena`.

## Active Optimization Pipeline

1. **Constant Folding (`opt_constant_fold.c`)**
   - Evaluates constant binary/unary operations at compile time and replaces instructions with `OP_MOV_IMM`.
2. **Copy Propagation (`opt_copy_prop.c`)**
   - Propagates `OP_MOV_REG` and constant definitions across basic blocks, invalidating mappings at labels and redefinitions.
3. **Common Subexpression Elimination (`opt_cse.c`)**
   - Eliminates redundant pure expressions within basic blocks while respecting flag-setting instructions (`REG_FLAGS`).
4. **Global Value Numbering (`opt_gvn.c`)**
   - Hash-based value numbering and algebraic simplification (`x + 0`, `x * 1`, `x ^ x`, etc.).
5. **Local Mem2Reg (`opt_mem2reg.c`)**
   - Forwards stack stores (`OP_MOV_MEM`) to subsequent stack loads (`OP_MOV`) within basic blocks to eliminate redundant stack traffic.
   - **Critical Safety Invariants**:
     - Only tracks 8-byte aligned stack slots (`offset % 8 == 0`).
     - When a sub-8-byte or unaligned store occurs (e.g., writing a 4-byte struct field at `-68(%rbp)` via `OP_LEA` + `OP_STORE_32`), **both** overlapping 8-byte boundary slots (`(-offset)/8` and `(-offset + 7)/8`) must be invalidated so stale 64-bit values are never forwarded to subsequent 8-byte struct loads.
     - Invalidates all tracked slots at labels, calls, indirect stores, and for address-taken variables (`OP_LEA`).
6. **Loop-Invariant Code Motion (`opt_loop.c`)**
   - Identifies natural loops by mapping synthetic `label_idx` targets to instruction indices (`label_to_idx`).
   - Uses a **Two-Pass Rebuild** into a new `IRArena`: hoists pure loop-invariant instructions immediately before the loop header label, turns their in-loop originals into `OP_NOP`, and updates `c->ir_call_args` instruction indices.
7. **SIMD Auto-Vectorization (`opt_vectorize.c`)**
   - Detects countable array arithmetic loops:
     - **Single-Precision Floats (`float[N]`)**: maps `OP_VLOADSS`, `OP_VSTORESS`, `OP_FADD`/`OP_FSUB`/`OP_FMUL`/`OP_FDIV` &rarr; 128-bit packed single vector instructions (`OP_VADDPS`, `OP_VSUBPS`, `OP_VMULPS`, `OP_VDIVPS`, `OP_VLOAD`, `OP_VSTORE`) with `vec_width = 4` (stride 4, `i += 4`).
     - **Double-Precision Floats (`double[N]`)**: maps `OP_VLOADSD`, `OP_VSTORESD`, `OP_FADD`/`OP_FSUB`/`OP_FMUL`/`OP_FDIV` &rarr; 128-bit packed double vector instructions (`OP_VADDPD`, `OP_VSUBPD`, `OP_VMULPD`, `OP_VDIVPD`, `OP_VLOAD`, `OP_VSTORE`) with `vec_width = 2` (stride 8, `i += 2`).
     - Supports multi-ISA backend lowering across x86-64 SSE2, AArch64 NEON (`fadd.2d`, `fadd.4s`, `ldr/str q`), and RISC-V RVV 1.0 (`vsetivli`, `vfadd.vv`, `vle32.v`/`vse32.v`).
8. **Dead Code Elimination (`opt_dce.c`)**
   - Marks unused temporaries and dead instructions as `OP_DEAD` / `OP_NOP` while preserving instructions with side effects (stores, calls, volatile loads, flag setters consumed by branches).

## Debugging Optimizations
- Use `--dump-ir` or `--profile-opts` on `./build/bin/zerocc` to inspect IR before and after optimization passes.

## Related Concepts
- Return to [Root Index](./index.md)
- See [Two-Pass IR & Register Allocation](./ir-and-regalloc.md)
- See [Multi-ISA Backends](./isa-backends.md)
