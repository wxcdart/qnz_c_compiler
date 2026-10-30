---
type: Concept
title: Two-Pass IR & Linear Scan Register Allocation
description: IR instruction format, call-argument tracking table, virtual register conventions, and linear scan register allocation constraints.
resource: middleend/ir-and-regalloc
tags: [ir, regalloc, ssa, registers, calling-convention]
timestamp: 2026-10-30T00:52:20Z
generated: antigravity-agent
verified: engineering-lead
status: active
---

# Two-Pass IR & Linear Scan Register Allocation

ZeroCC lowers AST nodes into an ISA-agnostic Two-Pass Intermediate Representation (`src/zero_cc/ir/ir.h`) before running optimizations and register allocation.

## Instruction Representation (`Instr`)

Each `Instr` in `c->ir_arena` has:
- `op` (`Opcode`): One of 129 opcodes (data movement, arithmetic, bitwise, comparison, control flow, calls, frame management, atomic ops, OS syscall, scalar FP, and 128-bit SIMD vector ops).
- `temp_dest`, `temp_src1`, `temp_src2` (`uint16_t`): Virtual register IDs, `TEMP_NONE` (0xFFFF) when unused. User temp IDs start at `VREG_USER_START` (16); `0`–`15` are fixed ABI/scratch vregs and `0xFFFF` is the no-temp sentinel (see Temp-Id Space below). 16-bit since Sprint 24 (C2b); the packed struct is 24 bytes, in-memory only.
- `imm` (`int16_t`) and `imm64` (`int64_t`): Immediate constants, stack frame offsets, or symbol name pointers (for `OP_CALL` and `OP_MOV_GLOBAL`).
- `width` (`uint8_t`): C99 narrow-int width mark (0 = none; 1/2/4 = signed byte/halfword/word; 8+1/2/4 = unsigned). When set, the MC layer (`ir/ast_lowering_mc.c`, after the op's own emission) extends `temp_dest` in place to 64-bit with the proper sign, giving true N-bit wrap semantics. Set via `ir_mark_last_width` from the lowering's usual-arithmetic-conversion analysis — and **never** for pointer results: `&expr` operands report no width, so pointer arithmetic keeps its full 64-bit value (a stray width mark here would SEXT32 a pointer and truncate it — see the pointer-exclusion note in the Frontend & Types concept).
- `label_idx` (`uint16_t`): Synthetic branch target label ID.

### Parameter Promotion (`OP_PARAM_LOAD`)

For eligible leaf functions, parameter spills to the stack are bypassed. Instead, incoming parameters are bound to virtual registers using `OP_PARAM_LOAD`:
- `temp_dest`: Destination virtual register.
- `imm`: ABI parameter register index (0–7).
- `imm64`: Floating-point indicator (`1` if FP argument register, `0` if integer/pointer argument register).
During code generation, if `temp_dest` is allocated to the identical physical ABI argument register, the copy is completely omitted (0 instructions). Loop optimizations mark `OP_PARAM_LOAD` with moving side effects to prevent hoist/sinking across blocks.

### Logical `&&` / `||` Lowering (Sprint 41)

`NODE_BINOP` with `TK_LOGAND`/`TK_LOGOR` lowers to a **straight-line** sequence (no embedded branches): each operand is tested with `OP_CMP_ZERO`+`OP_COND_SET(CC_NE)` to a 0/1 temp, then combined with `OP_AND`/`OP_OR` — i.e. `a&&b = (a!=0) & (b!=0)`, `a||b = (a!=0) | (b!=0)`, matching C's 0/1 result. It was previously missing from the `NODE_BINOP` dispatch and fell through to the `default` arm, which returned only the **left** operand (dropping the right operand's comparison — the `while(s[n]&&n<7)` over-run). Straight-line form is deliberate: a branch-based short-circuit (both operands + embedded `je`/`jmp` + labels, as `NODE_TERNARY` uses) confuses the -O2/-O3 CSE/GVN/LICM passes. **Limitation**: no short-circuit, so the right operand is always evaluated (`p && *p` with `p==NULL` still derefs).

