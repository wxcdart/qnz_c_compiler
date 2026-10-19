---
type: Concept
title: Optimization Passes & Safety Invariants
description: Active IR optimization pipeline (Constant Folding, CopyProp, CSE, GVN, DCE, Mem2Reg, LICM, Auto-Vectorization) and their correctness invariants.
resource: middleend/optimization-passes
tags: [optimizations, mem2reg, licm, vectorization, gvn, cse, dce]
timestamp: 2026-10-19T03:05:39Z
generated: antigravity-agent
verified: engineering-lead
status: active
---

# Optimization Passes & Safety Invariants

Optimization passes are registered in `src/zero_cc/opt/opt_pass.c` (`g_opt_passes`) and execute sequentially over `ZeroCCCompiler`'s `IRArena`.

## Active Optimization Pipeline

1. **Constant Folding (`opt_constant_fold.c`)**
   - Evaluates constant binary/unary operations at compile time and replaces instructions with `OP_MOV_IMM`.
2. **Copy Propagation (`opt_copy_prop.c`)**
   - Propagates `OP_MOV_REG` and constant definitions across basic blocks, invalidating mappings at labels and redefinitions.
3. **Common Subexpression Elimination (`opt_cse.c`)**
   - Eliminates redundant pure expressions within basic blocks while respecting flag-setting instructions (`REG_FLAGS`).
4. **Global Value Numbering (`opt_gvn.c`)**
   - Hash-based value numbering and algebraic simplification (`x + 0`, `x * 1`, `x ^ x`, etc.).
5. **Local & Cross-Block Mem2Reg (`opt_mem2reg.c`)**
   - Forwards stack stores (`OP_MOV_MEM`) to subsequent stack loads (`OP_MOV`) to eliminate redundant stack traffic.
   - **Straight-Line & Fallthrough Optimization**:
     - Preserves slot tracking across conditional branches (`OP_JE`, `OP_JNE`, `OP_JL`, `OP_JLE`, `OP_JG`, `OP_JCOND`) along the fallthrough path.
     - Detects unreferenced labels (such as `loop_step` in for-loops without `continue`) and skips slot invalidation.
     - Preserves slot mappings across in-place sign/zero extensions (`OP_SEXT32`/`OP_ZEXT32`).
     - Tracks loaded temps so subsequent loads from the same slot in the straight-line path reuse the loaded temp.
   - **Cross-Block Loop Promotion (`opt_promote_loop_carried`)**:
     - Detects natural loops with induction variables and accumulators that are not address-taken (`!slot_addr_taken`).
     - Converts preheader initialization stores (`MOV_MEM [S], T_init`) and backedge stores (`MOV_MEM [S], T_update`) into virtual register transfers (`OP_MOV_REG T_head, ...`), and replaces the in-loop load with `OP_NOP`.
     - Completely eliminates stack memory reads and writes from the inner loop body for promotable loop-carried variables (dropping loop-simple to 0 stores in the loop).
   - **Critical Safety Invariants**:
     - Only tracks 8-byte aligned stack slots (`offset % 8 == 0`) whose stored source is a virtual register (`temp_src1 >= 16`). Reserved physical registers (`VREG_0`..`VREG_15`, `< 16`) are never forwarded across instructions by `opt_mem2reg.c`, `opt_copy_prop.c`, `opt_cse.c`, or `opt_gvn.c` because their live ranges are not tracked by the general register allocator.
     - When a sub-8-byte or unaligned store occurs (e.g., writing a 4-byte struct field at `-68(%rbp)` via `OP_LEA` + `OP_STORE_32`), **both** overlapping 8-byte boundary slots (`(-offset)/8` and `(-offset + 7)/8`) must be invalidated so stale 64-bit values are never forwarded to subsequent 8-byte struct loads.
     - Address-taken variables (`OP_LEA`), indirect writes (`OP_STORE_*`), and calls inside loops invalidate or disqualify loop promotion to maintain memory consistency.
