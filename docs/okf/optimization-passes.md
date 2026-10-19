---
type: Concept
title: Optimization Passes & Safety Invariants
description: Active IR optimization pipeline (Constant Folding, CopyProp, CSE, GVN, DCE, Mem2Reg, LICM, Auto-Vectorization) and their correctness invariants.
resource: middleend/optimization-passes
tags: [optimizations, mem2reg, licm, vectorization, gvn, cse, dce]
timestamp: 2026-10-19T23:25:28Z
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
5. **Small Function Inlining (`ast_lowering_expr.c` / `jit/compile.h`)**
   - Automatically inlines static and `inline` leaf/helper functions ($N \le 35$ AST nodes, non-variadic, no struct returns $>8$B, non-recursive) at `-O2`/`-O3` (up to depth 2).
   - Candidate function bodies are cloned from function-local AST arenas into TU-level `tu_ast_arena`.
   - At call sites, argument expressions are evaluated to temporaries, bound to parameter identifiers, and the inlined body is lowered directly into caller IR, routing returns to destination temporaries without call instruction or stack frame overhead.
   - Subsequent passes (`mem2reg`, `licm`, `copy_prop`, `dce`) optimize seamlessly across the inlined boundary.
6. **Local & Cross-Block Mem2Reg (`opt_mem2reg.c`)**
   - Forwards stack stores (`OP_MOV_MEM`) to subsequent stack loads (`OP_MOV`) to eliminate redundant stack traffic.
   - **Straight-Line & Fallthrough Optimization**:
     - Preserves slot tracking across conditional branches (`OP_JE`, `OP_JNE`, `OP_JL`, `OP_JLE`, `OP_JG`, `OP_JCOND`) along the fallthrough path.
     - Detects unreferenced labels (such as `loop_step` in for-loops without `continue`) and skips slot invalidation.
     - Preserves slot mappings across in-place sign/zero extensions (`OP_SEXT32`/`OP_ZEXT32`).
     - Tracks loaded temps so subsequent loads from the same slot in the straight-line path reuse the loaded temp.
   - **Cross-Block Loop Promotion (`opt_promote_loop_carried`)**:
     - Detects natural loops with induction variables and accumulators that are not address-taken (`!slot_addr_taken`).
     - Converts preheader initialization stores (`MOV_MEM [S], T_init`) and backedge stores (`MOV_MEM [S], T_update`) into virtual register transfers (`OP_MOV_REG T_head, ...`), and replaces the in-loop load with `OP_NOP`.
     - **Conservative slot audit (Sprint 35 Phase 0)**: a slot is promoted only if the ENTIRE function is auditable - exactly one in-loop load and one in-loop store, exactly one out-of-loop store (the preheader init, strictly before the header), and every remaining load strictly after the loop exit. Other loads before the exit or any extra store disqualify the slot. ALL post-loop loads (until `RET`/`OP_EPILOGUE` or a store to the same slot) are forwarded to the head temp - forwarding only to the next label is insufficient because code after the loop (e.g. a post-loop print) still reads the stale slot.
     - **Loop entry-cleanness guard (Sprint 35 Phase 0)**: the loop is disqualified from promotion if any jump/branch from OUTSIDE the loop range targets a label inside it (header included) - such an entry skips the preheader init, leaving the promoted head temp uninitialised where the original in-loop load would have read the slot (see `test/jit/goto_into_loop.c`).
     - Completely eliminates stack memory reads and writes from the inner loop body for promotable loop-carried variables (dropping loop-simple to 0 stores in the loop).
   - **Critical Safety Invariants**:
     - Only tracks 8-byte aligned stack slots (`offset % 8 == 0`) whose stored source is a virtual register (`temp_src1 >= 16`). Reserved physical registers (`VREG_0`..`VREG_15`, `< 16`) are never forwarded across instructions by `opt_mem2reg.c`, `opt_copy_prop.c`, `opt_cse.c`, or `opt_gvn.c` because their live ranges are not tracked by the general register allocator.
     - When a sub-8-byte or unaligned store occurs (e.g., writing a 4-byte struct field at `-68(%rbp)` via `OP_LEA` + `OP_STORE_32`), **both** overlapping 8-byte boundary slots (`(-offset)/8` and `(-offset + 7)/8`) must be invalidated so stale 64-bit values are never forwarded to subsequent 8-byte struct loads.
     - Address-taken variables (`OP_LEA`), indirect writes (`OP_STORE_*`), and calls inside loops invalidate or disqualify loop promotion to maintain memory consistency.