## Temp-Id Space (16-bit, C2b) & the Exhaustion Guard

- Temp IDs are allocated per function from `c->next_temp_id` (reset to `VREG_USER_START` = 16 in `zero_cc_lower_ast`), stored in the **16-bit** `Instr` temp fields (C2b, Sprint 24). IDs are *not* recycled: every expression in a function draws new IDs, so a long flat function exhausts the space even when few temps are simultaneously live.
- **Why 16-bit**: the 8-bit era (C2) had id 255 aliasing the `0xFF` sentinel (LRA recorded no live interval; `regalloc_get_reg_for_temp` returned the default register) and 256+ truncating into `0..15`, colliding with the real ABI vregs - the compiler *finished successfully* and the generated program segfaulted or infinite-looped (silent miscompilation). C2b widened the three temp fields, `IRCallArgs.temps[]`, and the sentinel (`TEMP_NONE` = 0xFFFF) at 140+ sites, removed the LRA's legacy 256-temp cap, and widened/guarded the per-temp optimizer tables (DCE live table and GVN tables → 65536; copy-prop keeps a 256-entry map, skipping higher temps as a missed optimization only).
- **C2a guard** (`ast_lowering.c`, after `lower_node`) now bounds at the 16-bit edge: `function '<fn>' uses <N> temporaries; max is 65534 per function (16-bit temp ids, 0xFFFF = no-temp sentinel). Split the function ...` - a safety net, not a practical limit (verified: 400+-temp functions work; 190 nested calls work; the practical ceiling for nested *expressions* is the H1 parser stack at ~depth 200).
- **Base-register marker convention**: `OP_FLOAD`/`OP_FSTORE` (float stack access) and `OP_LEA` (stack address) take a base-register temp operand; the "frame-pointer base" marker is `TEMP_NONE` (0xFFFF) - **not** a real temp id. Pre-C2b the marker was 0xFF, identical to the no-temp sentinel; the C2b sweep would have routed it through `regalloc_get_reg_for_temp(255)` → garbage register (this silently broke float locals in JIT mode until the markers were moved to `TEMP_NONE`).
- **`OP_CALL` args**: the legacy 8-bit-immediate packing of args 2/3 into `instr->imm` was removed with C2b (unrecoverable at 16 bits); the `IRCallArgs` side table is the single source of truth for all 16 args.

## Call Argument Tracking (`IRCallArgsArena`)

Because `OP_CALL` can take up to `IR_CALL_MAX_ARGS` (16) register arguments, call arguments are recorded in a side table `c->ir_call_args` (`IRCallArgs`) keyed by `instr_index`:
- `temps[i]`: The virtual register holding argument `i`.
- `is_fp[i]`:
  - `0`: Integer / pointer argument (assigned sequentially to ABI integer registers).
  - `1`: Floating-point argument (assigned sequentially to ABI FP registers).
  - `2`: **Indirect callee function pointer** (used when `OP_CALL` has `fn_name == NULL`; passed to the backend so the target address is saved into scratch register `%r11` before argument registers are overwritten).
- **Important Invariant**: Any optimization pass that inserts, removes, or rebuilds `c->ir_arena` (such as `opt_loop.c`) **must** remap `instr_index` entries in `c->ir_call_args` so call sites stay synchronized with their `IRCallArgs` entry.

### General-Expression Indirect Calls (Sprint 56)

