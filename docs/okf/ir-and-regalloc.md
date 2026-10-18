---
type: Concept
title: Two-Pass IR & Linear Scan Register Allocation
description: IR instruction format, call-argument tracking table, virtual register conventions, and linear scan register allocation constraints.
resource: middleend/ir-and-regalloc
tags: [ir, regalloc, ssa, registers, calling-convention]
timestamp: 2026-10-18T13:14:02Z
generated: antigravity-agent
verified: engineering-lead
status: active
---

# Two-Pass IR & Linear Scan Register Allocation

ZeroCC lowers AST nodes into an ISA-agnostic Two-Pass Intermediate Representation (`src/zero_cc/ir/ir.h`) before running optimizations and register allocation.

## Instruction Representation (`Instr`)

Each `Instr` in `c->ir_arena` has:
- `op` (`Opcode`): One of 51 opcodes (data movement, arithmetic, bitwise, comparison, control flow, calls, frame management, atomic ops, scalar FP, and 128-bit SIMD vector ops).
- `temp_dest`, `temp_src1`, `temp_src2` (`uint16_t`): Virtual register IDs, `TEMP_NONE` (0xFFFF) when unused. User temp IDs start at `VREG_USER_START` (16); `0`–`15` are fixed ABI/scratch vregs and `0xFFFF` is the no-temp sentinel (see Temp-Id Space below). 16-bit since Sprint 24 (C2b); the packed struct is 24 bytes, in-memory only.
- `imm` (`int16_t`) and `imm64` (`int64_t`): Immediate constants, stack frame offsets, or symbol name pointers (for `OP_CALL` and `OP_MOV_GLOBAL`).
- `label_idx` (`uint16_t`): Synthetic branch target label ID.

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

## Register Allocation Rules (`src/zero_cc/ir/regalloc.h`)

- **Fixed ABI Return Virtual Registers**:
  - `VREG_0` (`0`) maps to `%rax` (`x0`/`a0`) for primary return values and `sret` pointers.
  - `VREG_2` (`2`) maps to `%rdx` for the upper 8 bytes of 9–16 byte struct returns on x86-64.
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
- **Loop Live-Range Extension (`live_range.h`)**:
  - Any virtual register live across a loop header has its live interval extended to the loop exit (`backedge`) so its physical register is not prematurely recycled mid-loop.

## Related Concepts
- Return to [Root Index](./index.md)
- See [Optimization Passes & Safety Invariants](./optimization-passes.md)
- See [System V AMD64 ABI & Struct Calling Conventions](./abi-sysv-x86_64.md)
- See [Multi-ISA Backends](./isa-backends.md)