7. **Loop-Invariant Code Motion (`opt_loop.c`)**
   - Identifies natural loops by mapping synthetic `label_idx` targets to instruction indices (`label_to_idx`). Note `detect_loops` may produce **multiple `LoopInfo` entries sharing one header** (several backedges into the same label, e.g. `continue` + main backedge).
   - Uses a **Two-Pass Rebuild** into a new `IRArena`: hoists pure loop-invariant instructions immediately before the loop header label, turns their in-loop originals into `OP_NOP`, and updates `c->ir_call_args` instruction indices.
   - **Shared-Header Emission (Sprint 35 Phase 0)**: when several loops share a header, the rebuild must emit the UNION of all those loops' invariants before the header (deduplicated via `emitted_at_header`). Emitting only the first matching loop's invariants loses definitions: the in-loop copies of the later loops' invariants are still NOP'd, leaving the temps undefined (Sprint 33 regression: `MOV_IMM r51, 1` in `break_continue.c` hoisted for loop A but the in-loop copy NOP'd under loop B's marking).
   - **Stack Load & Extension Invariant Hoisting**: Detects read-only stack slots (loaded but never stored to in the loop body, no pointer aliasing, no calls) and hoists `OP_MOV` and downstream `OP_SEXT`/`OP_ZEXT` instructions out of the loop into the preheader (e.g. loop bound `n`).
   - **Call-Spanning Safety Invariant**: Does not hoist loop-invariant constants/expressions out of loops that contain `OP_CALL` or `OP_CALL_INDIRECT`, preventing hoisted temporaries from exceeding the 5 callee-saved GPRs on x86-64 and spilling into caller-saved registers across calls.
   - **In-Loop Definition Invariant**: An instruction is hoisted only if every definition of each source operand either lies outside the loop body before the header, or (within the body) is itself marked invariant. A source defined ANYWHERE in the body - including AFTER the candidate, which feeds back through the backedge on the next iteration - makes the candidate loop-dependent. This is what protects promoted induction variables: after mem2reg + copy propagation the increment is `ADD r_new, i, 1` with `i` defined by the backedge write-back at the bottom of the loop; without this check the ADD would be hoisted and `i` frozen. In-place self-references (`SEXT32 r, r`) are exempt when they are the body's only definition of that temp.