`NODE_CALL` lowering (`ast_lowering_expr.c`) no longer requires an identifier callee. A callee that is any other expression — `a[i]()`, `p->cb()`, `(*fp)()` — lowers to the same indirect-call machinery used for named function pointers: `is_indirect_call` is true, the callee expression is lowered (`lower_expr(node->left)`) to obtain the pointer value, spilled, reloaded as the final `is_fp = 2` argument, and emitted as `OP_CALL` with `fn_name == NULL`. The call's return type (driving sret / 9–16-byte struct-return / float-result handling) comes from the semantic phase's `TypeInfo` attached to the `NODE_CALL` node (`expr_handle_call` types the callee expression, dereferences pointer-to-function, and stores the function return type). Identifier-only intercepts (`__zerocc_syscall`, `__atomic_*` builtins) still require a literal name. Related: dereferencing a pointer to function (`*fp`) yields a function lvalue whose value **is the pointer** — the `TK_STAR` lowering returns the operand temp without a memory load (the target address holds machine code, not data), which is what makes `(*fp)(args)` callable.

### Canonical Secondary Value Register (VREG_2)

All 9–16 byte value passing (struct returns, compound literal evaluation, by-value struct arguments) uses the **canonical virtual register `VREG_2`** for the second 8-byte chunk. The register allocator maps `VREG_2` to the target ISA's ABI secondary return/argument register: `rdx` (physical 2) on x86-64, `x1` (physical 1) on AArch64, `a1` (physical 11) on RISC-V64/32. This single virtual ID eliminates per-ISA conditionals in lowering and ensures the DCE liveness root set (which marks `VREG_0` and `VREG_2` live at function exit) protects the second chunk.

## OS Syscall Intrinsic (`OP_SYSCALL`, Sprint 43)

`__zerocc_syscall(number, a, b, c, d, e, f)` is the zero_rt Phase 0 OS boundary: a C-visible builtin (name whitelisted alongside `__builtin_*` in the semantic layer, `long` return) that lowers to `OP_SYSCALL` in `ast_lowering_expr.c`'s `NODE_CALL` interception (same mechanism as the `__atomic_*` builtins).

- **Argument packing**: identical to `OP_CALL` — `temp_dest` = arg count (1–7, enforced at lowering), first two temps in `temp_src1`/`temp_src2`, the full list in the `IRCallArgs` side table. The result is `VREG_0` (the ABI return register: `syscall` leaves it in `rax`), so no extra materialize move is needed.
- **Clobber semantics = call semantics**: the LRA (`live_range.h`) records `OP_SYSCALL` as a *call position*, so intervals spanning it get the `spans_call` restriction. This is sound because the syscall clobbers `rax/rcx/rdx/rsi/rdi/r8–r11` — a subset of what a call clobbers. Spilled syscall args are reloaded into `scratch_reg2` (`%r11`), which is already reserved and clobbered by the syscall itself.
- **Parallel-move invariant (x86-64)**: an argument temp may share a physical register with *another* argument's syscall slot (e.g. the count in `%rax` while the number also targets `%rax`), so the backend (`x86_emit_syscall`) moves arguments via a push/pop parallel move (the same idiom as `OP_CALL`), never sequential `mov`s. The text emitter (`x86_emit_syscall_asm`) mirrors it with `pushq`/`popq`.
- **SysV register map**: `eax`=number, `rdi`/`rsi`/`rdx`/`r10`/`r8`/`r9`=args 1–6 (note `r10`, not `rdx`, for the 4th argument — different from the call ABI).
- **Target support**: x86-64 only for now; the dispatcher (`ast_lowering_mc.c`) reports a compile error on backends whose `emit_syscall` slot is NULL (aarch64/riscv). All `OP_CALL` opt-pass special cases (DCE keep/side-effects/liveness, GVN `mf_reset`, mem2reg block-exit + loop safety, LICM `loop_has_call`) include `OP_SYSCALL` in parallel.

## Register Allocation Rules (`src/zero_cc/ir/regalloc.h`)

