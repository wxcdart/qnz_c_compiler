---
type: Concept
title: Two-Pass IR & Linear Scan Register Allocation
description: IR instruction format, call-argument tracking table, virtual register conventions, and linear scan register allocation constraints.
resource: middleend/ir-and-regalloc
tags: [ir, regalloc, ssa, registers, calling-convention]
timestamp: 2026-10-18T01:38:08Z
generated: antigravity-agent
verified: engineering-lead
status: active
---

# Two-Pass IR & Linear Scan Register Allocation

ZeroCC lowers AST nodes into an ISA-agnostic Two-Pass Intermediate Representation (`src/zero_cc/ir/ir.h`) before running optimizations and register allocation.

## Instruction Representation (`Instr`)

Each `Instr` in `c->ir_arena` has:
- `op` (`Opcode`): One of 51 opcodes (data movement, arithmetic, bitwise, comparison, control flow, calls, frame management, atomic ops, scalar FP, and 128-bit SIMD vector ops).
- `temp_dest`, `temp_src1`, `temp_src2` (`uint8_t`): Virtual register IDs, `0xFF` when unused. User temp IDs start at `VREG_USER_START` (16); `0`–`15` are fixed ABI/scratch vregs and `0xFF` is the no-temp sentinel (see Temp-Id Space below).
- `imm` (`int16_t`) and `imm64` (`int64_t`): Immediate constants, stack frame offsets, or symbol name pointers (for `OP_CALL` and `OP_MOV_GLOBAL`).
- `label_idx` (`uint16_t`): Synthetic branch target label ID.

## Temp-Id Space & the C2a Exhaustion Guard

- Temp IDs are allocated per function from `c->next_temp_id` (reset to `VREG_USER_START` = 16 in `zero_cc_lower_ast`), stored in the **8-bit** `Instr` temp fields. IDs are *not* recycled: every expression in a function draws new IDs, so a long flat function exhausts the space even when few temps are simultaneously live.
- **Exhaustion was silent miscompilation (C2)**: id 255 aliases the `0xFF` sentinel (LRA records no live interval; `regalloc_get_reg_for_temp` returns the default register) and 256+ truncate into `0..15`, colliding with the real ABI vregs. The compiler then *finished successfully* and the generated program segfaulted or infinite-looped.
- **C2a guard** (`ast_lowering.c`, after `lower_node`): if a function's temp IDs reached 255, compilation fails with `function '<fn>' uses <N> temporaries; max is 239 per function (8-bit temp ids, 0xFF = no-temp sentinel). Split the function into smaller helper functions ...`. Verified boundary: a function with 240 temps (e.g. 116 nested calls) still compiles; 241+ gets the diagnostic (no binary emitted). Note the workaround is splitting into *functions*, not *statements* — temp IDs never reset mid-function.
- **C2b (pending permanent fix)**: widen the three temp fields to `uint16_t` and move the sentinel to `TEMP_NONE` (`0xFFFF`); measured blast radius is 152 sentinel sites across 16 files, with LRA/arena already width-safe (`realloc` + `sizeof(Instr)`).

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
  - Virtual registers whose live intervals cross an `OP_CALL` or `OP_CALL_INDIRECT` instruction (`spans_call = 1`) are restricted exclusively to callee-saved GPRs (`%rbx`, `%r12`–`%r15` on x86-64; `x19`–`x28` on AArch64; `s1`, `s2`–`s11` on RISC-V).
  - When all callee-saved GPRs are occupied, `alloc_reg()` returns `-1` and `regalloc_allocate()` evicts or spills the interval to `[frame_base - (spill_base_offset + spill_slot * 8)]` (`c->regalloc.spill_base_offset`) rather than falling back to a caller-saved register.
  - Both JIT machine-code emission (`src/zero_cc/ir/ast_lowering_mc.c`) and AOT assembly emission (`src/zero_cc/ir/asm_emitter.c` and `src/zero_cc/isa/*_asm.c`) transparently reload spilled source operands into `scratch_reg1` / `scratch_reg2` before each instruction (including spilled call arguments and indirect callee pointers) and store spilled destinations back to their spill slot afterward.
  - Prologue and epilogue symmetrically emit `OP_CALLEE_SAVE_STORES` and `OP_CALLEE_SAVE_LOADS` across both JIT and AOT backends (`src/zero_cc/ir/ast_lowering.c`, `src/zero_cc/isa/{x86_64,aarch64,riscv}_asm.c`), preserving `%rbx`/`%r12`–`%r15` (x86-64), `x19`–`x23` (AArch64), and `s1`–`s5` (RISC-V) at `-8(%rbp)`..`-40(%rbp)` so caller registers (such as `main`'s `%rbx` used by `libc`) are never clobbered.
- **Loop Live-Range Extension (`live_range.h`)**:
  - Any virtual register live across a loop header has its live interval extended to the loop exit (`backedge`) so its physical register is not prematurely recycled mid-loop.

## Related Concepts
- Return to [Root Index](./index.md)
- See [Optimization Passes & Safety Invariants](./optimization-passes.md)
- See [System V AMD64 ABI & Struct Calling Conventions](./abi-sysv-x86_64.md)
- See [Multi-ISA Backends](./isa-backends.md)
