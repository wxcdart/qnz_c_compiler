---
type: Specification
title: System V AMD64 ABI & Struct Calling Conventions (sret)
description: Complete x86-64 System V ABI rules implemented in ZeroCC, including register assignment, by-value struct arguments, and hidden pointer (sret) returns for >16 byte structs.
resource: abi/sysv-x86_64
tags: [abi, x86_64, sret, structs, calling-convention, funcptr]
timestamp: 2026-10-24T22:47:01Z
generated: antigravity-agent
verified: engineering-lead
status: active
---

# System V AMD64 ABI & Struct Calling Conventions (`sret`)

ZeroCC implements the System V AMD64 ABI for x86-64 across both JIT and AOT modes, verified by bidirectional linking with GCC.

## 1. Scalar & Small Struct Calling Conventions

- **Integer / Pointer Arguments (up to 6)**: `%rdi`, `%rsi`, `%rdx`, `%rcx`, `%r8`, `%r9` (indices `0..5`).
- **Floating-Point Arguments (up to 8)**: `%xmm0` .. `%xmm7` (indices `0..7`).
- **FP-arg load ordering invariant (OP_CALL)**: the FP-arg loads (frame-slot → `%xmm0..7`, recorded per call in `LoweringState.call_fp_spills[]`) must be emitted **before** the 7th+ integer arguments are pushed onto the caller stack: those pushes (and the 8-byte odd-count pad) shift the stack pointer, so a frame-relative slot load after them would read the wrong bytes and silently mispass every float argument (symptom: printf prints a stale value while in-program float comparisons are still correct; repro: any call with ≥7 integer args plus a float arg, e.g. `test/jit/varargs_float.c`). Both the machine-code path (`OP_CALL` in `ast_lowering_mc.c`) and the `-S` path (`x86_emit_call_asm` in `x86_64_asm.c`, "Handle FP args first") follow this order.
- **Stack Arguments**: Arguments exceeding the 6 GPR or 8 XMM registers are pushed right-to-left on the caller stack (`16(%rbp)`, `24(%rbp)`, ... in the callee).
- **Struct Arguments Passed by Value (`ast_lowering.c`, `ast_lowering_expr.c`)**:
  - `<= 8` bytes: Passed in 1 integer register (or stack if no GPR remains).
  - `9 .. 16` bytes: Passed across 2 consecutive integer argument registers (`num_chunks = (size + 7) / 8`) in both caller argument setup (`NODE_CALL`) and callee prologue parameter saving (`OP_PARAM_SAVE_INT`).
  - Multi-eightbyte structs passed by value iterate over all `num_chunks` 8-byte chunks at `-(stack_offset - c * 8)`.
- **Struct Return Values (`<= 16` bytes)**:
  - `<= 8` bytes: Returned in `%rax` (`VREG_0`).
  - `9 .. 16` bytes: Lower 8 bytes in `%rax` (`VREG_0`), upper 8 bytes in `%rdx` (`VREG_2`).

---

## 2. Large Struct Returns (`> 16` bytes) via Hidden Pointer (`sret`)

When a function returns a `struct` or `union` with `size > 16` bytes (`fn->ret_struct_sz > 16`):

### Callee Responsibilities (`ast_lowering.c`, `ast_lowering_stmt.c`)
1. **Reserve `sret` Slot in Prologue**:
   - Reserves an 8-byte stack slot at `-fn->sret_stack_offset(%rbp)`.
   - Saves `%rdi` (integer argument register `0`) into `-fn->sret_stack_offset(%rbp)` via `ir_emit_param_save_int(0, -fn->sret_stack_offset)`.
2. **Shift Incoming Integer Parameters**:
   - User integer parameters start at register index `int_idx = 1` (`%rsi`, `%rdx`, `%rcx`, `%r8`, `%r9`), so only 5 user integer parameters fit in registers before spilling to stack.