- **Fixed ABI Return Virtual Registers**:
  - `VREG_0` (`0`) maps to `%rax` (`x0`/`a0`) for primary return values and `sret` pointers.
  - `VREG_2` (`2`) maps to `%rdx` for the upper 8 bytes of 9–16 byte struct returns on x86-64.
  - **Epilogue hazard (Sprint 37)**: the return value sits in `VREG_0` from its materialization in the body until `ret`, but the allocator has no liveness for that physical register, so any code inserted *after* return materialization (e.g. the static-local mirror-out in the shared epilogue block) may have its temps allocated onto the return register and silently clobber it (observed: program exit status = `(uintptr_t)&static_var & 0xFF`). Such code must either shield `VREG_0` through a dedicated frame slot (`lowering.static_local_rt_off`, see the static-local section of `frontend-and-types.md`) or use only the reserved backend scratch registers.
- **Reserved x86-64 Physical Registers (Never Allocatable to General Temps)**:
  - `%rsp` (`4`) and `%rbp` (`5`): Stack and frame pointers.
  - `%rcx` (`1`): Reserved for variable shift counts (`shl`, `shr`, `sar`).
  - `%r10` (`10`) and `%r11` (`11`): Reserved as backend scratch registers (`scratch_reg1` and `scratch_reg2`, used for spill reloads/stores, memory-to-memory moves, stack-arg setup, and indirect `call *%r11`).