6. **Loop-Invariant Code Motion (`opt_loop.c`)**
   - Identifies natural loops by mapping synthetic `label_idx` targets to instruction indices (`label_to_idx`).
   - Uses a **Two-Pass Rebuild** into a new `IRArena`: hoists pure loop-invariant instructions immediately before the loop header label, turns their in-loop originals into `OP_NOP`, and updates `c->ir_call_args` instruction indices.
   - **Stack Load & Extension Invariant Hoisting**: Detects read-only stack slots (loaded but never stored to in the loop body, no pointer aliasing, no calls) and hoists `OP_MOV` and downstream `OP_SEXT`/`OP_ZEXT` instructions out of the loop into the preheader (e.g. loop bound `n`).
   - **Call-Spanning Safety Invariant**: Does not hoist loop-invariant constants/expressions out of loops that contain `OP_CALL` or `OP_CALL_INDIRECT`, preventing hoisted temporaries from exceeding the 5 callee-saved GPRs on x86-64 and spilling into caller-saved registers across calls.
7. **SIMD Auto-Vectorization (`opt_vectorize.c`)**
   - Detects countable array arithmetic loops:
     - **Single-Precision Floats (`float[N]`)**: maps `OP_VLOADSS`, `OP_VSTORESS`, `OP_FADD`/`OP_FSUB`/`OP_FMUL`/`OP_FDIV` &rarr; 128-bit packed single vector instructions (`OP_VADDPS`, `OP_VSUBPS`, `OP_VMULPS`, `OP_VDIVPS`, `OP_VLOAD`, `OP_VSTORE`) with `vec_width = 4` (stride 4, `i += 4`).
     - **Double-Precision Floats (`double[N]`)**: maps `OP_VLOADSD`, `OP_VSTORESD`, `OP_FADD`/`OP_FSUB`/`OP_FMUL`/`OP_FDIV` &rarr; 128-bit packed double vector instructions (`OP_VADDPD`, `OP_VSUBPD`, `OP_VMULPD`, `OP_VDIVPD`, `OP_VLOAD`, `OP_VSTORE`) with `vec_width = 2` (stride 8, `i += 2`).
     - **32-Bit Packed Integers (`int[N]`)**: maps `OP_LOAD_32`, `OP_STORE_32`, `OP_ADD`/`OP_SUB`/`OP_MUL` &rarr; 128-bit packed integer vector instructions (`OP_VPADDD`, `OP_VPSUBD`, `OP_VPMULLD`, `OP_VLOAD`, `OP_VSTORE`) with `vec_width = 4` (stride 4, `i += 4`).
     - Supports multi-ISA backend lowering across x86-64 SSE2/SSE4.1 (`paddd`, `psubd`, `pmulld`), AArch64 NEON (`add.4s`, `sub.4s`, `mul.4s`, `fadd.2d`, `fadd.4s`, `ldr/str q`), and RISC-V RVV 1.0 (`vsetivli zero, 4, e32, m1`, `vadd.vv`, `vsub.vv`, `vmul.vv`, `vfadd.vv`, `vle32.v`/`vse32.v`).
   - **Call-Safety Invariant (C11)**: a loop is only vectorized if its body contains **no `OP_CALL` / `OP_CALL_INDIRECT` / `OP_RET`** (`has_call` guard in `analyze_loop_for_vectorization_full`). The transform widens the induction step (`i += 1` → `i += vec_width`) and converts scalar loads/arith/stores to 128-bit lane ops - a call (or return) in the body cannot be replicated across lanes and would run only once per `vec_width` iterations, silently dropping loop iterations (pre-existing miscompile: `for(...) { float t = use2(1.0f); s += t; }` incremented `k` by 4). Known residuals: mid-body conditional branches are not yet rejected (the loop-condition `JE` sits inside every for-loop's backedge span, so a blanket jump guard would disable the pass), and the increment rewrite matches any `OP_ADD` whose operand is constant 1, not only the counter's.
8. **Dead Code Elimination (`opt_dce.c`)**
   - Marks unused temporaries and dead instructions as `OP_DEAD` / `OP_NOP` while preserving instructions with side effects (stores, calls, volatile loads, flag setters consumed by branches).

## Debugging Optimizations
- Use `--dump-ir` or `--profile-opts` on `./build/bin/zerocc` to inspect IR before and after optimization passes.

## Related Concepts
- Return to [Root Index](./index.md)
- See [Two-Pass IR & Register Allocation](./ir-and-regalloc.md)
- See [Multi-ISA Backends](./isa-backends.md)
