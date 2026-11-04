---
type: Specification
title: System V AMD64 ABI & Struct Calling Conventions (sret)
description: Complete x86-64 System V ABI rules implemented in ZeroCC, including register assignment, by-value struct arguments, and hidden pointer (sret) returns for >16 byte structs.
resource: abi/sysv-x86_64
tags: [abi, x86_64, sret, structs, calling-convention, funcptr]
timestamp: 2026-11-04T14:55:01Z
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
- **Effective parameter signature (Sprint 99)**: argument marshalling converts int↔FP and extracts complex real parts against the callee's *declared* parameter types. Direct calls use `ZeroCCFunction::param_types`; **indirect calls (function pointers, pointer-to-function members) derive them from the callee expression's `TypeInfo` pointee** — the `ZeroCCType` function pointer carries its prototype (`fn_params`, see `frontend-and-types.md`), and a `NULL`/empty prototype means "no conversion" (old-style `()`, K&R, implicit decl). Marshalling against the wrong signature silently passes `int 33` in `%edi` where a `double` in `%xmm0` is expected (c-torture `execute/930702-1`).
- **Complex arguments (Sprint 99 no-prototype rule)**: a `_Complex` floating argument occupies **two consecutive FP argument slots** (real then imaginary). The complex→scalar real-part extraction (C99 6.3.1.7) applies **only when the declaration actually names a non-complex parameter at that position** (`param_idx < callee->param_count`); a no-prototype / old-style declaration (`void f();`) names no parameter, and C99 6.5.2.2 default argument promotions keep `_Complex T` complex, so the value must be marshalled as complex — dropping the imaginary half there is a miscompile (c-torture `execute/20010605-2`).
- **Mirror-form subscript `int[(ptr)]` (Sprint 99)**: `N[(int*)y]` (C `a[b] == *(a+b) == b[a]`, pointer as the *index*) lowers to `*(y + N*sizeof(int*))` — the **integer base is scaled by the pointee size** and added to the pointer; scaling the pointer operand instead is a wild dereference (c-torture `execute/pr22061-1`). The element type (load width) comes from the pointer's pointee via the semantic visitor (which must look at *both* operands, not just the base side).
- **VLA parameter row stride (Sprint 99)**: for `T a[...][N]` parameters the runtime row stride is the entry-time-evaluated innermost dim (`ZeroCCSym.vla_slot_off`) × the leaf element size. The 2-D indexing paths must take **parameters** (`is_param`) from that slot; VLA *locals* keep the separate `-stack_offset+16` stride-slot convention written at the declaration (the two conventions must not be mixed — locals also have `vla_slot_off` set for runtime `sizeof`, and using it as the stride regressed c-torture `execute/20221006-1`).
- **VLA struct by value (Sprint 100)**: a struct/union with variably-modified members has a **runtime** object size (the static layout only reserves a 2-element placeholder per VLA member), so it cannot use the size-based chunk/hidden-pointer rules. By-value parameters and arguments occupy **two consecutive GPR units**: (1) the object pointer, (2) the total runtime size. The callee prologue stores the pointer into the parameter's frame slot and the size into the reserved `vla_slot_off`; the trampoline unit model counts the parameter as 2 GPR units. `va_arg(ap, typeof(d))` for such a struct reads the same two GP units and returns the object pointer (the enclosing assignment copies the runtime-sized object via `memcpy`). Member accesses on VLA struct locals/params must load the object pointer from the frame slot and address pointer-relative — the slot holds the pointer, not the object.
- **Struct Arguments Passed by Value (`ast_lowering_expr.c`)**:
  - `<= 8` bytes: Passed in 1 integer register (or stack if no GPR remains).
  - `9 .. 16` bytes: Passed across 2 consecutive integer argument registers (`num_chunks = (size + 7) / 8`) in both caller argument setup (`NODE_CALL`) and callee prologue parameter saving (`OP_PARAM_SAVE_INT`).
  - Multi-eightbyte structs passed by value iterate over all `num_chunks` 8-byte chunks at `-(stack_offset - c * 8)`.
  - **9–16B chunk source for non-identifier arguments (Sprint 56)**: the `NODE_CALL` arg loop derives each extra chunk from the argument expression's value representation — a struct/union **local** (`NODE_IDENT`) is read from the local's stack slot (`-(stack_offset - ch*8)`), while any other 9–16 byte value (compound literal, struct-returning call) picks up chunk 1 from the ABI **secondary value register** (`VREG_2` — `rdx` on x86-64, `x1` on AArch64, `a1` on RISC-V64), the same register a 9–16 byte struct return / compound-literal lowering leaves it in. Argument size comes from the local's `ZeroCCType` or the semantic `TypeInfo` of the argument node.
  - `> 16` bytes: **Hidden-pointer convention (Sprint 64)** — the struct is NOT split across registers. The caller copies the source value into a fresh **16-byte-aligned stack buffer** (via `zc_emit_mem_copy`) and passes **one GP pointer** to it as an ordinary integer argument; the callee prologue saves that pointer and, **after all parameter saves** (and after `ir_emit_variadic_spill` for variadics — the spill must still see the original argument registers), copies the buffer contents into the parameter's local storage (again `zc_emit_mem_copy`; deferring the copy is mandatory — an immediate copy lets the regalloc place copy-loop temps in not-yet-saved argument registers, corrupting later scalar parameters, c-torture `execute/20030914-2`). `va_arg(ap, struct S)` for `sizeof(S) > 16` therefore yields the **hidden pointer itself**, which is why `ast_expr_is_struct_addr` treats such `NODE_VA_ARG` results as address-form. The copies use a real IR loop over 8-byte chunks with a fixed temp set (1/2/4-byte tail stores) — straight-line unrolling a 128 KB struct would need 16,384 temps, over the 16-bit per-function temp-id budget.
  - **Struct-copy tails must copy every byte (Sprint 71)**: every 8-byte-chunked struct copy must handle the `size % 8` remainder completely — `zc_emit_mem_copy` decomposes its tail into **4+2+1** units advancing both pointers (a single 4/2/1-byte tail lost bytes for sizes 31/33/35/42/50); `zero_cc_emit_struct_copy_to_addr` is **source-aware**: local-frame sources copy tail bytes byte-by-byte frame-relative, memory sources reload byte-by-byte (an 8-byte tail load overruns non-8-multiple objects), and register-pair values use `zc_store_low_bytes`. An 8-byte store at a tail offset scribbles the adjacent stack slot — the classic silent-clobber symptom is a later struct/variable in the same frame printing the previous value's bytes.
  - **9–16B secondary-register (`VREG_2`) tails store exactly `size − 8` bytes (Sprint 71)**: the `VREG_0/VREG_2` pair paths (call results, `NODE_ASSIGN` from an address RHS, declaration initializers) must store all `size − 8` remaining bytes of the second register — a single 4/2/1-byte unit drops the rest (an 11-byte struct lost byte 11), and the old declaration-initializer path stored a **full 8-byte word** (adjacent-slot scribble). All of them go through `zc_store_low_bytes` (`ir/ast_lowering_internal.h`): rem 1/2/4 are single narrow stores; rem 3/5/6/7 **spill the value to a scratch frame slot and copy byte-by-byte** — the original shift-based version (value `>> 8·offset` into a fresh temp) hit the latent LRA/redundant-move-elimination collision in large functions: the shift-amount temp's register was reused for a live value, so byte 2 was silently stored as byte 0. Branch-free, no shift temps.
  - **`va_arg(ap, struct S)` > 16 B is exactly one dereference (Sprint 71)**: the `va_list` GP slot holds the **address of the save slot**; the save slot holds the **hidden pointer** to the caller's struct copy. The `va_arg` value must be `*save_slot` (one `ir_emit_deref`) — zero derefs return the address (bogus pointer arithmetic downstream), two derefs return the struct's first 8 bytes (the "looks like data but wrong size" miscompile).
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