3. **Copy Return Struct to Caller Buffer (`lower_stmt_return`)**:
   - Loads the saved hidden pointer from `-sret_stack_offset(%rbp)` for each 8-byte chunk `i` and copies local struct data into `[hidden_ptr + i*8]`.
   - **Tail Returns (`return other_call(...)`)**: Passes the callee's saved `sret` pointer directly as argument `0` (`%rdi`) to the inner call, or copies from the returned pointer if the expression was evaluated into a temporary buffer.
4. **Return Hidden Pointer in `%rax`**:
   - Reloads `-sret_stack_offset(%rbp)` into `VREG_0` (`%rax`) before jumping to the epilogue.

### Caller Responsibilities (`ast_lowering_expr.c`, `ast_lowering_stmt.c`)
1. **Allocate Caller `sret` Buffer**:
   - Allocates a 16-byte aligned buffer of `(ret_struct_sz + 7) & ~7` bytes on the caller's stack frame (`sret_buf_off`).
2. **Pass Buffer Address in `%rdi`**:
   - Emits `ir_emit_lea(sret_ptr_temp, BE(c)->fp_reg, -sret_buf_off)` and prepends `sret_ptr_temp` as `arg_temps[0]` (`is_fp[0] = 0`).
   - User integer arguments automatically shift to `%rsi`, `%rdx`, `%rcx`, `%r8`, `%r9` (max 5 user GPR args before stack push).
3. **Indirect Function Pointer Calls (`Task 4.3`)**:
   - When calling through a local function pointer variable (`fn(...)` where `sym->type.tag == TYPE_PTR && sym->type.base && sym->type.base->size > 16`), the caller applies the exact same `sret` buffer allocation and `%rdi` shift.
   - The function pointer value temp is appended to `IRCallArgs` with `is_fp[idx] = 2`.
   - In `x86_emit_call_asm` (`x86_64_asm.c`) and `OP_CALL` (`ast_lowering_mc.c`), the indirect target is first copied into `%r11` (`scratch_reg2`) **before** the parallel `push`/`pop` sequence populates `%rdi`..`%r9`, followed by `call *%r11`.
4. **Size-Aware Tail Chunk Copy (Preventing Stack Corruption)**:
   - When copying a `>16` byte struct from an `sret` buffer into a local struct variable (`Big20 r = fn(...)` or `r = fn(...)`) or out to `[hidden_ptr + i*8]`, if the remaining byte count `rem = size - i * 8` is `< 8` (e.g., `rem == 4` for the 3rd chunk of a 20-byte struct), ZeroCC **must** use `ir_emit_load_32` / `ir_emit_store_32` (or `16`/`byte` variants) rather than an 8-byte `movq`. An 8-byte store at offset 16 of a 20-byte struct would overwrite 4 bytes of the adjacent variable on the stack.

## Caller Stack Arguments & Dynamic SP Rebasing (`zero_cc_frame_disp_at`)

When a function call requires stack arguments (>6 integer/pointer args on x86-64):
- **Alignment Pad & Push Sequence**:
  - The caller reserves 16-byte alignment via `subq $8, %rsp` if `n_stack` is odd (`stack_pad = 8`).
  - Overflow arguments are pushed in reverse order onto `%rsp`.
  - Register arguments (up to 6) are pushed onto `%rsp` and popped into `%rdi..%r9` to resolve arbitrary parallel register swaps safely.
- **Dynamic Stack-Pointer Displacement Invariant**:
  - When frame pointer omission is active (`omit_frame_pointer` is true), slots are addressed relative to `%rsp`.
  - During argument marshalling, `%rsp` shifts downwards by `extra_sp = stack_pad + pushed_stack_args * 8 + pushed_reg_args * 8`.
  - Any reloaded spill slot (for register arguments or stack arguments) must be addressed via `zero_cc_frame_disp_at(c, -off, extra_sp)` (defined in `src/zero_cc/ir/ir.c`), ensuring frame reloads never collide with newly pushed arguments or alignment pad slots.

## Related Concepts
- Return to [Root Index](./index.md)
- See [Frontend, Type System & Semantic Analysis](./frontend-and-types.md)
- See [Multi-ISA Backends](./isa-backends.md)
- See [Build, Test & Verification Guide](./build-and-verification.md)