- **Call-Spanning Intervals (`spans_call`) & Stack Spilling**:
  - GPR virtual registers whose live intervals cross an `OP_CALL` or `OP_CALL_INDIRECT` instruction (`spans_call = 1`) are restricted exclusively to callee-saved GPRs (`%rbx`, `%r12`–`%r15` on x86-64; `x19`–`x28` on AArch64; `s1`, `s2`–`s11` on RISC-V).
  - **FP/SIMD (C8)**: there are no callee-saved FP registers on any supported ABI, so a `REG_CLASS_XMM` interval with `spans_call = 1` is **always spilled** (`alloc_reg` returns -1) - it can never keep a value across a call in a register. An FP interval spanning a call may also **never steal** a register from an active interval (both spill-selection loops skip such candidates), because every XMM is caller-saved.
  - When all candidate registers are occupied, `alloc_reg()` returns `-1` and `regalloc_allocate()` evicts or spills the interval to `[frame_base - (spill_base_offset + spill_slot * 8)]` (`c->regalloc.spill_base_offset`) rather than falling back to a caller-saved register.
  - **No stealing from already-spilled intervals**: the steal branch copies the stolen interval's `assigned_reg`; once an interval is spilled that field is `-1`, and handing `-1` to the current interval creates a *dangling temp* (no register, no spill slot) that silently reads the fallback register. Both selection loops therefore skip `is_spilled` active intervals, and a `-1` steal falls back to spilling the current interval.
  - Both JIT machine-code emission (`src/zero_cc/ir/ast_lowering_mc.c`) and AOT assembly emission (`src/zero_cc/ir/asm_emitter.c` and `src/zero_cc/isa/*_asm.c`) transparently reload spilled source operands into scratch registers before each instruction (including spilled call arguments and indirect callee pointers) and store spilled destinations back to their spill slot afterward. Spill reload/store is **class-aware**: GPR temps use `scratch_reg1/2` + `emit_load/store`; FP (XMM-class) temps use `fp_scratch_reg1/2` + `emit_load_fp/store_fp` (8-byte movsd).
  - **FP spill scratch registers**: `fp_scratch_reg1/2` = physical 15/14 (`xmm15`/`xmm14`, `d15`/`d14`, `f15`/`f14`) - deliberately outside the allocatable XMM pool (0–7). The indices are shared constants `REGALLOC_FP_SCRATCH1/2` in `regalloc.h`; VEC allocation on x86/AArch64 skips them.
  - **Materialized FP call returns (C8)**: a float/double `OP_CALL` result is materialized into a fresh tracked temp via `OP_FMOV` immediately after the call (`ir/ast_lowering_expr.c`) - the physical return register (`VREG_F0`) is never referenced directly as an operand, so the LRA/regalloc sees the result's live range and spills it across subsequent calls when needed.
  - **XMM↔VEC pool cross-marking (C8)**: VEC-class temps (scalar float locals load via 16-byte `OP_VLOAD`) and XMM-class temps share the same physical registers 0–7 on x86/AArch64. Allocation cross-marks both pools at indices 0–7 (allocate and expire) so an overlapping VEC load can never clobber an XMM temp; on RISC-V the v-registers are separate hardware and the pools stay independent.
  - Prologue and epilogue symmetrically emit `OP_CALLEE_SAVE_STORES` and `OP_CALLEE_SAVE_LOADS` across both JIT and AOT backends (`src/zero_cc/ir/ast_lowering.c`, `src/zero_cc/isa/{x86_64,aarch64,riscv}_asm.c`), preserving `%rbx`/`%r12`–`%r15` (x86-64), `x19`–`x23` (AArch64), and `s1`–`s5` (RISC-V) at `-8(%rbp)`..`-40(%rbp)` so caller registers (such as `main`'s `%rbx` used by `libc`) are never clobbered.
  - **Frame-slot base rebasing (frame pointer omission)**: IR and lowering record frame slots as FP-relative (negative for locals/CSR/parameter saves, positive for the caller's stack arguments) regardless of the frame-pointer policy. When a function omits the frame pointer (`LoweringState.omit_frame_pointer`, see [Multi-ISA Backends](./isa-backends.md) → *Frame Pointer Omission on Fixed-Frame Functions*), every emission site resolves the actual base/displacement through `zero_cc_frame_base_reg(c)` (SP instead of FP) and `zero_cc_frame_disp(c, off)` (`off + frame_delta`), so register allocation and spill-slot bookkeeping remain frame-policy-agnostic. The FP register stays in the reserved (non-allocatable) set even when omitted.
- **Loop Live-Range Extension (`live_range.h`)**:
  - Any virtual register live across a loop header has its live interval extended to the loop exit (`backedge`) so its physical register is not prematurely recycled mid-loop.

## Spill Philosophy: Linear-Scan Speed vs Graph Coloring Trade-Offs

ZeroCC intentionally uses Poletto & Sarkar's Linear-Scan Register Allocation algorithm paired with dedicated scratch registers (`%r10`/`%r11` for GPR, `xmm15`/`xmm14` for FP), rather than iterative Graph Coloring (Chaitin-Briggs) or complex live-range splitting.

### Architectural Rationale & Trade-Offs
1. **Compilation Speed ($O(N)$ vs $O(N^2)$)**:
   - Graph coloring requires building, maintaining, and iteratively updating an interference graph, which takes tens to hundreds of milliseconds.
   - Linear scan allocates physical registers in a single forward pass over sorted intervals in microseconds. The earlier "~2.8 ms (78× faster than GCC)" figure was **retracted in Sprint 35 Phase 4**: re-measured on `test/bench/synth_large.c`, the JIT compiles+runs in ~234 ms vs `gcc -O0` ~164 ms / `gcc -O2` ~365 ms. See [Performance Measurements](./perf-measurements.md) for the full matrix and methodology.
2. **Scratch-Register Spill Safety**:
   - Reserving fixed scratch registers outside the allocatable pool guarantees that spilling a temporary can *never trigger a secondary spill*. This permanently prevents spill-cascade loops and allocator thrashing across all three supported ISAs (x86-64, AArch64, RISC-V).
3. **The Architectural Division of Labor**:
   - Modern 64-bit architectures have ample physical registers (14 allocatable GPRs on x86-64, 30 on AArch64, 32 on RISC-V).
   - In production compiler design, the register allocator is not meant to rescue poor intermediate representation. Hot inner loops typically require only 3 to 6 simultaneously live variables (induction variables, accumulators, loop bounds).
   - The responsibility of keeping loop variables off the stack belongs to the **frontend and IR optimization passes** (`mem2reg`, `licm`). Once `mem2reg` promotes stack slots into virtual register temporaries, the linear-scan allocator naturally places them into physical registers with zero spills.

## CFG Layer (`src/zero_cc/opt/cfg.{h,c}`, Sprint 35 Phase 2)

The IR is a flat, non-SSA linear array with implicit control flow (`OP_LABEL`/`OP_JMP`). Sprint 35 Phase 2 added a **lazily-built, cached CFG view** over the `Instr` array (a pure read-only view — it never writes to the `IRArena`):

- **Blocks**: contiguous `[start,end)` instruction ranges. Leaders: instruction 0, every `OP_LABEL`, and the instruction after any terminal `JMP/JE/JNE/JL/JLE/JG/JCOND/RET/EPILOGUE`. Blocks tile the IR exactly once.
- **Edges**: successors from each block's last instruction (explicit label target + fallthrough for conditional branches; `RET`/`EPILOGUE` have none); predecessor lists derived. Note zerocc emits **no `OP_EPILOGUE` instruction** — the epilogue is the final `CALLEE_SAVE_LOADS` block, and `leave`/`ret` are emitted by the backend outside the IR, so the last block legitimately has no successors.
- **Reachability**: BFS from block 0. Lowering emits unreachable tails after `return`s and orphaned constant blocks (a codegen property, present at `-O0`), so unreachable blocks are *reported*, not an invariant.
- **Loops**: an edge `u->v` is a backedge iff `blocks[v].start <= blocks[u].start` (the natural-loop convention the legacy passes already use); `v` is marked `is_loop_header`. Multiple natural loops may share one header (the Sprint 33 LICM bug class).
- **Caching & invalidation**: `ZeroCCCFG cfg_cache` + `uint64_t cfg_version` on `ZeroCCCompiler`; `cfg_build()` rebuilds only on version mismatch. The version is bumped in `ir_reset()` (per function), after **every** optimizer pass (`opt_pass.c` — passes may rewrite the IR in place), and after the in-place lowering mutations (callee-save append, x86 float-CC remap). Not built at `-O0` unless requested (`-dump-cfg`).
- **`-dump-cfg`**: stable plain-text dump — `fn main` / `bb1: [4..10] succ=bb8,bb2 pred=bb0,bb7 loop_header=1` / `loops:` section / `N blocks, M reachable, K loops` summary. Verified by `tools/cfg_verifier.py` (independent Python recomputation of blocks/loops from `-dump-ir`, cross-checked against the C report; see [Build & Verification](./build-and-verification.md)).
- **Phase 3 (done, 2026-10-20)**: the three known-fragile passes now derive loop structure from the CFG instead of private re-derivations — LICM `detect_loops` iterates `cfg->loops[]` (no `label_to_idx` left); mem2reg `opt_promote_loop_carried` uses CFG pred/succ edges for entry-cleanness and first-exit (unconditional-`OP_JMP` backedges only, as before); the vectorizer's mid-body-conditional check is a per-CFG-block test (plus a verified-constant-loop-bound prerequisite, since the transform has no remainder handling — see [Optimization Passes](./optimization-passes.md)). Full plan & phases: [Development Plan §10](../ZEROCC_Development_Plan.md).

## Known Limitations & Planned Direction (Sprint 35 plan)

The IR is a flat, non-SSA linear array with implicit control flow, temps assigned more than once (ternaries, inlined branches, loop-carried `OP_MOV_REG`), physical registers 0-15 living in the temp namespace, no type tag, and call arguments in an `instr_index`-keyed side table. A CFG view now exists (above), and the three known-fragile passes (LICM, mem2reg loop promotion, vectorizer) derive their loop structure from it as of Sprint 35 Phase 3; **dominators and out-of-SSA are not implemented**. Unmeasured claims (e.g. JIT-vs-GCC compile speed) must be re-measured before being relied on.

## Related Concepts
- Return to [Root Index](./index.md)
- See [Optimization Passes & Safety Invariants](./optimization-passes.md)
- See [System V AMD64 ABI & Struct Calling Conventions](./abi-sysv-x86_64.md)
- See [Multi-ISA Backends](./isa-backends.md)