8. **SIMD Auto-Vectorization (`opt_vectorize.c`)**
   - Detects countable array arithmetic loops:
     - **Single-Precision Floats (`float[N]`)**: maps `OP_VLOADSS`, `OP_VSTORESS`, `OP_FADD`/`OP_FSUB`/`OP_FMUL`/`OP_FDIV` &rarr; 128-bit packed single vector instructions (`OP_VADDPS`, `OP_VSUBPS`, `OP_VMULPS`, `OP_VDIVPS`, `OP_VLOAD`, `OP_VSTORE`) with `vec_width = 4` (stride 4, `i += 4`).
     - **Double-Precision Floats (`double[N]`)**: maps `OP_VLOADSD`, `OP_VSTORESD`, `OP_FADD`/`OP_FSUB`/`OP_FMUL`/`OP_FDIV` &rarr; 128-bit packed double vector instructions (`OP_VADDPD`, `OP_VSUBPD`, `OP_VMULPD`, `OP_VDIVPD`, `OP_VLOAD`, `OP_VSTORE`) with `vec_width = 2` (stride 8, `i += 2`).
     - **32-Bit Packed Integers (`int[N]`)**: maps `OP_LOAD_32`, `OP_STORE_32`, `OP_ADD`/`OP_SUB`/`OP_MUL` &rarr; 128-bit packed integer vector instructions (`OP_VPADDD`, `OP_VPSUBD`, `OP_VPMULLD`, `OP_VLOAD`, `OP_VSTORE`) with `vec_width = 4` (stride 4, `i += 4`).
     - Supports multi-ISA backend lowering across x86-64 SSE2/SSE4.1 (`paddd`, `psubd`, `pmulld`), AArch64 NEON (`add.4s`, `sub.4s`, `mul.4s`, `fadd.2d`, `fadd.4s`, `ldr/str q`), and RISC-V RVV 1.0 (`vsetivli zero, 4, e32, m1`, `vadd.vv`, `vsub.vv`, `vmul.vv`, `vfadd.vv`, `vle32.v`/`vse32.v`).
   - **Call-Safety Invariant (C11)**: a loop is only vectorized if its body contains **no `OP_CALL` / `OP_CALL_INDIRECT` / `OP_RET`** (`has_call` guard in `analyze_loop_for_vectorization_full`). The transform widens the induction step (`i += 1` → `i += vec_width`) and converts scalar loads/arith/stores to 128-bit lane ops - a call (or return) in the body cannot be replicated across lanes and would run only once per `vec_width` iterations, silently dropping loop iterations (pre-existing miscompile: `for(...) { float t = use2(1.0f); s += t; }` incremented `k` by 4).
   - **Hardening (`5766bee`)**: loops with a mid-body conditional branch (any `OP_JE/JNE/JL/JLE/JG/JCOND` other than the backedge at `loop->exit_idx`, `has_mid_body_cond`) are rejected, and the increment rewrite only matches the loop counter (`loop_var_temp`), not any `OP_ADD` with constant 1.
   - **Optimization levels**: default is `-O1`. `-O1`: constant_fold, flag_dce, dce, copy_prop, mem2reg. `-O2`: + cse, gvn, licm, small-function inlining. `-O3`: + strength_reduction, branch_simplify, auto_vectorize. `-O0` runs no passes.
   - **Sprint 33 regression RESOLVED (Sprint 35 Phase 0, 2026-10-20)**: the `-O1`+ `break_continue`/`goto_loop` and `-O2`+ `loops_conditionals`/`test/licm/*` miscompiles stemmed from (a) `x86_emit_extend`/`x86_emit_extend_asm` emitting `MOVSXD`/`MOV` with the wrong source register in the ModR/M, (b) `x86_emit_cond_set_asm` hardcoding `%al` and clobbering live `RAX`, (c) the LICM shared-header invariant-loss bug above, (d) LICM hoisting loop-dependent `ADD`s (in-loop source check missing), and (e) DCE single-sweep liveness deleting backedge write-backs. Verified by `make test-opt` (88/88 at `-O0..-O3`, JIT + AOT).
9. **Dead Code Elimination (`opt_dce.c`)**
   - Marks unused temporaries and dead instructions as `OP_DEAD` / `OP_NOP` while preserving instructions with side effects (stores, calls, volatile loads, flag setters consumed by branches).
   - **Fixpoint Liveness (Sprint 35 Phase 0)**: liveness is computed by iterating the backward sweep over a MONOTONIC live set until it stops growing (bounded at 10 sweeps; on non-convergence DCE marks everything live and deletes nothing). A single backward sweep is unsound in the presence of loops: a temp defined at the bottom of the loop (a promoted induction variable's backedge write-back) and used at the top appears dead at its definition on the first sweep, so DCE would delete the update and turn the loop infinite. The pipeline's fixed-point iteration (`opt_run_passes`, up to 10 rounds) re-runs DCE after later passes create new loop-carried register values, which is why this bug only surfaced once CSE/GVN/LICM ran at `-O2`.

## Debugging Optimizations
- Use `--dump-ir` or `--profile-opts` on `./build/bin/zerocc` to inspect IR before and after optimization passes.

## Related Concepts
- Return to [Root Index](./index.md)
- See [Two-Pass IR & Register Allocation](./ir-and-regalloc.md)
- See [Multi-ISA Backends](./isa-backends.md)