## External `va_list` Shadow (glibc x86-64, Sprint 69)

ZeroCC's private `va_list` (64 B: `gp`/`fp`/overflow pointers) is not ABI-compatible with glibc's 16-byte `struct va_list`. Passing it to an **external** variadic libc function (`vprintf`, `vfprintf`, `__vprintf_chk`, …) makes glibc read the wrong register-save region. The call lowering (`ast_lowering_expr.c`, `big_struct_ptr_conv` path) therefore builds a **208-byte glibc-layout shadow** on the caller stack before the call, gated on: arg TypeInfo is a struct tagged `va_list` with size > 16, the callee is a **prototype** (extern — zerocc-compiled callees keep the native layout), and the target is x86-64:

- **Header (32 B)**: `gp_offset`, `fp_offset`, `overflow_arg_area` = the zerocc overflow pointer (the caller's stack args), `reg_save_area` = the shadow's 176-byte region.
- **Region (176 B)**: a glibc-ordered **copy** of the variadic frame's register save area — 6 GP × 8 B + **8** FP × 16 B (low 8 bytes of each double, high zeroed; slots 6..7 zero-filled because glibc sizes the region for 8 XMMs). Slot order is **reversed** relative to the zerocc spill: ZeroCC spills downward from slot 0 (`slot k` at `base-8k`), glibc's `reg_save_area` grows upward from the RDI slot (`region+8l`). FP slots live at `gp_end - 8l` (8-byte doubles).
- **Origin from the `va_list` itself, not the current frame**: `gp_end = *(ap+8)`, `save_abs = gp_end + 48`. This is what makes wrapper forwarding work — `__vprintf_chk(int, const char *, va_list)` forwarding its argument to `vprintf` is not itself variadic and has no spill region; the region address must be carried in the `va_list`.
- **Offsets are branch-free**: `gp_offset = save_abs - gp_ptr` (always in [0,48]; 48 *is* glibc's exhausted value) and `fp_offset = 48 + 2·(gp_end - fp_ptr)` (always in [48,176]; the zero-filled slots 6..7 make the exhausted value read 0.0 instead of running past the buffer). The first implementation used `test/je/jmp` clamps and **miscompiled at -O1+** (redundant-move elimination/LRA dropped the non-exhausted restore and stored `gpd-48` — `vprintf-1` printed `(null)`/segfaulted while O0 was clean); the branch-free form removes the fragile pattern. **Do not reintroduce branches/clamps here without re-testing -O1..-O3.**
- glibc steps `overflow_arg_area` by 16 while the zerocc caller packs stack args at 8 B — overflow (more than 6 GP or 6 FP variadics) to an extern is unsupported; the printf-family corpus cases fit in the register region.
- Companion frontend pieces: `stdio.h` declares the `vprintf`/`vfprintf`/`vscanf`/`vfscanf` family, and `va_end(ap)` is `((void)(ap))` — it must **evaluate** its argument (`va-arg-21` advances a pointer through `va_end(**ap_ptr++)`).
- The ELF writer must not emit **unreferenced** prototype symbols (`main.c`): an undefined symbol for a never-called prototype makes GNU `ld` pull archive members (musl `vfscanf` → missing soft-float helpers on riscv32 → link failure).

## Related Concepts
- Return to [Root Index](./index.md)
- See [Frontend, Type System & Semantic Analysis](./frontend-and-types.md)
- See [Multi-ISA Backends](./isa-backends.md)
- See [Build, Test & Verification Guide](./build-and-verification.md)
