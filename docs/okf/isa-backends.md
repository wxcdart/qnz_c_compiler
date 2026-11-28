---
type: Architecture
title: Multi-ISA Backends (x86-64, AArch64, RISC-V) & Linker/JIT
description: Backend vtable interface (ZeroCCISABackend), JIT machine code emission, AOT assembly generation, parallel argument moves, and symbol relocation resolution.
resource: backend/isa-backends
tags: [backend, x86_64, aarch64, riscv, jit, aot, elf, relocations]
timestamp: 2026-11-28T16:02:54Z
generated: antigravity-agent
verified: engineering-lead
status: active
---

# Multi-ISA Backends (x86-64, AArch64, RISC-V) & Linker/JIT

ZeroCC abstracts target-specific code generation behind `ZeroCCISABackend` (`src/zero_cc/isa/isa_backend.h`), supporting four architecture families (5 targets):
- **x86-64**: `src/zero_cc/isa/x86_64.c` (JIT binary emission) & `src/zero_cc/isa/x86_64_asm.c` (AOT `.s` assembly emission)
- **AArch64**: `src/zero_cc/isa/aarch64.c` & `src/zero_cc/isa/aarch64_asm.c`
- **AArch32 (ARMv7-A & Thumb-2 Cortex-M)**: `src/zero_cc/isa/aarch32.c` & `src/zero_cc/isa/aarch32_asm.c` (Sprint 185 ARM32 MC/AOT; Sprint 275 Thumb-2 `-mthumb` Cortex-M0/M3/M4/M7, IT blocks, interworking `blx`)
- **RISC-V (RV64GC)**: `src/zero_cc/isa/riscv64.c` & `src/zero_cc/isa/riscv_asm.c`
- **RISC-V (RV32GC / RV32I Soft-Float)**: `src/zero_cc/isa/riscv32.c` (Sprint 170 rv32 i64 GPR64 pairs; Sprint 276 freestanding soft-float runtime in `zero_rt_riscv32.a`)
- **Shared RISC-V encoders**: `src/zero_cc/isa/riscv_common.h`
- **Backend registry**: `src/zero_cc/isa/backend_registry.c`

## Lazy Backend Initialization

The global ISA→vtable registry lives in `src/zero_cc/isa/backend_registry.c` (Sprint 114: extracted from `x86_64.c` so no single ISA emitter owns the lookup). Backend structs (`x86_64_backend`, `aarch64_backend`, `riscv64_backend`, `riscv32_backend`) are non-`const` and lazily wired with their assembly emitter callbacks via `zero_cc_init_backends_once()` inside `zero_cc_get_backend()` and `zero_cc_get_native_backend()`.

**RV32 vtable copy-initialization (Sprint 114)**: the RV32 backend reuses the RV64 MC emitters (they branch on `c->isa` at the 32/64-bit boundary, e.g. `lw`/`sw` vs `ld`/`sd`); the only RV32-specific MC code is the 4-byte prologue/leave/epilogue/push/pop frame sequence. Because the two vtables differ in only ~10 fields, `riscv32_init_backend()` (called first inside `zero_cc_init_backends_once()`, before the asm-pointer fill-in) copies the fully static-initialized `riscv64_backend` into `riscv32_backend` and overrides `isa`/`conv`/frame functions, zeroing the RV64-only GPR128 scratch pool and `emit_mul128_*` helpers. This keeps a single definition of every shared emitter in `riscv64.c` (all `static`); the one cross-TU call is `riscv64_emit_mov_imm` (large-frame immediates in the RV32 prologue), declared extern.

### Vtable signature conformance (Sprint 41)

Every backend callback **must** match its `ZeroCCISABackend` slot signature exactly — a mismatch compiles with only an `-Wincompatible-pointer-types` warning but reads **garbage** for the extra parameters at the call site (the caller passes the slot's argument list, not the function's). This bit the RISC-V backend: `riscv64_emit_load_32/16/byte` and `riscv64_emit_store_32/16/byte` carried a trailing `int32_t offset` the slot `emit_*_32/16/byte(c, dst, src)` never supplied (so the offset field was uninitialized stack), and `riscv64_emit_lea_rip` took `const char *label` where the slot `emit_lea_rip(c, dst, size_t rodata_offset)` passes a size_t. Both are now conformed (loads/stores use offset 0, matching x86's `mov (src), dst`; `lea_rip` takes `size_t rodata_offset`). Build the tree clean (`make clean`) to surface any future signature drift as warnings.

### Direct Raw Binary Object Emission Default (Sprint 41)

All four target ISAs (x86-64, aarch64, riscv64, riscv32) default to direct **RAW binary object emission** via `elf_writer` (`src/zero_cc/jit/elf_writer.c`) for both `-c` and `-o` compilation in `src/main.c`.

- **`-o`** (`compile_and_link`): generates a binary relocatable object `.o` via `zero_cc_emit_elf_obj` (`elf_writer`), then invokes the target system cross-linker (`gcc` / `ld`) with `-static` to link against libc/musl.
- **`-c`**: writes the relocatable `.o` directly via `elf_writer` (ELF64 for x86-64, aarch64, riscv64; ELF32 for riscv32).
- **Escape Hatch (`ZC_TEXT_AOT=1`)**: When `ZC_TEXT_AOT=1` is set in the environment, compilation routes through the text assembly path (`aarch64_asm.c` / `riscv_asm.c`) and system cross-assembler (`compile_object_cross`).

### Computed Goto Indirect Jumps & Label Address Emission (Sprint 84)

The backend interface `ZeroCCISABackend` defines two operations for computed gotos:
- `void (*emit_jmp_reg)(ZeroCCCompiler *c, int reg)`: Emits an indirect jump to the address in physical register `reg`.
  - **x86-64**: `0xFF /4` (`jmp *%reg`, rex.W if reg >= 8).
  - **AArch64**: `br xN` (`0xD61F0000 | (reg << 5)`).
  - **RISC-V**: `jalr zero, 0(reg)` (`0x00000067 | (reg << 15)`).
- `size_t (*emit_lea_label)(ZeroCCCompiler *c, int dst_reg)`: Emits a PC-relative address computation targeting a code label, returning the byte offset of the displacement field to be patched:
  - **x86-64**: `leaq disp32(%rip), %dst` (opcode `0x8D`, ModR/M `0x05 | (dst << 3)`). Patched with `rel32 = target_offset - (patch_pos + 4)`.
  - **AArch64**: `adr xN, 0` (`0x10000000 | dst`). Patched with `imm21 = target_offset - patch_pos`.
  - **RISC-V**: `auipc dst, 0` + `addi dst, dst, 0`. Patched with `hi20` + `lo12` sign-adjusted pair.

### Multi-ISA Inline `OP_SETJMP` & `OP_LONGJMP` Emission

Both the raw binary emitter (`ast_lowering_mc.c`) and the text assembly emitter (`asm_emitter.c`) emit inline machine instructions for `OP_SETJMP` and `OP_LONGJMP` across all 4 target ISAs (`x86-64`, `aarch64`, `riscv64`, `riscv32`), using `BE(c)->emit_lea_label` + `BE(c)->patch_jmp` for the resume label:
- **`OP_SETJMP(buf_reg)`**:
  - Stores `FP` (`%rbp`/`x29`/`s0`) at `0(buf_reg)`.
  - Computes the PC-relative resume label address into `scratch_reg1` (`%r10`/`x16`/`t1`) via `emit_lea_label` and stores it at `word_size(buf_reg)` (`8` on 64-bit ISAs, `4` on `riscv32`).
  - Copies `SP` (`%rsp`/`sp`) into `scratch_reg1` (using `add Rd, sp, #0` on AArch64 since register 31 in `orr` encodes `xzr`) and stores it at `2 * word_size(buf_reg)`.
  - Zeros `VREG_0` (`%rax`/`x0`/`a0`) on the initial path; the resume label lands immediately after.
- **`OP_LONGJMP(buf_reg, val_reg)`**:
  - Copies `buf_reg` into `scratch_reg1` (`%r10`/`x16`/`t1`) and `val_reg` into `VREG_0` (`%rax`/`x0`/`a0`) carefully avoiding register overlap when `buf_reg` is already in `VREG_0`.
  - Reloads `FP` from `0(scratch_reg1)`, `SP` from `2 * word_size(scratch_reg1)`, and target `PC` from `word_size(scratch_reg1)` into `scratch_reg2` (`%r11`/`x17`/`t2`), then jumps indirectly via `emit_jmp_reg(scratch_reg2)`.

**Measured state (`test/isa_qemu/` battery via `make test-isa-qemu` / `tools/isa_qemu_diff.py`):** all 4 target ISAs pass 100% cleanly (**48/48 PASS**): x86-64 (**12/12**), aarch64 (**12/12**), riscv64 (**12/12**), and riscv32 (**12/12**). For aarch64, stack frame setup, relocation pairs (`R_AARCH64_ADR_PREL_PG_HI21` / `R_AARCH64_ADD_ABS_LO12_NC`), byte/halfword stores, variable shifts, and FP instruction encodings were repaired. For riscv32, the ELF32 object emitter (`ELFCLASS32`, `EM_RISCV`), calling conventions, 4-byte frame setup (`sw`/`lw`), shifts, width extensions, and 5-8 byte struct return register pair (`a0`/`a1`) are fully functional.

## Multi-ISA `sret` (>16B Struct Return) Conventions

Across target ISAs, structs exceeding 16 bytes return via an implicit caller-allocated buffer pointer:
- **x86-64 (System V AMD64 ABI)**:
  - Caller allocates stack buffer and passes its pointer in `%rdi`.
  - Regular integer arguments shift starting from `%rsi` (index 1).
  - Callee writes struct to `[%rdi]` and returns the buffer pointer in `%rax`.
- **AArch64 (ARM64 AAPCS64 ABI)**:
  - Caller allocates buffer and passes its pointer in `x8` (dedicated indirect result location register).
  - Regular arguments start at `x0` without register consumption/shifting.
  - Callee preserves `x8` in its prologue (`param_save_int_asm(8, sret_offset)`) and returns the pointer in `x0`.
- **RISC-V (RV64GC LP64D ABI)**:
  - Caller allocates buffer and passes its pointer in `a0` (first integer argument register).
  - Regular integer arguments shift starting from `a1` (index 1).
  - Callee writes struct to caller buffer and returns pointer in `a0`.

## Prologue & Epilogue Management

ZeroCC manages function stack frames and register preservation symmetrically across JIT and AOT code paths:

1. **Selective Callee-Saved Register Spilling**:
   - In both machine code (`ast_lowering_mc.c`) and assembly emission (`x86_64_asm.c`, `aarch64_asm.c`, `riscv_asm.c`), ZeroCC checks `c->regalloc.used_callee_saved_gpr_mask` populated during register allocation.
   - Functions that do not clobber callee-saved registers (such as leaf functions or small arithmetic routines) emit **0** CSR stores in the prologue and **0** CSR loads in the epilogue (saving up to 10-11 spills/reloads on AArch64 and RISC-V).
2. **Fallthrough Jump Elimination**:
   - Both `asm_emitter.c` and `ast_lowering_mc.c` look ahead past dead/nop instructions on `OP_JMP`. If the target label immediately succeeds the jump, the branch instruction (`jmp`, `b`, `j`) is omitted and execution falls through directly into the label.
3. **x86-64 System V Red Zone (128-byte Window)**:
   - For leaf functions on x86-64 (`!has_calls`) with total stack requirements $\le 128$ bytes, stack allocation is serviced within the 128-byte Red Zone below `%rsp`. `subq $N, %rsp` is completely omitted in the prologue.
4. **Inline Epilogues on Early Returns**:
   - Early return guards in functions without static-local mirror blocks inline the epilogue directly (`leave; ret` or `ret`) instead of emitting an unconditional jump `jmp .L_epilogue`, eliminating branch latency on early exit paths.
5. **Frameless Leaf Functions & Direct Parameter Register Promotion (AArch64, RISC-V, x86-64)**:
   - For leaf functions (`!has_calls`) with no local stack variables (`c->local_count == fn->param_count`), no address-of parameter (`&param`), and no parameter mutations (assignments, increments, decrements), incoming ABI argument registers are directly promoted into virtual registers (`sym->is_promoted = true`, `sym->promoted_vreg`) via `OP_PARAM_LOAD`.
   - Frame pointer setup (`push rbp` / `stp x29, x30` / `addi sp, sp, -16; sd ra, s0`), stack frame allocation (`c->current_stack_frame = 0`), and stack parameter spill stores/loads are completely bypassed.
   - The function emits direct computation straight to return register followed by a single `ret`. Supported up to 3 integer arguments on x86-64 (matching `rdi, rsi, rdx` without clobber hazards against reserved `rcx`) and up to 8 arguments on AArch64 and RISC-V (`x0..x7` / `a0..a7`).
6. **Frame Pointer Omission on Fixed-Frame Functions (`-fomit-frame-pointer`, default at `-O1`+)**:
   - Functions whose frame is fully static — not frameless, no runtime VLA (`OP_VLA_ALLOC`) — omit the frame pointer and address every frame slot from the stack pointer with a constant per-function displacement. Policy is decided once per function in `ast_lowering.c` and stored in `LoweringState` (`omit_frame_pointer`, `frame_delta`). The decision mirrors GCC: omitted by default at effective `-O1` and above, kept at `-O0` unless the user forces it.
   - CLI: `-fomit-frame-pointer` (force omit) and `-fno-omit-frame-pointer` (force keep) are parsed in `src/main.c` and forwarded to `ZeroCCCompiler.omit_frame_pointer`.
   - **Slot rebasing**: IR/lowering continue to record frame slots as FP-relative. All frame-slot emission sites (machine + asm, all four ISAs, including CSR saves, promoted/stack parameter saves, and variadic spills) go through `zero_cc_frame_base_reg(c)` / `zero_cc_frame_disp(c, off)` (declared in `isa_backend.h`, implemented in `ir.c`): with omission the base becomes SP and the displacement becomes `off + frame_delta`.
   - **Per-ISA geometry** (`frame_delta` = old FP − final SP):
     - x86-64: prologue `sub rsp, X` (X = `frame_delta + 8`); `frame_delta = X - 8` (i.e. `alloc_frame_size` normally, `-8` for the red-zone leaf case with X = 0). Epilogue `add rsp, X` (emitted by both `emit_epilogue` and `emit_leave`, the latter being the real final-epilogue hook — an FP-less epilogue that only `ret`s leaks the frame and corrupts the caller's stack).
     - AArch64: prologue `str x30, [sp, #-16]!` + `sub sp, sp, #aligned`; `frame_delta = aligned`. Epilogue restores CSRs, `add sp, sp, #aligned`, `ldr x30, [sp], #16`. For `aligned > 4095` the epilogue uses `add x16, sp, x16; mov sp, x16` because SP is not a valid Rd of ADD (extended) (Rd = 31 encodes XZR).
     - RISC-V (64 & 32): prologue `addi sp, sp, -(aligned+16)` + `sd/sw ra, (aligned+8)(sp)`; `frame_delta = aligned`. Epilogue `ld/lw ra`, `addi sp, sp, aligned+16`. The FP is kept (fallback) when `aligned + 16 > 2047` to stay inside the `addi` immediate range.
   - **C11 `_Alignas(N>16)` realign mode** (Sprint 95 x86-64, Sprint 96 MC + x86/AArch64/RISC-V ASM, Sprint 194 AArch32 `-S` & x86-64 signed mask parity): when the max local alignment N > 16 (power of two, ≤ 4096; no VLA/variadic/frame-address), the frame is rounded to N and SP itself is realigned at entry so slots are actually N-aligned. `LoweringState.frame_align` holds N; all 5 target configurations (`x86-64`, `aarch64`, `aarch32`, `riscv64`, `riscv32`) emit the realign prefix and use the omit-FP slot geometry (`frame_delta = frame`, base = SP) across both raw MC and `-S` assembly paths:
     - x86-64: `push rbp; mov rbp, rsp; and $-N, %rsp; sub $frame, %rsp` (Sprint 194: `x86_64_asm.c` emits signed `$-N` instead of positive `$0x...`, which GNU `as` rejected for 64-bit `andq`); epilogue = plain `leave` (rbp kept at entry-SP-8).
     - AArch64: `stp x29,x30,[sp,#-16]!; mov x29,sp; mov x16,sp; and x16,x16,#-N; sub x16,x29,x16; sub sp,sp,x16; sub sp,sp,#frame` (SP cannot be the destination of a logical op → mask through x16/IP0); epilogue = plain FP-mode `mov sp,x29; ldp x29,x30,[sp],#16`.
     - AArch32: `push {r11, lr}; add r11, sp, #0; bic sp, sp, #(N-1)` (or `movw`/`movt` scratch `r12` + `bic sp, sp, r12` for large masks), `sub sp, sp, #frame`, CSR/param/variadic accesses rebased via `zero_cc_frame_base_reg`/`zero_cc_frame_disp` with large-offset scratch fallbacks in both MC and `-S` (`aarch32.c`), `OP_MOD` `-S` lowering (`udiv`/`sdiv` + `mul` + `sub`), and `.arch armv7ve` + `.fpu vfpv3-d16` emitted at the top of AArch32 `.s` files (`src/main.c`); epilogue restores CSRs, `mov sp, r11; pop {r11, lr}`.
     - RISC-V (64 & 32): `addi sp,sp,-16; sd/sw ra,8(sp)/4(sp); sd/sw s0,0(sp); mv s0,sp; andi sp,sp,-N (N<4096) or li t0,-N; and sp,sp,t0 (N=4096); sub sp,sp,#frame` (register-sub for frame > 2047); epilogue = plain leave (`addi sp,s0,0; lw/ld ra; lw/ld s0; addi sp,sp,16`). **RV32 note**: the ra save slot is at +4 (not +8) to match `riscv32_emit_leave`.
     - **AArch64 AND-mask encodings**: the realign mask MUST be the 64-bit logical-immediate form (`and x16, x16, #-N`, SF=0, e.g. N=32 → `0x927BEA10`). The 32-bit literal form (`#0xffffffe0`, SF=1) zero-extends into x16 and destroys SP's upper half — QEMU matches real hardware. The `aarch64.c` table carries a comment about this pitfall.
     - **RISC-V N=4096**: `(~(N-1)) & 0xFFF == 0` would emit `andi sp, sp, 0` (zeroes SP) — both MC (`riscv.c`) and ASM (`riscv_asm.c`) fall back to `li t0, -N; and sp, sp, t0` (t0 is free at function entry).
     - **RISC-V RV64 funct7 traps** (hit while porting): `sub` = f7 `0x20` (0b0100000), NOT f7 1 (that is `mul`); `and` = f3 7, f7 0. The `rv64_R(rd, rs1, rs2, f3, f7)` helper takes f7 last — passing the opcode `0x33` there silently produces an illegal instruction (SIGILL under QEMU).
   - **Large SP-relative displacements** (frames > 4KB, e.g. `_Alignas(4096)` locals):
     - AArch64 `aarch64_emit_lea` (MC) must keep large offsets in the ADD/SUB *immediate* form — SP is **not** a legal Rm of ADD (register) (register 31 there is XZR, so `add dst, dst, sp` silently becomes `add dst, dst, xzr` and the SP vanishes). Offsets > 4095 use the scaled-immediate form (`#imm, LSL #12`, bit 22), or a two-instruction immediate split; only non-SP bases may use mov+register-add. `emit_load`/`emit_store`'s large-offset fallback had the same latent bug (a pre-set Rm bit in the constant made it `add x16, sp, x18`) — fixed.
     - RISC-V S/I-type immediates are 12-bit (±2047): `riscv64_emit_load/store` (both RV64 and RV32 — the RV32 vtable now uses the ISA-aware RV64-named helpers) and the ASM-path load/store/param-save/CSR-restore all fall back to a scratch-register base (`li t0, off; add t0, base, t0; st/ld, 0(t0)`) when the displacement exceeds the immediate range.
     - **binutils 2.41 RISC-V `sym+offset` bug**: `lla dst, .Lrodata+off` (and `%pcrel_hi(sym+off)`/`%pcrel_lo(1b)`) compute the wrong address (LO12 reloc references a phantom `.L0`; verified off-by-one against a plain-label control). The `-S` RISC-V rodata pointer loads (`asm_emitter.c` OP_LEA_RIP / OP_FMOV_IMM) therefore emit `lla t0, .Lrodata` + `addi/add` with the offset instead of a symbol+offset reference. AArch64 `adrp x9, .Lrodata+off` is unaffected (verified).
   - **Scope**: the frame-pointer register (`%rbp` / `x29` / `s0`) is currently only *not set up*; it is not yet handed to the register allocator as a general-purpose register, so the win is prologue/epilogue cost, not register pressure.
   - Verified by the QEMU raw-emitter battery (63/63) and the extended `prologue_epilogue_verifier.py` (see below).
7. **Verification Tools**:
   - `python3 tools/verify/prologue_epilogue_verifier.py` verifies selective CSR omission, frameless leaf emission (both pure leaf and leaf with promoted arguments), Red Zone usage, omit-frame-pointer shapes (FP-free prologue/epilogue with balanced SP adjustments and LR/ra save-restore on all four ISAs), and store-load symmetry for both FP-based (`[x29, #-N]` / `N(s0)` / `N(%rbp)`) and SP-based (`[sp, #N]` / `N(sp)` / `N(%rsp)`) CSR slots.
   - `python3 tools/verify/extension_verifier.py` verifies GNU statement expressions, switch case ranges, designated initializer ranges, and `__builtin_offsetof`.

## Parallel Argument Setup in Function Calls

When setting up arguments for `OP_CALL`, source virtual registers may reside in physical registers that overlap with destination ABI argument registers (e.g., arg 1 currently in `%rdi` while `%rdi` must receive the `sret` pointer).
- **AOT (`x86_emit_call_asm` in `x86_64_asm.c`)**:
  1. If the call is indirect (`fn_name == NULL`), moves the callee address register into `%r11` first.
  2. Pushes all integer argument source registers onto the stack in reverse order (`n_int - 1` down to `0`), then pops them into their target ABI registers (`%rdi`, `%rsi`, `%rdx`, `%rcx`, `%r8`, `%r9`).
  3. Emits `call fn_name` or `call *%r11`.
- **JIT (`OP_CALL` in `ast_lowering_mc.c`)**:
  1. If the call is indirect (`fn_name == NULL`), copies the callee address register into `BE(c)->scratch_reg2` (`%r11`) before touching argument registers.
  2. Uses push/pop or register moves for integer/FP arguments, then emits `BE(c)->emit_call_reg(&c->code_buf, BE(c)->scratch_reg2)`.

## Symbol & Function Address Resolution (`OP_MOV_GLOBAL`)

When taking the address of a function (`fn` or `&fn` assigned to a function pointer), `ast_lowering_expr.c` emits `ir_emit_mov_global(temp, fn_name)`:
- **AOT Assembly (`asm_emitter.c`)**:
  - Checks `zero_cc_find_function(c, sym_name)`. If it is a function symbol, emits `leaq sym(%rip), %reg` to load the function's code address (whereas global data variables emit `movq var(%rip), %reg` to load the variable's value).
- **AOT ELF Object / Executable (`src/main.c`, `src/zero_cc/jit/elf_writer.c`)**:
  - Resolves `c->relocs[i]` against `c->globals` (`.data`), `c->functions` (`.text`), and referenced external variables (`extern FILE *stderr;` emitted with `ELF_STT_NOTYPE`).
  - **Named object & function sections (Sprint 107/108)**: globals with `section[0] != 0` (GNU `__attribute__((section("name")))`) are grouped by section, laid out (aligned) in a dedicated `PROGBITS` (`NOBITS` if all BSS-like) section, the bytes copied out of `.data`, and each object symbol emitted in the named section at `named_sec_off`. Functions with `section[0] != 0` are copied out of `.text` (their `[text_offset, next-function-text_offset)` extent) into a dedicated `PROGBITS|EXECINSTR` section and their symbol emitted there at `named_text_sec_off`. Relocations are symbol-based so references resolve to the named section; the dead `.data`/`.text` copies are left in place. See [Frontend & Types -> Named Object & Function Sections](./frontend-and-types.md).
  - When `elf_writer_write()` partitions `ELF_STB_LOCAL` symbols ahead of `ELF_STB_GLOBAL` symbols in `.symtab`, it builds `sym_remap[]` and updates every `.rela.text` relocation's `ELF64_R_INFO(new_sym, r_type)` so symbol indices remain synchronized.
- **JIT Execution (`src/zero_cc/jit/execution.h`, `src/zero_cc/types/symbol_mgmt.h`)**:
  - Resolves `c->relocs[i]` by checking `c->globals`, then `zero_cc_find_function(c, sym_name)` (computing `(void*)(c->code_buf.data + fn->code_offset)`), and finally `zero_cc_resolve_symbol(sym_name)` (`&stdin`, `&stdout`, `&stderr`, or `dlsym`).

## Conditional Branch Semantics (`OP_JE`/`OP_JNE`)

`OP_JE`/`OP_JNE` carry a **condition temp** (the result of `OP_COND_SET` or a 0/1 value), not raw flags. The x86-64 emitter lowers them as `test reg, reg` + `je`/`jne` — i.e. **jump when the temp is zero / non-zero**. Consequences:
- "jump if condition TRUE" requires `ir_emit_jne(cond_temp, label)`; "jump if FALSE" uses `ir_emit_je` (as in `lower_stmt_if`, where the JE target is the *else* branch and the then-branch is the fallthrough).
- Dispatch chains (e.g. `switch` in `lower_stmt_switch`) must use `ir_emit_jne(flag, case_label)`.

## `OP_CMP_ZERO` Is ISA-Specific in the `-S` Path

`OP_CMP_ZERO` (sets the condition for a following `OP_COND_SET`; used by `!expr`, `expr==0/!=0`, and `&&`/`||`) has **ISA-dependent** emission in `asm_emitter.c` (Sprint 41: it was x86-hardcoded `testq`, which broke aarch64/riscv `-S`): x86-64 `testq %r,%r`, aarch64 `cmp r,#0` (NZCV for the next `cset`), riscv `mv t0,r` (the riscv `snez`/`seqz` cond-set reads the `t0` scratch, same convention as `emit_cmp_asm`'s `sub t0,a,b`). The x86-64 JIT/AOT binary path handles `OP_CMP_ZERO` in its own MC emitter and is unaffected.

## Global Data Reloc Patching (JIT)

`ZeroCCDataReloc` entries (pointer initializers `const char *g = "...";`, `int *p = &x;` — see [global-data-relocs.md](./global-data-relocs.md)) are patched in `execution.h` **after** the data `memcpy`: 8-byte absolute addresses written into the still-writable data page (`(char*)mem + data_offset + patch_offset`), targeting the data page (kind 1) or `(char*)mem + text_section.length + rodata_offset` (kind 0). The RX transition happens afterwards, so the patched pointers are valid at run time.

## Integer Width Extension (`OP_SEXT*`/`OP_ZEXT*`) & the x86 Byte-Register Encoding Trap

In-place width-extension opcodes (`OP_SEXT8/16/32`, `OP_ZEXT8/16/32`, appended at the **end** of the `Opcode` enum to preserve existing values) are emitted via the `emit_extend` vtable slot (binary) and `emit_extend_asm` (the `-S` path):

- **x86-64** (`x86_64.c` `x86_emit_extend`): `sext32` = `movsxd`; `zext32` = 32-bit self-`mov` (there is **no** `MOVZX r64, r/m32`); `sext16` = `movsx r32, r/m16` (`0F BF`, **not** `0F BE` which is the byte form) + `movsxd`; `sext8` = `movsx r32, r/m8` (`0F BE`) + `movsxd`; `zext16` = `movzx r64, r/m16`; `zext8` = `movzx r64, r/m8`.
- **⚠️ Byte-register ModRM trap**: for 8-bit register r/m operands the legacy ModRM 0-7 field is **not** 1:1 with the register index (verified against GNU `as`: `%bl`=`0F B6 C3`, `%dh`=`0F B6 C6`, `%sil`=`40 0F B6 C6`, `%dil`=`40 0F B6 C7`, `%bpl`=`40 0F B6 C5`, `%spl`=`40 0F B6 C4`). In ZeroCC's numbering (0=rax…3=rbx, **4=rsp, 5=rbp, 6=rsi, 7=rdi**), registers 4-7 need the REX **X** bit and 8-15 the REX **B** bit with rm = reg&7. An un-mapped encoding silently reads the wrong byte (e.g. `rm=3` without REX reads BL on a 32-bit dest, but is the *only* legal encoding for BL there) — every byte-register operand must go through the mapping table in `x86_emit_extend`.
- **AArch64**: `sxtb/sxth/sxtw` + `uxtb/uxth/uxtw`; note SXTB/SXTH/UXTB/UXTH require **W** registers in the asm emission.
- **RISC-V**: `slli`+`srai` (sext), `andi 0xff/0xffff` or `slli/srli 32` (zext); the `slli` operand must carry `rs1 = src` (a missing field silently extends zero).

The `-S` asm emitters mirror the binary encodings but must stay **GAS-legal**: `movzbl`/`movzwl` have no accepted 64-bit-destination syntax, so the x86 asm path emits 32-bit-destination forms (which zero the upper half) plus a follow-up `movl` where a 64-bit dest is required.

## x86 ModRM Source/Destination Placement (`setcc`, cross-register extend)

The Sprint 35 Phase 0 regression sweep (`make test-opt`) exposed two ModRM operand-placement bugs that only misfire when the relevant register index is >= 8 (REX-extended) or differs from the hardcoded default:

- **`setcc` destination is in the r/m field, not the reg field**: `0F 90-9F` (`SETcc r/m8`) encodes the destination register in **r/m**, with the reg field = 000. `x86_emit_setcc` must emit `x86_modrm(c, 3, 0, dst & 7)` with the REX **B** bit (not R) when `dst >= 8`. The `-S` path (`x86_emit_cond_set_asm`) had the same class of bug a different way: it hardcoded `setcc %al` + `movzbq %al,<dst>`, which **clobbers live RAX** whenever the condition result belonged to another temp (e.g. a loop bound loaded into `r19` while `r0`/rax held a live value). It must use the destination's low byte (`%al`/`%cl`/`%r12b`...) as the `setcc` target, then zero-extend into the destination.
- **Cross-register extend must put `src` in r/m**: `MOVSXD r64, r/m32` and `MOV r64, r/m32` take the **source** in the r/m field and the destination in the reg field. `x86_emit_extend` (MC path) and `x86_emit_extend_asm` (text path) previously ignored `src` and/or swapped the fields, so for `dst != src` (or `dst >= 8`) they extended the wrong register (e.g. `movslq %esp, %r12` instead of `movslq %r12d, %r12`), silently corrupting the value. In-place extends (`src == dst`) hid the bug; the loop-promotion mem2reg pass (Sprint 33) started producing cross-register extends, which is when it detonated.

Rule of thumb when adding an x86 emitter: write the instruction, verify the ModRM reg/rm assignment against the Intel manual for that opcode, and test with a register >= 8 as an operand - the in-register-index-0-7 path rarely catches field swaps.

## Multi-ISA SIMD / Vector Emission (SSE, NEON, RVV 1.0)

When loop vectorization emits packed vector opcodes (`OP_VLOAD`, `OP_VSTORE`, `OP_VADDPS`, `OP_VSUBPS`, `OP_VMULPS`, `OP_VDIVPS`):
- **x86-64 (SSE/AVX)**:
  - Vector registers `xmm0`–`xmm15`.
  - Memory moves: `movups (%rsi), %xmm0` / `movups %xmm0, (%rdi)`.
  - Vector arithmetic: `addps %xmm1, %xmm0`, `subps`, `mulps`, `divps`.
- **AArch64 (NEON)**:
  - 128-bit SIMD registers `v0`–`v31` (referenced as `q0`–`q31` for 128-bit vector loads/stores and `s0`–`s31` for scalar singles).
  - Memory moves: `ldr q0, [x1]` / `str q0, [x0]`.
  - Vector arithmetic: `fadd v0.4s, v0.4s, v1.4s`, `fsub`, `fmul`, `fdiv`.
- **RISC-V (RVV 1.0 `rv64gcv`)**:
  - Vector registers `v0`–`v31` with explicit vector length/type configuration.
  - Configuration: `vsetivli zero, 4, e32, m1, ta, ma` (sets vector length to 4 elements of 32-bit floats, LMUL=1).
  - Memory moves: `vle32.v v0, 0(a1)` / `vse32.v v0, 0(a0)`.
  - Scalar float operations continue using standard RISC-V FP registers (`fa0`–`fa7`, `ft0`–`ft11`) via `flw`/`fsw`/`fadd.s`.

### Target SIMD Implementation & Register Allocator Integration (Sprint 179)

- **AArch64 128-bit NEON Raw MC Emission**: `aarch64_emit_vload` and `aarch64_emit_vstore` in `src/zero_cc/isa/aarch64.c` emit authentic 128-bit `ldr q` (`0x3DC00000` / `0x3CC00000`) and `str q` (`0x3D800000` / `0x3C800000`). Handles scaled 12-bit unsigned offsets (multiples of 16), unscaled 9-bit signed offsets, and scratch register `x16` computation for unaligned or large offsets.
- **RISC-V RVV 1.0 Raw MC & Text Assembly Emission**:
  - `src/zero_cc/isa/riscv64.c` implements authentic RVV 1.0 vector machine code emission with explicit vector configuration: `vsetivli zero, 4, e32, m1, ta, ma` (`0xCD027057`) for 4x32-bit single-precision floats and 32-bit integers, and `vsetivli zero, 2, e64, m1, ta, ma` (`0xCD817057`) for 2x64-bit double-precision floats.
  - Implements `emit_vload`, `emit_vstore`, `emit_vaddps` (`vfadd.vv`), `emit_vsubps` (`vfsub.vv`), `emit_vmulps` (`vfmul.vv`), `emit_vdivps` (`vfdiv.vv`), `emit_vaddpd`, `emit_vsubpd`, `emit_vmulpd`, `emit_vdivpd`, `emit_vpaddd` (`vadd.vv`), `emit_vpsubd` (`vsub.vv`), `emit_vpmulld` (`vmul.vv`), `emit_vandps` (`vand.vv`), `emit_vorps` (`vor.vv`), `emit_vxorps` (`vxor.vv`), and `emit_vmovaps` (`vmv.v.v`).
  - Text assembly emission in `src/zero_cc/isa/riscv_asm.c` accumulates non-zero offsets into scratch register `t5` before emitting `vle32.v` / `vse32.v`, conforming to GNU assembler syntax which rejects immediate displacements on vector memory instructions.
- **Multi-ISA Raw MC Vector Bitwise Emission (Sprint 197 Track 1)**:
  - Added native raw machine code emitters for `OP_VANDPS`, `OP_VORPS`, `OP_VXORPS` to `ZeroCCISABackend` (`emit_vandps`, `emit_vorps`, `emit_vxorps`):
    - x86-64: `0x0F 0x54` (`andps`), `0x0F 0x56` (`orps`), `0x0F 0x57` (`xorps`) with ModR/M and operand swap/move when `dst != src1`.
    - AArch64: `and v.16b` (`0x4E201C00`), `orr v.16b` (`0x4EA01C00`), `eor v.16b` (`0x4EA01C00`).
    - RISC-V 64: `vand.vv` (`0x26000057`), `vor.vv` (`0x2A000057`), `vxor.vv` (`0x2E000057`) with `e32, m1` vector configuration.
  - Machine code lowering in `ast_lowering_mc.c` wires raw backend calls for `OP_VANDPS`, `OP_VORPS`, `OP_VXORPS`.
- **Decoupled Vector Register Allocation (`REG_CLASS_VEC`)**:
  - `REG_CLASS_VEC` is decoupled from scalar FP (`REG_CLASS_XMM`): vector registers are caller-saved across function calls (`spans_call -> spill`), vector scratch registers (14 and 15) are reserved from allocation, and 16-byte aligned 2-slot spill slots are allocated on the stack.

### AVX / AVX2 256-Bit SIMD Emission (Sprint 199)

- **CLI & Feature Flags**: `-mavx`, `-mavx2`, `-mno-avx`, `-mno-avx2`, with `#define __AVX__ 1` and `#define __AVX2__ 1` defined during preprocessing when active.
- **Target Traits & Vector Width**: `zc_target_default_vec_width` scales to 8 for single-precision floats and 4 for double-precision floats on x86-64 when `has_avx` is enabled; `zc_target_max_vec_bytes` reports 32 bytes.
- **2-Byte and 3-Byte VEX Prefix Encoding**:
  - `x86_vex` in `src/zero_cc/isa/x86_64_common.h` dynamically formats 2-byte VEX prefixes (`0xC5`) when `map_select == 1` and `X_bit == 1 && B_bit == 1`, falling back to 3-byte VEX prefixes (`0xC4`) when extended registers (`r8`–`r15`, `xmm8`–`xmm15`) or non-0F opcode maps are involved.
  - Three-operand non-destructive vector instructions: `v_reg` is encoded inverted (`~v & 0x0F`) in bits [6:3] of byte 1 (2-byte VEX) or byte 2 (3-byte VEX). `L=1` designates 256-bit vector operations (`%ymm`).
- **256-Bit Raw MC and Text Assembly Backend**:
  - Implemented 14 256-bit operations in `ZeroCCISABackend`: `emit_v256_load`, `emit_v256_store`, `emit_v256_addps`, `emit_v256_subps`, `emit_v256_mulps`, `emit_v256_divps`, `emit_v256_addpd`, `emit_v256_subpd`, `emit_v256_mulpd`, `emit_v256_divpd`, `emit_v256_andps`, `emit_v256_vorps`, `emit_v256_vxorps`, and `emit_v256_zeroupper`.
  - Machine code emitters in `src/zero_cc/isa/x86_64.c` write VEX-prefixed machine code directly into the JIT/ELF text section.
  - Text assembly emitters in `src/zero_cc/isa/x86_64_asm.c` emit AT&T syntax instructions with `%ymm0`–`%ymm15` (`vmovups`, `vaddps`, `vaddpd`, etc.) and `vzeroupper`.
- **Intrinsics Virtual Headers & Builtin Lowering**:
  - Virtual headers `<immintrin.h>`, `<avxintrin.h>`, `<avx2intrin.h>` define 256-bit vector types (`__m256`, `__m256d`, `__m256i`, `__v8sf`, `__v4df`, `__v8si`, `__v4di`) and standard intrinsics (`_mm256_loadu_ps`, `_mm256_storeu_ps`, `_mm256_add_ps`, `_mm256_sub_ps`, `_mm256_mul_ps`, `_mm256_div_ps`, `_mm256_add_pd`, `_mm256_sub_pd`, `_mm256_mul_pd`, `_mm256_div_pd`, `_mm256_set1_ps`, `_mm256_setzero_ps`, `_mm256_zeroupper`, etc.).
  - Builtins lower to hardware VEX opcodes on x86-64 when `-mavx` is enabled, and gracefully fall back to 8-lane / 4-lane scalar emulation when AVX is unavailable or targeting other ISAs.
- **Auto-Vectorizer Scaling**:
  - `opt_auto_vectorize` sets `vector_width = 8` for floats and `vector_width = 4` for doubles when `has_avx` is enabled on x86-64.
  - Emits `OP_V256_LOAD`, `OP_V256_STORE`, and `OP_V256_*` arithmetic opcodes, automatically peeling remainder loops when trip counts are not exact multiples of 8 or 4.
  - Machine code lowering (`ast_lowering_mc.c`) and text assembly lowering (`asm_emitter.c`) route vector spill reloads and stores through `emit_vload` / `emit_vstore`.
  - Strict separation between packed vector opcodes (`ir_opcode_is_simd`) and scalar floating-point opcodes (`ir_opcode_is_float`, e.g. `OP_VADDSS`, `OP_VSTORESD`) guarantees scalar float temps never alias or spill as vector registers.
- **Auto-Vectorizer Induction Classification (`opt_vectorize.c`)**:
  - Induction address classification supports `OP_SEXT8/16/32` and `OP_ZEXT8/16/32`, enabling vectorization of standard loops indexing arrays with 32-bit `int` variables.
  - Temp-to-vector mapping checks use the complete 16-bit temp space (65536).
### AVX-512 512-Bit SIMD Emission (Sprint 200)

- **CLI & Feature Flags**: `-mavx512f`, `-mno-avx512f` (with `-mavx512f` implying AVX and AVX2), with `#define __AVX512F__ 1` defined during preprocessing when active.
- **Target Traits & Vector Width**: `zc_target_default_vec_width` scales to 16 for single-precision floats and 8 for double-precision floats on x86-64 when `has_avx512f` is enabled; `zc_target_max_vec_bytes` reports 64 bytes.
- **4-Byte EVEX Prefix Encoding**:
  - `x86_evex` in `src/zero_cc/isa/x86_64_common.h` formats standard 4-byte EVEX prefixes (`0x62`) with inverted field encoding: `R`, `X`, `B`, `R'`, `mm[1:0]` in Byte 1; `W`, `vvvv[3:0]`, fixed bit 2, `pp[1:0]` in Byte 2; and `z`, `L'L = 10` (512-bit vector length), `b = 0`, `V' = 1`, `aaa = 000` (k0 mask) in Byte 3.
  - Supports 32 ZMM registers (`%zmm0`–`%zmm31`), non-destructive 3-operand vector instructions, and compressed displacement scaling (`disp8 * 64`) for memory operations.
- **512-Bit Raw MC and Text Assembly Backend**:
  - Implemented 13 512-bit operations in `ZeroCCISABackend`: `emit_v512_load`, `emit_v512_store`, `emit_v512_addps`, `emit_v512_subps`, `emit_v512_mulps`, `emit_v512_divps`, `emit_v512_addpd`, `emit_v512_subpd`, `emit_v512_mulpd`, `emit_v512_divpd`, `emit_v512_andps`, `emit_v512_vorps`, `emit_v512_vxorps`.
  - Machine code emitters in `src/zero_cc/isa/x86_64.c` write EVEX-prefixed machine code directly into the JIT/ELF text section.
  - Text assembly emitters in `src/zero_cc/isa/x86_64_asm.c` emit AT&T syntax instructions with `%zmm0`–`%zmm31` (`vmovups`, `vaddps`, `vsubps`, `vmulps`, `vdivps`, `vaddpd`, `vsubpd`, `vmulpd`, `vdivpd`, etc.).
- **Intrinsics Virtual Headers & Builtin Lowering**:
  - Virtual header `<immintrin.h>` defines 512-bit vector types (`__m512`, `__m512d`, `__m512i`, `__v16sf`, `__v8df`, `__v16si`, `__v8di`) and standard intrinsics (`_mm512_loadu_ps`, `_mm512_storeu_ps`, `_mm512_add_ps`, `_mm512_sub_ps`, `_mm512_mul_ps`, `_mm512_div_ps`, `_mm512_add_pd`, `_mm512_sub_pd`, `_mm512_mul_pd`, `_mm512_div_pd`, `_mm512_and_ps`, `_mm512_or_ps`, `_mm512_xor_ps`, `_mm512_set1_ps`, `_mm512_setzero_ps`, etc.).
  - Builtins lower to hardware EVEX opcodes on x86-64 when `-mavx512f` is enabled, and gracefully fall back to 16-lane / 8-lane scalar emulation when AVX-512 is unavailable or targeting other ISAs.
- **Auto-Vectorizer Scaling**:
  - `opt_auto_vectorize` sets `vector_width = 16` for floats and `vector_width = 8` for doubles when `has_avx512f` is enabled on x86-64.
  - Emits `OP_V512_LOAD`, `OP_V512_STORE`, and `OP_V512_*` arithmetic opcodes, automatically peeling remainder loops when trip counts are not exact multiples of 16 or 8.

### AVX-512 Opmask & Predication (Sprint 201)

- **Opmask Register & Type Abstraction**:
  - Mask types `__mmask8`, `__mmask16`, `__mmask32`, `__mmask64` defined in `<immintrin.h>`.
  - Bitwise mask intrinsics (`_kand_mask8/16/32/64`, `_kor_mask*`, `_kxor_mask*`, `_knot_mask*`) and scalar conversion intrinsics (`_cvtu32_mask8/16`, `_cvtmask8/16_u32`, `_cvtu64_mask32/64`, `_cvtmask32/64_u64`).
  - GPR-to-opmask transfer instructions: `kmovw` (16-bit) and `kmovb` (8-bit) between scalar GPRs and hardware opmask register `k1` (`OP_KMOVW_TO_K1` and `OP_KMOVW_FROM_K1`). GNU `as` AT&T syntax requires 32-bit register operands (e.g. `kmovw %edi, %k1`).
- **EVEX Write-Masking & Zeroing ({k1}, {k1}{z})**:
  - Machine code emitters configure EVEX byte 3 mask bits `aaa = 001` (k1) and zeroing bit `z` (0 for merging write-masking, 1 for zeroing-masking `{z}`).
  - Assembly emitters append `{k1}` or `{k1}{z}` EVEX operands (e.g., `vmovups (%rsi), %zmm0{%k1}`, `vaddps %zmm2, %zmm1, %zmm0{%k1}{z}`).
- **Masked Vector Load, Store, Arithmetic & Comparison**:
  - Masked memory operations: `_mm512_mask_loadu_ps`, `_mm512_maskz_loadu_ps`, `_mm512_mask_storeu_ps`, `_mm512_mask_loadu_pd`, `_mm512_maskz_loadu_pd`, `_mm512_mask_storeu_pd`.
  - Masked arithmetic: `_mm512_mask_add_ps` / `_mm512_maskz_add_ps`, `sub_ps`, `mul_ps`, `div_ps`, `add_pd`, `sub_pd`, `mul_pd`, `div_pd`, `mov_ps`, `mov_pd`.
  - Mask-producing comparisons: `_mm512_cmpeq_ps_mask`, `_mm512_cmplt_ps_mask`, `_mm512_cmpeq_pd_mask`, `_mm512_cmplt_pd_mask`, plus their masked counterparts `_mm512_mask_cmpeq_ps_mask`, etc.
  - IR lowering emits hardware EVEX instructions on x86-64 when `-mavx512f` is active; falls back to bitwise lane-predicated scalar emulation in JIT / non-AVX-512 environments.

### AVX-512 Integer SIMD & AVX-512BW / AVX-512DQ (Sprint 202)

- **CLI & Feature Flags**: `-mavx512bw`, `-mno-avx512bw`, `-mavx512dq`, `-mno-avx512dq` (with `-mavx512bw` and `-mavx512dq` implying AVX, AVX2, and AVX-512F); `#define __AVX512BW__ 1` and `#define __AVX512DQ__ 1` defined during preprocessing when active.
- **512-Bit Integer IR Opcodes & Emitters**:
  - Implemented 15 512-bit integer arithmetic and bitwise opcodes in `ZeroCCISABackend`:
    - 32-bit & 64-bit element operations (AVX-512F / AVX-512DQ): `OP_V512_PADDD` (`vpaddd`), `OP_V512_PSUBD` (`vpsubd`), `OP_V512_PMULLD` (`vpmulld`), `OP_V512_PADDQ` (`vpaddq`), `OP_V512_PSUBQ` (`vpsubq`), `OP_V512_PMULLQ` (`vpmullq`).
    - Bitwise logic (AVX-512F): `OP_V512_PANDD` (`vpandd`), `OP_V512_PORD` (`vpord`), `OP_V512_PXORD` (`vpxord`), `OP_V512_PANDND` (`vpandnd`).
    - Byte & Word element operations (AVX-512BW): `OP_V512_PADDB` (`vpaddb`), `OP_V512_PSUBB` (`vpsubb`), `OP_V512_PADDW` (`vpaddw`), `OP_V512_PSUBW` (`vpsubw`), `OP_V512_PMULLW` (`vpmullw`).
  - Condition code multiplexing: `cond = 0` (unmasked EVEX), `cond = 1` (masked `{k1}` write-masking), `cond = 2` (masked zeroing `{k1}{z}`).
  - Integer vector comparisons (`OP_V512_CMP_MASK`): unified emitter supporting `cmp_type` 2 (`vpcmpd`), 3 (`vpcmpud`), 4 (`vpcmpq`), 5 (`vpcmpuq`), emitting condition codes directly into opmask registers `k1` and transferring to scalar GPRs via `kmovw`/`kmovb`.
- **Intrinsics Virtual Headers & Builtin Lowering**:
  - Added vector types to `<immintrin.h>`: `__v32hi`, `__v32hu`, `__v64qi`, `__v64qu`.
  - Added comprehensive AVX-512 integer intrinsics:
    - Arithmetic: `_mm512_add_epi32`, `_mm512_sub_epi32`, `_mm512_mullo_epi32`, `_mm512_add_epi64`, `_mm512_sub_epi64`, `_mm512_mullo_epi64`, `_mm512_add_epi8`, `_mm512_sub_epi8`, `_mm512_add_epi16`, `_mm512_sub_epi16`, `_mm512_mullo_epi16`.
    - Bitwise logic: `_mm512_and_si512`, `_mm512_or_si512`, `_mm512_xor_si512`, `_mm512_andnot_si512`.
    - Broadcast & set: `_mm512_set1_epi64`, `_mm512_set1_epi64x`, `_mm512_set1_epi32`, `_mm512_set1_epi16`, `_mm512_set1_epi8`, `_mm512_set_epi32`, `_mm512_set_epi64`, `_mm512_setr_epi32`, `_mm512_setr_epi64`.
    - Masked variants: `_mm512_mask_*`, `_mm512_maskz_*` for all integer arithmetic and bitwise intrinsics.
    - Comparisons: `_mm512_cmpeq_epi32_mask`, `_mm512_cmplt_epi32_mask`, `_mm512_cmpgt_epi32_mask`, etc.
    - Masked moves: `_mm512_mask_mov_epi32`, `_mm512_maskz_mov_epi32`, `_mm512_mask_mov_epi64`, `_mm512_maskz_mov_epi64`.
  - Fallback emulation: bit-level scalar loops handling full 512-bit integer arithmetic, masking, and comparisons on hosts lacking AVX-512 hardware.
- **Auto-Vectorizer Scaling**:
  - `opt_auto_vectorize` maps 32-bit integer loop operations to `OP_V512_PADDD`, `OP_V512_PSUBD`, `OP_V512_PMULLD`, `OP_V512_PANDD`, `OP_V512_PORD`, `OP_V512_PXORD` when `has_avx512f` is active.

### AVX-512 Dedicated Mask Register File & Mask Instructions (Sprint 205 - Phase 11)

- **Dedicated Mask Registers (`k0..k7`) & Instructions**:
  - Implemented raw VEX/EVEX and AT&T assembly emitters for dedicated mask register instructions: `kmovw` (register-to-mask, mask-to-register, mask-to-mask), `kandw`, `korw`, `kxorw`, `knotw`, and `kortestw`.
  - Added 10 IR opcodes: `OP_KMOVW_TO_K`, `OP_KMOVW_FROM_K`, `OP_KMOVW_KK`, `OP_KANDW`, `OP_KORW`, `OP_KXORW`, `OP_KNOTW`, `OP_KORTESTW`, `OP_V512_BLENDPS`, `OP_V512_BLENDPD`.
  - `kortestw` tests `(ks1 | ks2) == 0` (setting ZF, extracted via `sete`) and `(ks1 | ks2) == 0xFFFF` (setting CF, extracted via `setc`).
- **Masked Vector Blend (`vblendmps` / `vblendmpd`)**:
  - Encoded with EVEX opmask write-masking (`vblendmps %zmm2, %zmm1, %zmm0{%k1}`), selecting lanes from src2 when the mask bit is 1, and src1 when the mask bit is 0.
- **Intrinsics Virtual Headers & Builtin Lowering**:
  - Added mask manipulation intrinsics (`_kand_mask*`, `_kor_mask*`, `_kxor_mask*`, `_knot_mask*`, `_kortestz_mask*`, `_kortestc_mask*`, `_load_mask*`, `_store_mask*`) and blend intrinsics (`_mm512_mask_blend_ps`, `_mm512_mask_blend_pd`, `_mm512_mask_blend_epi32`, `_mm512_mask_blend_epi64`) to `<immintrin.h>`.
  - Built fallback scalar simulation loops ensuring correct execution in JIT mode and on CPUs lacking AVX-512 hardware.

### SIMD Vendor Intrinsics & Virtual Headers (Sprint 196 Track 1)

ZeroCC embeds virtual standard vendor headers in `src/zero_cc/core/virtual_headers.h` providing production-grade intrinsic compatibility:
- `<immintrin.h>` (x86-64 SSE, SSE2, SSE3, SSSE3, SSE4.1, FMA):
  - Types: `__m128` (float x 4), `__m128d` (double x 2), `__m128i` (integer vector).
  - Packed single/double arithmetic: `_mm_add_ps`, `_mm_sub_ps`, `_mm_mul_ps`, `_mm_div_ps`, `_mm_add_pd`, `_mm_sub_pd`, `_mm_mul_pd`, `_mm_div_pd`.
  - Packed integer arithmetic: `_mm_add_epi32`, `_mm_sub_epi32`, `_mm_mullo_epi32`, `_mm_add_epi64`, `_mm_sub_epi64`.
  - Memory & initialization: `_mm_load_ps`, `_mm_loadu_ps`, `_mm_store_ps`, `_mm_storeu_ps`, `_mm_set1_ps`, `_mm_set_ps`, `_mm_setzero_ps`, `_mm_set1_epi32`.
  - Extended SSSE3/SSE4.1/FMA: `_mm_hadd_ps`, `_mm_hsub_ps`, `_mm_abs_epi32`, `_mm_fmadd_ps`, `_mm_min_ps`, `_mm_max_ps`.
- `<arm_neon.h>` (AArch64 NEON & cross-platform emulation):
  - Types: `float32x4_t`, `float64x2_t`, `int32x4_t`, `uint32x4_t`, `int16x8_t`, `int8x16_t`.
  - Vector operations: `vld1q_f32`, `vst1q_f32`, `vaddq_f32`, `vsubq_f32`, `vmulq_f32`, `vdupq_n_f32`, `vld1q_s32`, `vst1q_s32`, `vaddq_s32`, `vsubq_s32`, `vmulq_s32`, `vdupq_n_s32`, `vfmaq_f32`, `vmlaq_s32`, `vmlsq_s32`, `vmaxq_f32`, `vminq_f32`, `vabsq_f32`.
- **Lowering via `__builtin_zerocc_v*` Builtins (`ast_lowering_builtins.c`)**:
  - Compiler builtins map directly to hardware vector IR instructions on supported 64-bit targets (x86-64 SSE, AArch64 NEON, RISC-V RVV), with automatic scalar fallback on 32-bit platforms.
  - Return ABI invariant: in `ast_lowering_stmt.c`, function calls returning vector values copy the 16-byte result chunks from stack memory buffers into return registers `(VREG_0, VREG_2)`.

## Scalar Float / Integer Conversions (`OP_CVTSI2SD` / `OP_CVTTSD2SI`)

Conversion between integers and scalar floating-point values is supported across JIT and AOT assembly emission via `emit_fp_cvtsf` / `emit_fp_cvtsf_asm` (int to double) and `emit_fp_cvtsi` / `emit_fp_cvtsi_asm` (double to int):
- **x86-64**: `cvtsi2sd %src, %xmm_dst` and `cvttsd2si %xmm_src, %dst`.
- **AArch64**: `scvtf d<dst>, <src>` and `fcvtzs <dst>, d<src>`.
- **RISC-V 64**: `fcvt.d.w <dst>, <src>` and `fcvt.w.d <dst>, <src>, rtz`.

## AOT Assembly Data Emission (`-S .data` and `.rodata`)

When `-S` is invoked, `emit_asm_data_sections()` emits all global symbols, alignments, initialized data bytes, and relocations:
- `.rodata` for string literals and floating-point constants.
- `.data` section emitting `.globl` directives, `.p2align` / `.align`, labels, initialized bytes, and `.quad` relocations pointing to `.rodata` string literals or other globals (`target_kind = 0/1`).

## `-S` In-Place ALU Opcode Coverage (Sprint 40)

`ir/asm_emitter.c` (the `-S` switch) previously fell through to a `# UNKNOWN` comment for `OP_ADD_IMM`, `OP_SUB_IMM`, `OP_INC`, `OP_DEC`, `OP_NOT`, and `OP_LNOT` — any `-S` output containing them (e.g. `return ~a;` at `-O0`) was silently wrong while the `-o` ELF path (MC lowering) handled them fine. Sprint 40 added emitters on all three backends (new vtable slots `emit_add_imm_asm` / `emit_sub_imm_asm` / `emit_inc_asm` / `emit_dec_asm` / `emit_not_asm` in `isa_backend.h`):
- **x86-64**: `addq $imm, %d` / `subq $imm, %d` / `incq %d` / `decq %d` / `notq %d`; `OP_LNOT` composes the existing emitters (`testq %s,%s` + `sete` + `movzbq`).
- **AArch64**: `add d, #imm` / `sub d, #imm` / `add d, #1` / `sub d, #1` / `mvn d, s`.
- **RISC-V**: `addi d, d, imm` / `addi d, d, -imm` / `addi d, d, ±1` / `xori d, d, -1`.
- **In-place NOT semantics**: `x86` and `riscv` emit `mv dst, src` (or the `xori`/`mvn` two-operand form on AArch64) when the allocator picked `dst != src`, mirroring the MC emitters (`x86_emit_not` in `x86_64.c`) — the first attempt that omitted the copy read a garbage register and only surfaced in `-S` execution.
- Note: the Sprint 39 documentation listed `OP_SUB_INC`/`OP_ADD_INC` in this gap — those opcodes do not exist in the 127-opcode enum; the real list is the six above. `ir_emit_inc/dec/add_imm/sub_imm` had no lowering callers pre-Sprint 40 (in-place `i++` lowers to plain `ADD`/`SUB`), so the gap was latent until the emitters landed.
- Verified: 0 `# UNKNOWN` comments across the 79-test `test/jit` `-S` corpus; all assemblable standalone outputs compile with `gcc -c` (16/16).

## RISC-V Raw Binary Emitter & Relocation Fixes (Sprint 41)

Differential testing under QEMU exposed several immediate-encoding and relocation issues in the RISC-V raw machine-code path (`riscv.c`, `main.c`, `ast_lowering_mc.c`, `ast_lowering_expr.c`), which were resolved to bring `riscv64` raw execution to **15/15 passing**:

- **B-Type Immediate Encoding (`rv64_B` & `riscv64_patch_jmp`)**:
  - RISC-V B-type format encodes `imm[12|10:5] rs2 rs1 funct3 imm[4:1|11] opcode`.
  - Immediate bit 11 maps to instruction bit 7: requires `((u & 0x800u) >> 4)` (previously shifted `<< 4` into bit 15 clobbering `rs1`).
  - `imm[4:1]` (bits 4:1) moves to instruction bits 11:8: requires `((u & 0x1Eu) << 7)`.
- **J-Type Immediate Encoding (`rv64_J` & `riscv64_patch_jmp`)**:
  - RISC-V J-type format encodes `imm[20|10:1|11|19:12] rd opcode`.
  - `imm[10:1]` mask is `0x7FEu` (10 bits, 0b0111_1111_1110); previous `0x3FE` mask dropped bit 10.
  - `imm[19:12]` mask is `0xFF000u` (bits 19:12); previous `0xFF00u` masked bits 15:8.
- **S-Type Immediate Encoding (`rv64_S`)**:
  - `imm[11:5]` is 7 bits (bits 11 to 5) with mask `0xFE0u` (previous `0xFF80u` mask dropped bits 5 and 6).
- **ELF psABI Relocations (`main.c`)**:
  - **Function Call Relocations**: `reloc_type_call` emits `R_RISCV_JAL` (17) instead of `R_RISCV_CALL` (18). `R_RISCV_CALL` expects an 8-byte `auipc + jalr` pair, whereas zerocc emits a 4-byte `jal ra`. Internal calls to defined functions are patched directly at codegen time.
  - **PC-Relative Symbol Addressing (`%pcrel_hi` / `%pcrel_lo`)**: For global symbols and string literal relocations (`OP_MOV_GLOBAL`), lowered to an 8-byte `auipc rd, 0` + `addi rd, rd, 0` sequence. A local label symbol (`.L_pcrel_hi_<id>`) is created at the `auipc` offset in `.text`. The relocation pair emitted consists of `R_RISCV_PCREL_HI20` (23) at `patch_offset - 4` targeting the symbol with data addend, and `R_RISCV_PCREL_LO12_I` (24) at `patch_offset` targeting the local label symbol with **addend = 0**. This conforms strictly to the RISC-V ELF psABI and avoids GNU ld's fatal rejection (`dangerous relocation: %pcrel_lo section symbol with an addend`).
  - **Data Pointer Relocations**: `reloc_type_data_abs` mapped to `R_RISCV_64` (2) on RV64, `R_RISCV_32` (1) on RV32/x86-64, and `R_AARCH64_ABS64` (257) on AArch64.
- **Arithmetic & Comparison Codegen (`riscv.c`)**:
  - `div` / `idiv`: `funct3=4`, `funct7=1` (was erroneous `funct3=0, funct7=4` producing illegal instructions).
  - `mod`: `funct3=6`, `funct7=1` (was erroneous `funct3=0, funct7=8`).
  - `deref`: 64-bit `ld` (`funct3=3`) on RV64, 32-bit `lw` (`funct3=2`) on RV32.
  - `cond_set CC_EQ`: `sltiu dst, dst, 1` (`seqz`).
  - `cond_set CC_NE`: `sltu dst, zero, dst` (`snez`).
  - `lnot`: `sltiu dst, src, 1` (`seqz`).
  - `jle` / `jcond CC_LE`: `bge src2, src1` (swapping source registers because RISC-V lacks a dedicated `ble`).
- **Floating-Point & Variadic ABI Lowering (`riscv.c`, `ast_lowering_expr.c`)**:
  - Encodings for double-precision: `fadd.d` (0x01), `fsub.d` (0x05), `fmul.d` (0x09), `fdiv.d` (0x0D), `fneg.d` (`fsgnjn.d`, 0x11, rm=1), `fmv.d` (`fsgnj.d`, 0x11, rm=0), `fcvt.l.d` (0x61, rs2=2, rm=1 RTZ), `fcvt.d.w` (0x69, rs2=0, rm=7), `flt.d` (0x51, rm=1), `fle.d` (0x51, rm=0), `feq.d` (0x51, rm=2).
  - `load_fp_rodata`: uses integer temporary register `t0` (`x5`) for `auipc` address generation instead of FP register `dst_reg` (which mapped to `zero`/`x0`, causing null pointer faults).
  - Wired backend vtable slots: `.emit_vloadsd = riscv64_emit_load_fp`, `.emit_vstoredsd = riscv64_emit_store_fp`, `.emit_vloadss`, `.emit_vstoress`.
  - **Variadic Calling Convention**: Under standard RISC-V calling conventions, floating-point arguments to variadic functions (`printf`, etc.) must be passed in integer GPRs (`a0..a7`) rather than FP registers (`fa0..fa7`). Lowering in `ast_lowering_expr.c` spills FP values and reloads them into integer argument registers.
- **Comparison & Flag-Lowering State (`ast_lowering_mc.c`, `lowering_state.h`, `live_range.h`)**:
  - Added `cmp_is_fp` flag in lowering state to track floating-point vs integer comparisons.
  - Extended live ranges across RISC-V comparisons so source registers are preserved until condition codes or predicates are materialized (`slt`, `seqz`, conditional branches).
  - In `OP_CMP_ZERO`, explicitly record `cmp_src1 = phys_src1` and `cmp_src2 = 0` (zero register).
- **Execution & Calling Convention Fixes**:
  - `riscv64_emit_extend`: Corrected `slli` `funct3` to 1 (was erroneously 0 `addi`), resolving an infinite loop in 64-bit loop-counter sign extensions.
  - `riscv64_emit_param_save_int`: Fixed incoming parameter save to store `a0..a7` (`x10..x17`) into `-offset(s0)` (was storing `x0` to a positive offset from `sp`), unblocking recursive function arguments in `fact(4)`.
  - `riscv64_emit_load`: Fixed 64-bit load to use `ld` (`funct3=3`) rather than `lw` (`funct3=2`) on RV64, correctly loading 64-bit structs returned by value (`11_retstruct.c`).
  - `elf_emitter.h`: Skipped generic 4-byte call relocation patching on RISC-V to prevent corrupting `jal` opcodes with raw offsets.

## AArch32 (ARMv7-A) Backend & AAPCS32 Integration

ZeroCC includes an ARMv7-A backend (`src/zero_cc/isa/aarch32.c`) targeting 32-bit ARM Linux (EABI / AAPCS32):

- **Register Allocation & Calling Convention**:
  - **Callee-Saved Registers**: `r4`–`r11` (`r11` serves as the frame pointer), `lr` (return address).
  - **Argument & Return Registers**: `r0`–`r3` pass the first 4 32-bit arguments; scalar returns are passed in `r0` (or `(r0, r1)` pair for 64-bit integers and doubles).
  - **Stack Arguments**: Pushed onto stack in 4-byte slots aligned to 4-byte boundaries.
- **64-bit Scalar Lowering**:
  - Low-half and high-half decomposition across arithmetic, comparison, and memory operations (`ir_emit_load64`, struct member loads/stores).
  - 64-bit integer multiply (`aarch32_emit_mul64`, `aarch32_emit_mul64_asm`): evaluates cross-terms `a_lo * b_hi + a_hi * b_lo` using `lr` (r14) with `MLA` (`mla lr, a_hi, b_lo, lr`) followed by `umull dlo, dhi, a_lo, b_lo` and `add dhi, dhi, lr`. Dedicated `lr` usage ensures zero register collision when `(dlo, dhi)` aliases with spilled scratch pairs.
  - Variable-count 64-bit shift helpers (`zc_shl64`, `zc_shr64`, `zc_sar64`) in `zero_rt/core/int128.c` using AAPCS32 register pair `(r0, r1)` and count in `r2`.
- **Target-Aware Pointer Sizing**:
  - `type_set_target_metrics(int ptr_sz, int long_sz)` dynamically configures pointer and long sizing across semantic analysis (`TypeInfo`), `ZeroCCType` (`zero_cc_type_ptr`), and parser struct member layouts.
  - On `ISA_AARCH32` and `ISA_RISCV32`, pointers are 4 bytes and 4-byte aligned (`sizeof(void*) == 4`).
- **Variadic Arguments (`va_list`) & stdio Formatting**:
  - Standardized 20-byte `va_list` layout matching the 5-element 32-bit pointer stream (`gp`, `gp_end`, `fp`, `fp_end`, `ovf`).
  - Pointer arguments in `va_arg` correctly evaluate as 4-byte scalar quantities instead of 8-byte quantities.
  - AAPCS32 even-register-pair alignment: in `ast_lowering_varargs.c`, 8-byte variadic arguments (`double`, `long long`) check current register offset `(gp_val - gp_start) & 4` and advance to the next even register pair `(r0:r1, r2:r3)` before loading.
  - `zero_rt/core/stdio.c` (`vsnprintf_core`): length-modifier-aware unpacking (`is_64` for `ll` and `j`), popping 64 bits (`long long`) for `%lld/%llu/%llx` on 32-bit targets where `sizeof(long) == 4`.
- **ELF32 Relocatable Object & Linker Support**:
  - Direct ELF32 object generation (`EM_ARM`, machine code 40) via `elf_writer` with `e_flags = ZC_EF_ARM_EABI_VER5` (`0x05000000u`).
  - In-instruction addend encoding: `aarch32_emit_lea_rip` and `aarch32_emit_load_fp_rodata` encode `rodata_offset` into the lower/upper 16-bit immediate fields of `movw` and `movt` instructions, ensuring standard ELF `REL` format compliance when linked by GNU `ld` or own-linker.
  - Relocations: `R_ARM_CALL` / `R_ARM_JUMP24`, `R_ARM_MOVW_ABS_NC`, `R_ARM_MOVT_ABS`, `R_ARM_ABS32`.
  - Standalone, `-S` text assembly, and raw-binary execution verified across all 16 tests in `test/isa_qemu/` under `qemu-arm-static`.

### Raw Emitter Verification Suite (`tools/`)

To systematically eliminate raw-emitter and cross-target crashes, three dedicated verification tools were introduced:

1. **`tools/riscv_enc_check.py` (T1: Encoding Oracle)**:
   - Part A: Fuzzes the round-trip encoding/decoding of all canonical RISC-V instruction formats (`I`, `S`, `B`, `J`, `U`) across their full ranges (including 21-bit signed J-type range `[-1048576, 1048574]`).
   - Part B: Cross-checks formulas against an independent pure-Python class-level reference (vendored from celebi with corrected S-type string packing), bypassing binutils `as` constant-displacement branch relaxation issues.
   - Part C: Scans zerocc raw `.o` text to verify no compressed instructions and all branch/jump targets land on valid 4-byte aligned offsets in `.text`.
2. **`tools/reloc_pair_check.py` (T2: Relocation Pairing & Section Validator)**:
   - Uses `tools/elf_min.py` to audit `.rela.*` records and symbols in raw objects.
   - Verifies every `R_RISCV_PCREL_LO12_I` / `_S` is preceded by a matching `R_RISCV_PCREL_HI20` on the corresponding `auipc`.
   - Catches fatal GNU ld rejections like `SECTION_SYMBOL_ADDEND` (when a PC-relative load references a section symbol such as `.rodata` with a non-zero addend) and section symbol `shndx` mismatches.
3. **`tools/analysis/vtable_check.py` (T3: VTable Conformance & Caller Auditor)**:
   - Audits `ZeroCCISABackend` definitions against `isa_backend.h` and scans all `BE(c)->` / `backend->` call sites across `src/`.
   - **Full VTable Parity (Sprint 89)**: All **221/221** backend slots in `ZeroCCISABackend` are now initialized across all 4 targets (`x86_64`, `aarch64`, `riscv64`, `riscv32`), with **0 missing slots** between `riscv64_backend` and `riscv32_backend` (`python3 tools/analysis/vtable_check.py --diff riscv64 riscv32`).
   - `riscv32_backend` is wired to `riscv64_init_asm_emission(&riscv32_backend)` in `zero_cc_init_backends_once()`, with word-sized `sw`/`lw` awareness (`BE(c)->isa == ISA_RISCV32`) in `riscv_asm.c`, `riscv.c`, and `asm_emitter.c`.

## Cross-ISA `OP_SYSCALL` (`__zerocc_syscall`, Sprint 89)

`__zerocc_syscall(nr, a1, a2, a3, a4, a5, a6)` lowers to `OP_SYSCALL` (`ir_emit_syscall`) and is implemented in both the raw machine-code emitters and GAS `.s` assembly emitters across all 4 target ISAs:

- **x86-64 (`x86_emit_syscall`, `x86_emit_syscall_asm`)**: Linux System V AMD64 ABI (`rax = nr`, `rdi, rsi, rdx, r10, r8, r9 = a1..a6`, `syscall`, result in `rax` / `VREG_0`). Preserves caller-saved GPRs (`rcx`, `r11`) clobbered by `syscall`.
- **AArch64 (`aarch64_emit_syscall`, `aarch64_emit_syscall_asm`)**: Linux AArch64 ABI (`x8 = nr`, `x0..x5 = a1..a6`, `svc #0`, result in `x0` / `VREG_0`). Saves/restores 13 allocatable GPRs on a 16-byte aligned frame (`112` bytes) and loads spilled temporaries via the pre-decrement `sp` frame.
- **RISC-V 64 & 32 (`riscv64_emit_syscall`, `riscv_emit_syscall_asm`)**: Linux RISC-V ABI (`a7 = nr`, `a0..a5 = a1..a6`, `ecall`, result in `a0` / `VREG_0`). Uses word-sized `sw`/`lw` (`4`-byte slots) on `ISA_RISCV32` and `sd`/`ld` (`8`-byte slots) on `ISA_RISCV64`.
- **Differential Verification (`test/isa_qemu/13_syscall.c`)**: Exercises `SYS_getpid`, `SYS_write`, and `SYS_brk` across all 5 ISAs under `make test-isa-qemu` (80/80 green across all 5 ISAs as of Sprint 189, including `15_nested_tramp.c`).

## Trampoline Stubs (Sprint 93 Phase 3b; cross-ISA Sprints 97, 189)

Real computed-goto trampolines for indirect calls through nested function pointers (`f = inner; f(x)`), supported across all 5 target ISAs (**x86-64, AArch64, AArch32, RISC-V64, and RISC-V32**):

- Each capturing nested function gets a static **context object** (`__zf<N>_<name>_ctx`, 32 bytes per capture slot on 64-bit ISAs, 16 bytes on 32-bit ISAs) populated at address-taking time (`zc_lower_fn_value` in `ast_lowering_expr.c`):
  - **By-value capture**: copies the captured value's words into the context object.
  - **By-reference capture**: stores the pointer value (the enclosing slot's address) - via `zc_sym_addr`, never `zc_sym_load` (that dereferences and stores the pointee's VALUE; a deeper function then dereferences an int as a pointer and segfaults).
  - **Multi-level capture**: stores the pointer value of the intermediate capture (mirror param).
  - **Large context (>32B / >16B)**: stores the address of the context object itself.

- **Stub function** with the user-facing signature (no hidden context param): emitted right after the real nested function in the same text image (`x86_64_emit_tramp_stub` / `aarch64_emit_tramp_stub` / `aarch32_emit_tramp_stub` / `riscv64_emit_tramp_stub`, shared unit model in `src/zero_cc/isa/tramp_stub.h`). The stub:
  1. Rebuilds the full ABI argument stream (`w_sz`-byte units per class: integer register slots + overflow, FP register slots + overflow, struct hidden pointer; the entry geometry uses **separate namespaces** - register slot 0..N vs shared caller-stack unit index 0.. - which is why the unit model carries `ue_reg[]` and `ue_stk[]` separately).
  2. Loads context object values into the appropriate argument registers (hidden capture params first, then user args).
  3. Ensures stack alignment (16-byte on x86-64/AArch64/RISC-V, 8-byte on AArch32) with a **pre-push pad** (odd unit count pads the frame BEFORE any push - matching the OP_CALL lowering model).
  4. `call`s the real nested function; the real function's `ret` lands on the stub's epilogue, returning to the ORIGINAL caller.

  **Sprint 100 (VLA struct members)**: a by-value parameter or capture of a struct/union with variably-modified members occupies **2 GPR units** in the unit model (`tramp_unit_gpr_slots` in `tramp_stub.h` + the x86 local model): unit 0 = object pointer, unit 1 = total runtime size (the by-value ABI is defined in `abi-sysv-x86_64.md`). This is what makes `compile/nested-1.c` (`int nested (struct s x)` with `struct s { int a; char b[argc]; }`) PASS: the stub must route both units into the same argument registers the real function's prologue reads.

- **ISA stub-encoding invariants** (each bug below was found via a QEMU miscompile/segfault):
  - **x86-64**: `movaps xmm,mem` (load) = `0F 28 /r` but `movaps mem,xmm` (store) = `0F 29 /r` - the entry-save loop must use the STORE form; XMM entry-save offsets beyond disp8 (-144) need the disp32 `41`/`44`/`45` REX form; loading XMM from the frame uses `movdqu` (`F3 0F 6F`, 16B) / `movsd` (`F2 0F 10`, 8B) - `F2 0F 6E` is not a valid 16-byte load; entry stack args at `[rbp+16+8m]`; `call rel32`'s PC is the NEXT instruction (the +5 relative to the `call` itself).
  - **AArch64**: LDUR/STUR are the **immediate-offset family** (imm9 signed, two's-complement wrap = 512: STUR int/FP `0xF8000000`/`0xFC000000`, LDUR `0xF8400000`/`0xFC400000`), distinct from the 12-bit 8x-scaled LDR/STR unsigned-offset family (`0xF94`/`0xF90`/`0xFD4`/`0xFD0`) - pasting one family into the other silently loads the wrong register; offsets >255 use `sub x10, x29, #off` + plain `[x10]` access; `bl` is PC-relative with PC = the `bl`'s own address (no +4) and does not push LR (first stack arg at `[x29+16]`); 8-byte `str/ldr` push/pop (`stp/ldp` pairs were wrong for the stub's single-register geometry).
  - **AArch32**: Prologue creates standard frame link `push {r11, lr}`, saves incoming GPRs `r0..r3` at `[r11-4..-16]`, VFP doubles `d0..d7` at `[r11-24..-80]`, and caller stack args at `[r11+8+4m]`. Materializes ctx address using `movw`/`movt` into `r14` (`lr` is preserved on entry), populates `r0` with ctx, reloads user args into `r1..r3`/`d0..d7` or stack, and invokes inner nested function via `bl <inner_fn>`.
  - **RISC-V**: `jal` is PC-relative with PC = the `jal`'s own address (no +4); the stub must `addi sp,sp,-D` after `mv s0,sp` (without it the entry saves overwrite the caller's live stack); D > 2047 uses `li t0, D; sub sp,sp,t0`; R-type `sub` is funct7 0x20 (funct7 0x01 is `mul`) and `and` is funct3 7 / funct7 0. RV32 uses `lw`/`sw` (`w_sz=4`) and `fcvt.w.d` (`rs2=0`), while RV64 uses `ld`/`sd` (`w_sz=8`) and `fcvt.l.d` (`rs2=2`).
  - **Direct calls to nested functions on RISC-V** (`riscv64_emit_call`): the early-patch path (baking `fn->text_offset - pos` into the `jal`) is only valid for already-emitted functions. Nested functions are emitted AFTER their enclosing function (deferred), so their `text_offset` is still 0 - early-patching bakes a jump to text offset 0 (the first function = the enclosing scope) and infinite-recurses until the stack overflows. Nested targets are routed through the `R_RISCV_JAL` relocation instead (the .o + system-linker path resolves it at final offsets); AArch64/x86 already use call relocations and are unaffected.

## GNU `__int128` (128-bit integer) Support (Sprint 109)
A 128-bit integer value is a **pair of GPRs** (`REG_CLASS_GPR128`: low in `assigned_reg`, high in `assigned_reg_hi`); each 128-bit op lowers to a `(lo,hi)` decomposition built from 64-bit primitives. All three 64-bit architectures (**x86-64, AArch64, RISC-V64**) are **fully verified** across `-O0..-O3` in both JIT and AOT modes (`test-opt`, QEMU ISA battery, and comprehensive 128-bit test suites).
- **Register allocation & pair pools**: AArch64 and RISC-V64 allocate real `(lo,hi)` register pairs (callee-saved `x19..x28` / `s2..s11`). The 128-bit spill path allocates two contiguous 8-byte stack slots (`spill_slot` and `spill_slot - 8`), saving and reloading both low and high halves without loss.
- **Dedicated large-offset address scratch**: Large stack frames (>256B on AArch64, >2048B on RISC-V) require dedicated address scratch registers that never collide with value temps or spill scratch:
  - **AArch64**: `x16` (IP0) is reserved exclusively for large frame offset address calculations in both MC and ASM backends. GPR128 spill scratch pairs use `(x8, x17)` and `(x15, x18)` with `x15` reserved in `regalloc.h`.
  - **RISC-V**: `t5` (`x30`) is reserved as the dedicated scratch register for large-offset loads/stores, removed from the allocatable pool and syscall clobber masks.
- **Arithmetic & Comparison Codegen**:
  - **x86-64**: `addq`/`adcq`, `subq`/`sbbq`, `mulq` (low/high cross products), bitwise ops, shifts, and two's-complement negation (`notq; addq $1; notq; adcq $0`).
  - **AArch64**: `adds`/`adc`, `subs`/`sbc` (using flag-setting S=1), unsigned high multiply `umulh`, bitwise logic, and `negs`/`sbc`.
  - **RISC-V64**: Unsigned carry/borrow via `sltu`, `mul`/`mulhu`, bitwise logic, and two's complement negation.
- **128-bit division/modulo** has no single instruction on any target, so it is **not inlined** and **not delegated to libgcc** (musl/`zero_rt` ship no `__divti3`-style helper, and `__int128` is a freestanding compiler extension). It calls a **ZeroCC-owned portable** software routine `zero_rt/core/int128.c` (`zc_udiv128`/`zc_umod128`/`zc_sdiv128`/`zc_smod128`) that operates on **64-bit halves only** (Knuth-style multiword, C99 trunc-toward-zero, `INT128_MIN/-1` handled). The lowering passes the two operands as four 64-bit halves and recombines the `(lo,hi)` result via `MK128`; the helper is registered in the TU symbol table for the **AOT** object (JIT resolves it via `dlsym` of the zerocc binary). The routine is pure C, so its correctness is ISA-independent (verified against GCC); division runs end-to-end on x86-64, AArch64, and RISC-V64 under QEMU.
- **Return ABI & Caller Protection**: Functions returning `__int128` place low in `VREG_0` and high in `VREG_2` (canonical mapping to `rdx` on x86-64, `x1` on AArch64, `a1` on RISC-V). In `live_range.h`, post-call copy windows protect `VREG_0` and `VREG_2` as dedicated hardware intervals to prevent physical register reuse before callers capture return halves.
- **Argument ABI (Sprint 110)**: A by-value `__int128`/`__uint128_t` argument occupies **two consecutive integer-arg units** (low half in unit N, high half in unit N+1 — `rdi:rsi`/`rdx:rcx`/`r8:r9` on x86-64, `xN:xN+1`, `aN:aN+1`); past the register window the 16 bytes ride the caller stack in argument order (low unit at the lower address), and a pair may **straddle** the window (low in a register, high on the stack). The caller normalizes the value to the parameter's width (narrow args zero/sign-extend via `ZEXT64_128`/`SEXT64_128` per the argument's signedness; a 128-bit value to a 64-bit parameter truncates via `TRUNC128_64`) and records two int-arg entries (`HI128` splits the high half); the callee reuses the 16-byte struct chunk pattern (two `PARAM_SAVE_INT`s, chunk 1 at `stack_offset - 8`). 128-bit parameters disable frameless param promotion (the promoted-vreg model has no pair form). Two parser prerequisites were fixed: `f(__int128 a)` was misread as a **K&R two-name list** (registering two int params — the 128-bit base names are now excluded from the bare-name scan in `zero_cc_parse_fn_param_block`), and a bare 128-bit base name in parameter position was defaulting to `int64` (now mapped via `zc_name_int128_kind`). Covered by `test/jit/int128_args.c` (96/96 in the cross-ISA isa128 gate).
- **128-bit function-pointer signatures & O2 inlining (Sprint 110 follow-up)**: `__int128`/`__uint128_t` now appear in function-pointer parameter and return types. Two fixes: (1) `type_is_compatible` gained a `TYPE_KIND_INT128` arm — without it two identical 128-bit types compared *incompatible*, so pointer-to-function compatibility failed and `__uint128_t (*fp)(__uint128_t) = id_u;` was rejected. (2) the small-function inliner (`ast_lowering_call.c`) lost the high half of 128-bit values in inlined bodies: the inlined-param substitution didn't set `last_expr_is_128` (so `v >> 64` lowered as `HI128(ZEXT64_128(v_lo)) = 0`), the arg loop lacked 128-bit width normalization, and the inlined `return` used `OP_MOV_REG` (one 64-bit half). Added `param_is_128[8]` to the inlining state, normalized arg width (zext/sext/trunc), set `last_expr_is_128` on substitution and on inlined 128-bit returns, and widened+`MOV128` the inlined return. 128-bit scalar returns are now inlining-eligible (`compile.h`: `ret_struct_sz <= 8 || ret_is_128_scalar`). Regression: `test/jit/int128_fnptr.c` (in the cross-ISA isa128 gate; static helpers are inlined at `-O2`).
- **Remaining gaps (tracked for follow-up)**: RISC-V32 has no 128-bit pair pool yet. True binary128 **precision** is not implemented (see `__float128` below: the object/value model is in place, computed as double).
- **Text-AOT Cross-ISA Fixes (Sprint 151b)**:
  - **x86-64 `mul128_mixed` opcode byte**: line 159 of `src/zero_cc/isa/x86_64.c` defined `mul_op` as `{0xFF}` (`/4` encodes `jmp *%reg`), crashing on register-form 128-bit multiplications; corrected to `{0xF7}` (`mul %reg`).
  - **Text-AOT Call FP Spill Reloads**: in `x86_64_asm.c`, `aarch64_asm.c`, and `riscv_asm.c`, `emit_call_asm` failed to check if an FP argument temp was spilled (`phys_reg == -1`, `sp >= 0`), defaulting to register 0 and omitting the stack reload; added `emit_load_fp_asm` from `spill_base_offset + (sp + 1) * 8`.
  - **RISC-V Text Comparison Flag Materialization**: `riscv_emit_cmp_asm` previously emitted `sub t0, src1, src2; mv t1, src1; mv t2, src2` while `riscv_emit_cond_set_asm` read `t1/t2` for unsigned comparisons. When both operands spilled (`src1 == t0`, `src2 == t1`), the subtract clobbered `t0` before the moves, leaving both `t1` and `t2` holding `src1 - src2` and making `t2 <u t1` evaluate to 0 (broke unsigned and 128-bit relational comparisons in `int128_arith.c`). Aligned with the MC path: `riscv_emit_cmp_asm` is a no-op, operands are recorded in `c->lowering.cmp_src1/2`, and `riscv_emit_cond_set_asm` + branch emitters emit `slt`/`sltu`/`beq`/`bne`/`blt`/`bge` directly from operand registers.

## RV32 64-bit Integer Arithmetic (`REG_CLASS_GPR64`, Sprint 170)
On 32-bit RISC-V (`--target riscv32`, ILP32 ABI), 64-bit integers (`long long` / `int64_t`) are decomposed into **(lo,hi) 32-bit GPR pairs** (`REG_CLASS_GPR64`), mirroring the GPR128 pair allocation model. See the comprehensive [RV32 64-bit Integer Support Roadmap](./rv32-i64-roadmap.md) for the 4-phase design (Phase 1: Core arithmetic, Phase 2: psABI calling convention, Phase 3: libgcc div/rem, Phase 4: sanitizer width-8 checks).
- **Register Allocation**: `gpr128_alloc_pair` allocates adjacent callee-saved registers (`s1`, `s2..s11`). Spilled temps occupy an 8-byte stack slot (`slot*8` for lo, `slot*8 + 4` for hi). Temporary renumbering pool count expanded to `TR_POOL_COUNT = 8 * 256`.
- **Scratch Invariants**: `t0` (5) and `t5` (30) serve as carry/shift scratch. `t2` (7) is reserved as unallocatable on rv32 to serve as third multiply/scratch.
- **Operations & Emitters**:
  - `ADD64`/`SUB64`: 32-bit `add` + unsigned overflow carry `sltu t2, sum, alo` / borrow `sltu t0, blo, alo`.
  - `MUL64`: low 64 bits of 64x64 product: `alo*blo` + cross terms `ahi*blo + alo*bhi`.
  - `AND64`/`OR64`/`XOR64`/`NOT64`/`NEG64`: elementwise 32-bit bitwise logic; two's-complement negation with carry.
  - `SHL64`/`SHR64`/`SAR64`: 3-way branch `<32`, `32..63`, `>=64` with cross terms read before dest clobber; alias-safe.
  - `CMP64` + predicate materialization: `EQ`, `NE`, `LT`, `LE`, `GT`, `GE` signed/unsigned comparisons.
  - `MOV64_IMM`: loads low word into `dlo`, high word into `dhi`.
  - `LOAD64`/`STORE64`: adjacent 32-bit memory access with alias-safe load ordering.
- **Frontend Lowering**: Intercepts 64-bit binops, compound/simple assignments (`ast_lowering_assign.c`), local declaration initializers (`ast_lowering_stmt.c`), and identifier loads (`ast_lowering_expr.c`), normalizing constants via `zc_operand_to64`.

## GNU `__float128` (quad precision) Support (Sprint 112)
`__float128` is a 16-byte, 16-aligned scalar floating type. ZeroCC models it with the **same object/value model as `long double`**: the value travels through the 64-bit double value model (value in the low 8 bytes of the 16-byte storage); true binary128 (113-bit significand) arithmetic is **not** implemented. This is a deliberate v1: the compiler's FP pipeline is IEEE-754 double throughout (FADD/FSUB/... and the FP register ABI are 64-bit), so the full cost of a 128-bit FP value model (software mul/div, 3-register pass-by-value, libm) is not justified yet. Values exactly representable in double produce results identical to a true binary128 implementation, which is what the regression gate relies on.
- **Type plumbing**: `zero_cc_type_float128()` (tag `TYPE_DOUBLE`, size/align 16, `is_float128` flag). The flag mirrors `is_long_double` in `ZeroCCType` (incl. struct members), `TypeInfo`, the JIT `ZeroCCTypeInfo` bridges (both directions + equality), `type_size`/`type_align` (16), and the 16-byte slot store paths (`ast_lowering_assign.c` stores the 8-byte double and **zeros the top 8 bytes** so the full 16-byte object is deterministic; `compile_unit.c` global initializers emit the double bits + 8 zero bytes).
- **Name recognition** (tokenized as an identifier): `is_type_specifier_at`, `zero_cc_tok_is_type_start` (sizeof/_Alignof), `zc_parse_basic_type_spec`, `zero_cc_parse_type_name_at_pos`, the cast-target resolver, cast detection in `zero_cc_type_of_unary`, function **return** types and **parameter** types in `compile.h` (with the K&R-shape exclusion, as for `__int128`), and struct member parsing (`decl_struct.c` size pass + 4 registration sites).
- **ABI**: by-value arguments and returns use the **FP register stream** exactly like `double` (x86-64 `xmm0`/`xmm1`..., AArch64 `d0`/`d1`..., RISC-V `fa0`/`fa1`...) — the System V convention for the 64-bit half; the callee reads only the low 8 bytes, which is the entire stored value. Structs containing `__float128` members follow the struct ABI (16-byte alignment drives layout; by-value structs >16B use `sret`).
- **MC-path fix found by this work (all FP args, not float128-specific)**: the raw machine-code `OP_CALL` handler loaded FP register args from a **fixed 64-entry** `call_fp_spills` side table (per function); calls past the 64th in a function silently passed **garbage** FP args (the `-S` emitter uses the dynamically-grown `IRCallArgs` table and was unaffected — a raw-AOT-only miscompile that `-S` diffing would not have caught). The MC handler now falls back to the `IRCallArgs` reload temps (register move or spill-slot load; FP **stack** args past the cap spill to `[rsp-8]` before the push, mirroring the `-S` emitter), and `call_idx` advances unconditionally (the old code also read the capped table out of bounds for FP stack args past the cap). Covered by `test/jit/int128_float128.c` (main makes 143 calls; the FP-arg calls sit past the 64th).
- **Verified**: `test/jit/int128_float128.c` (arithmetic, comparisons, casts incl. float128→int, globals, struct members + pass-by-value structs, arrays, pointers, ternary, 143-call main) passes at `-O0..-O3` in JIT and both AOT paths on x86-64 (native), AArch64 and RISC-V64 (QEMU), diffed against host `gcc -static` in `make test-isa128` (144/144).

## GPU Acceleration Architecture & Separate Project Boundary (zero_gpu Repository)

Heterogeneous acceleration compiles compute kernels to run on massively parallel GPU shader cores. Following an architectural review, ZeroCC adopts a strict **domain separation** between CPU/baremetal compilation and GPU compute:

```mermaid
flowchart TD
    subgraph ZeroCC ["ZeroCC Core (C99/C11/C23)"]
        direction TB
        C_SRC["Host C Application"] --> ZCC_PARSE[C Parser / Semantics]
        ZCC_PARSE --> ZCC_IR[Flat Linear IR]
        ZCC_IR --> ZCC_RA[Linear-Scan Regalloc]
        ZCC_RA --> ZCC_BE[ZeroCCISABackend (CPU/Baremetal)]
        ZCC_BE --> ZCC_LINK[Own Linker / ELF / JIT]
    end

    subgraph ZeroGPU ["zero_gpu (Separate Repository)"]
        direction TB
        GPU_SRC["Kernel Source / DSL"] --> GPU_FE[Kernel Parser]
        GPU_FE --> GPU_SSA[SSA IR (Address Spaces)]
        GPU_SSA --> GPU_SIMT[SIMT Optimizations (Divergence / Coalescing)]
        GPU_SIMT --> PTX_OUT["NVIDIA PTX Text (.ptx)"]
        GPU_SIMT --> SPV_OUT["Khronos SPIR-V Binary (.spv)"]
    end

    SPV_OUT -.->|Export C Header / Object| ZCC_LINK
    PTX_OUT -.->|Export C Header / In-Memory String| ZCC_LINK
```

### Why GPU Compilation is Decoupled to a Separate Repository

Attempting to force GPU targets into ZeroCC's core C pipeline introduces severe architectural impedance mismatch:

1. **Register Allocator & Stack Frame Mismatch**:
   - ZeroCC's pipeline performs physical register allocation (`regalloc.h`) with live-interval spilling into an activation record stack frame.
   - GPU targets have no physical register allocation or stack frames: **PTX** uses infinite virtual registers (`.reg .b32 %r<n>`), leaving allocation to the NVIDIA driver (`ptxas`), while **SPIR-V** is pure SSA (`%id`). Forcing them through ZeroCC would require bypassing register allocation and frame generation entirely.
2. **Memory Hierarchy & Address Spaces**:
   - ZeroCC assumes a single flat 32/64-bit pointer address space.
   - GPUs require distinct address spaces in types and pointer operations: `__global` (VRAM), `__shared` (workgroup scratchpad), `__constant`, and `__private`.
3. **Execution Model**:
   - ZeroCC compiles for sequential CPU threads with POSIX calling conventions.
   - GPUs execute in SIMT (Single Instruction, Multiple Threads) warps/subgroups requiring divergence analysis, barrier synchronizations (`bar.sync` / `OpControlBarrier`), and grid/block parameter staging.
4. **Binary & Object Formats**:
   - SPIR-V is a stream of 32-bit integer words (magic `0x07230203`), and PTX is UTF-8 text. Neither conforms to ELF relocatable objects (`elf_writer.c` and `linker.c`).

### Dedicated Architecture in Independent `zero_gpu` Repository
Established as an independent project in its own repository, `zero_gpu` provides:
- **Trivial Bytecode / Text Emission**: SPIR-V is cleanly serialized via structured 32-bit word encoders, and PTX is generated using dedicated emitter templates.
- **Fast SSA Transformations**: SSA dominance frontiers, dead-code elimination, and memory coalescing passes are implemented with high developer velocity.
- **Direct Driver JIT Testing**: Dynamic loading of `libcuda.so` (`cuModuleLoadDataEx`, `cuLaunchKernel`) or `libvulkan.so` without compiling complex C host harnesses.
- **ZeroCC Integration**: `zero_gpu` compiles kernels to `.spv` or `.ptx` and exports C header arrays (`const uint32_t my_kernel_spv[] = {...};`) or embedded binaries, which ZeroCC compiles directly into the host binary and links via `--zerocc-link`.

---

## Target Architecture Refactoring & Extension Roadmap (Baremetal & Native CPUs)

ZeroCC currently targets four architectures (x86-64, AArch64, RV64GC, RV32GC), with AArch32 in progress. While functional, the backend contains cross-cutting target leaks (over 80+ `if (c->isa == ...)` checks in AST lowering, `asm_emitter.c`, `regalloc.h`, and `linker.c`).

The following 5-phase refactoring roadmap decouples the target subsystem, making the addition of new native and baremetal ISAs (e.g. AArch32, MIPS, Cortex-M, RV32E) modular and robust.

### 1. `TargetTraits` / `TargetConfig` Extraction
Replace scattered ternary checks (`(c->isa == ISA_RISCV32) ? 4 : 8`) and ISA conditionals with a unified target traits struct:
```c
typedef struct TargetTraits {
    uint8_t  ptr_size;            /* 4 or 8 */
    uint8_t  word_size;           /* 4 or 8 */
    uint8_t  stack_align;         /* 16 for x86/arm64, 4/8/16 for others */
    bool     has_red_zone;        /* true on x86-64 */
    bool     has_flags_register;  /* true on x86/arm, false on riscv */
    int      ret_reg1;            /* RAX / X0 / A0 / R0 */
    int      ret_reg2;            /* RDX / X1 / A1 / R1 */
    int      sret_reg;            /* RDI / X8 / A0 / R0 */
    bool     sret_consumes_arg0;  /* false on AArch64 (x8 dedicated), true on SysV/RISC-V */
    uint16_t elf_machine;         /* EM_X86_64, EM_AARCH64, EM_RISCV, etc. */
    uint8_t  elf_class;           /* ELFCLASS32 / ELFCLASS64 */
    uint32_t dwarf_addr_reloc;    /* Target relocation type for address */
    uint32_t dwarf_sec_reloc;     /* Target relocation type for section offset */
} TargetTraits;
```
*Impact:* Eliminates over 40 ad-hoc ternary checks in AST lowering, `dwarf.c`, and `main.c`.

### 2. Table-Driven Register Allocator Configuration
Extract register reservations from `regalloc.h` into declarative tables inside `ISARegInfo`:
- `regalloc_set_isa(ra, isa)` is replaced by `regalloc_init_from_target(ra, backend->reg_desc)`.
- Physical register counts, allocatable masks, callee-saved masks, and scratch reservations become data tables defined in `src/zero_cc/isa/<name>.c`.
*Impact:* Adding a new target to the register allocator requires zero code changes in `regalloc.h`.

### 3. Decoupled ABI Lowering Hooks
Move ABI-specific calling convention details out of `ast_lowering_call.c` and `ast_lowering_varargs.c` into backend vtable hooks:
- `be->classify_arg(type, ...)`: Classifies arguments into GP registers, FP registers, stack, or split pairs.
- `be->classify_return(type, ...)`: Classifies return types as direct register return or hidden `sret`.
*Impact:* Removes hundreds of lines of ABI-specific branching from common AST lowering.

### 4. Modular Linker Relocation Ops (`TargetLinkerOps`)
Decouple `src/zero_cc/linker/linker.c` by moving architecture-specific relocation resolution and PLT stub generation into target-registered tables:
```c
typedef struct TargetLinkerOps {
    uint16_t machine;
    uint32_t default_flags;
    uint64_t default_text_base;
    int (*apply_reloc)(ZcLink *L, const ZcReloc *rel, uint8_t *site,
                       uint64_t P, int64_t S, int64_t A, char *err, size_t errsz);
    void (*write_plt_entry)(uint8_t *stub, uint64_t got_vma, uint64_t plt_vma);
} TargetLinkerOps;
```
*Impact:* `linker.c` becomes a clean, universal ELF engine. New architectures are registered in `src/zero_cc/linker/targets/<name>.c`.

### 5. Baremetal CPU Target Support
Extend ZeroCC for microcontrollers and embedded cores (Cortex-M, RV32E, Xtensa):
- **Target OS / Environment**: Introduce `TargetOS` (`OS_LINUX`, `OS_BAREMETAL`).
- **LMA vs. VMA Section Mapping**: Support linker scripts (`-T script.ld`) in `linker.c` allowing `.text` and initial `.data` to be loaded in Flash (LMA) while executing from SRAM (VMA).
- **Baremetal Runtime (`zero_rt_baremetal.a`)**: Provide an OS-free `crt0` (reset handler, vector table, BSS zeroing, Flash-to-RAM data copying) and stubbed I/O without Linux syscall dependencies.
- **Reduced Register Sets**: Support 16-register embedded targets (e.g. RV32E) in `TargetRegDesc`.

### 6. ARM SVE / SVE2 Scalable Vector Extension (Sprint 203)
ZeroCC implements scalable vector length support for AArch64 via ARM SVE and SVE2:
- **CLI & Feature Flags**: `-msve`, `-mno-sve`, `-msve2`, `-mno-sve2`. Preprocessor flags define `__ARM_FEATURE_SVE` and `__ARM_FEATURE_SVE2` when targeting AArch64 with `-msve`/`-msve2`.
- **ACLE Header (`<arm_sve.h>`)**: Virtual header in `src/zero_cc/core/virtual_headers.h` providing ACLE vector types (`svint8_t`..`svint64_t`, `svuint8_t`..`svuint64_t`, `svfloat32_t`, `svfloat64_t`, `svbool_t`), vector length helpers (`svcntb`, `svcnth`, `svcntw`, `svcntd`), predication constructors (`svptrue_b*`, `svpfalse_b`, `svwhilelt_b*`), predicate-driven memory access (`svld1_*`, `svst1_*`), arithmetic (`svadd_*`, `svsub_*`, `svmul_*`, `svdiv_*`), bitwise logic (`svand_*`, `svorr_*`, `sveor_*`), comparisons (`svcmpeq_*`, `svcmpgt_*`), reductions (`svaddv_*`), and SVE2 saturating arithmetic (`svqadd_*`, `svqsub_*`).
- **Software Emulation Fallback & JIT**: On x86-64 native and targets without hardware SVE, builtins lower to 512-bit vector simulation (16 lanes of 32-bit elements, 8 lanes of 64-bit elements, etc.) with predication masking.
- **AArch64 Hardware Emission**:
  - `src/zero_cc/isa/aarch64_asm.c`: Formats textual SVE instructions (`ptrue`, `pfalse`, `whilelt`, `cntb/w/d`, `ld1b/w/d`, `st1b/w/d`, `add`, `sub`, `mul`, `fdiv`, `and`, `orr`, `eor`, `cmpeq/cmpgt`, `uaddv/faddv`, `sqadd/sqsub`).
  - `src/zero_cc/isa/aarch64.c`: Emits raw machine code for SVE instructions. Predicated arithmetic operations (`add`, `sub`, `mul`, `div`) use exact SVE encodings with `(pg & 7) << 10`, `(zm & 0x1F) << 5`, and `zd & 0x1F`, and register copy via `0x04603000` (ORR).
  - Active predicate registers bind to `p0`, and predicate bitmasks compute into GPRs for standard C logic and control flow. `OP_SVE_CNT` destination temporary is classified as `REG_CLASS_GPR`.

### 7. RISC-V Vector Extension (RVV 1.0 - Sprint 204 & Phase 12)
ZeroCC implements the standard RISC-V Vector Extension (RVV 1.0 — `Zve32x` / `Zve64d` / `V`):
- **CLI & Feature Flags**: `-mrvv`, `-mno-rvv`, `-march=rv64gcv` / `-march=rv32gcv`. Defines preprocessor macros `__riscv_v_intrinsic` (1000000) and `__riscv_vector` (1).
- **Virtual Header (`<riscv_vector.h>`)**: Virtual header in `src/zero_cc/core/virtual_headers.h` providing vector types (`vint8m1_t`..`vint64m1_t`, `vuint8m1_t`..`vuint64m1_t`, `vfloat32m1_t`, `vfloat64m1_t`, `vbool*`), vector length configuration (`__riscv_vsetvl_e*`, `__riscv_vsetvlmax_e*`), unit-stride loads/stores (`__riscv_vle*`, `__riscv_vse*`), strided loads/stores (`__riscv_vlse*`, `__riscv_vsse*`), indexed unordered loads/stores (`__riscv_vluxei*`, `__riscv_vsuxei*`), vector arithmetic (`vadd`, `vsub`, `vmul`, `vdiv`, `vfadd`, `vfsub`, `vfmul`, `vfdiv`), bitwise logic (`vand`, `vor`, `vxor`), comparisons (`vmseq`, `vmslt`, `vmfeq`, `vmflt`), reductions (`vredsum`, `vfredusum`), broadcasts (`vmv_v_x`, `vfmv_v_f`), and scalar extracts (`vmv_x_s`, `vfmv_f_s`).
- **Software Simulation Fallback**: Builtins lower to scalar/lane simulation on non-RVV hosts and JIT runs, allowing portable execution of vector intrinsics including strided and indexed gather/scatter operations.
- **Hardware Codegen & Vector State Invariants**:
  - `src/zero_cc/isa/riscv64.c`: Direct binary MC emission for RVV 1.0 instructions:
    - Unit-stride: `vle<eew>.v` (`0x02000007`), `vse<eew>.v` (`0x02000027`)
    - Strided: `vlse<eew>.v` (`0x0A000007`), `vsse<eew>.v` (`0x0A000027`)
    - Indexed: `vluxei<eew>.v` (`0x06000007`), `vsuxei<eew>.v` (`0x06000027`)
  - `src/zero_cc/isa/riscv_asm.c`: Textual assembly emission using standard GNU as RVV syntax (`vlse32.v vd, (rs1), rs2`, `vsse32.v vs3, (rs1), rs2`, `vluxei32.v vd, (rs1), vs2`, `vsuxei32.v vs3, (rs1), vs2`).
  - Store instruction liveness invariant: `OP_RVV_VSSE` and `OP_RVV_VSUXEI` treat `temp_dest` as vector value USE (not definition), preventing incorrect SSA def tracking and clobbers.
  - Default `vsetivli zero, <lanes>, e<esz>, m1, ta, ma` insertion ensures active `vtype.sew` and `vl` state before vector operations; bypassed when `c->rvv_dynamic_vl` is active (set on dynamic `OP_RVV_VSETVLI` in scalable vector loops).
  - Scalar float conversion: `fcvt.s.d` ensures single-precision values are NaN-boxed into 64-bit FP registers before `vfmv.v.f`, and `fcvt.d.s` restores extracted scalars to double precision.

---

## 10-Phase Checklist for Adding a New CPU/Baremetal ISA to ZeroCC

Use this checklist when introducing a new target ISA:

### Phase 1: Target Definition & CLI Plumbing
- [ ] 1.1 `src/zero_cc/isa/isa_backend.h`: Add `ISA_<NAME>` to `ZeroCCISA` enum.
- [ ] 1.2 `src/zero_cc/jit/elf_emitter.h`: Add `EM_<NAME>` ELF machine ID mapping.
- [ ] 1.3 `src/zero_cc/jit/elf_writer.h`: Add `ELF_MACHINE_<NAME>` constant.
- [ ] 1.4 `src/main.c`: Add target string parsing (`--target <name>`), configure bitness (`is_32`), default relocations, runtime library name (`zero_rt_<name>.a`), and target preprocessor macros (`__<isa>__`, `__LP64__` or `__ILP32__`).
- [ ] 1.5 `src/zero_cc/isa/backend_registry.c`: Register ISA init function and dispatch in `zero_cc_get_isa_backend()`.

### Phase 2: Calling Convention & Register Pool (psABI)
- [ ] 2.1 Define physical register count, argument registers (`ARG_REGS[]`), return registers, and frame/stack pointers in `TargetRegDesc`.
- [ ] 2.2 Define callee-saved vs caller-saved sets and scratch register reservations.
- [ ] 2.3 `src/zero_cc/types/registry.h`: Set standard type sizes (pointer, `long`, `size_t`).
- [ ] 2.4 Verify struct alignments, bitfield rules, and integer literal widths for the target data model.

### Phase 3: Machine Code (MC) Encoder & Backend Vtable
- [ ] 3.1 Create `src/zero_cc/isa/<name>.c`:
  - Implement instruction encoding primitives (opcodes, formats, immediate fields).
  - Implement `ZeroCCISABackend` callbacks (`emit_prologue`, `emit_epilogue`, `emit_mov_*`, `emit_load`, `emit_store`, `emit_binop`, `emit_cmp`, `emit_branch`, `emit_call`, `emit_ret`, `emit_atomic_*`).
- [ ] 3.2 `Makefile`: Add `src/zero_cc/isa/<name>.o` to compiler build objects.

### Phase 4: Text Assembly (`-S`) Emitter
- [ ] 4.1 Create `src/zero_cc/isa/<name>_asm.c`:
  - Implement register name mappings and textual assembly formatting.
- [ ] 4.2 Verify with host cross-assembler (e.g. `<arch>-linux-gnu-as` / `gcc -c`).

### Phase 5: ELF Relocatable Object Emission (`-c`)
- [ ] 5.1 `src/zero_cc/jit/elf_writer.c`:
  - Set ELF header fields (`e_machine`, `e_flags` such as EABI/float ABI flags).
  - Add relocation types for text, data, and symbol references (PC-rel calls, GOT/HI-LO pairs).
- [ ] 5.2 Validate emitted `.o` files with `readelf -h -r -s <object.o>`.

### Phase 6: Own Linker Integration (`--zerocc-link`)
- [ ] 6.1 `src/zero_cc/linker/elf_reader.c`: Accept `EM_<NAME>` objects in 32-bit / 64-bit mode.
- [ ] 6.2 `src/zero_cc/linker/linker.c` (or `targets/<name>.c`):
  - Define target default text/data base virtual addresses.
  - Implement relocation dispatch loop for all emitted relocation types (`R_<ISA>_*`).
  - Handle PC-relative offset computations, immediate masking, and overflow checks.

### Phase 7: Freestanding Runtime (`zero_rt`)
- [ ] 7.1 For Linux targets: Implement raw system calls (`zc_syscall*`) in `zero_rt/rt_os.h` and memory/thread routines in `zero_rt/linux/os.c`.
- [ ] 7.2 For Baremetal targets: Implement `crt0.s`, vector table, Flash-to-RAM initialization, and UART I/O stubs.
- [ ] 7.3 Compile runtime library `build/lib/zero_rt_<name>.a`.

### Phase 8: Dynamic Loading (`dynlink.c` — Hosted Only)
- [ ] 8.1 `zero_rt/dlfcn/dynlink.c`:
  - Add default library search paths (`/lib/<triplet>`, `/usr/lib/<triplet>`).
  - Implement `zc_apply_reloc_<name>()` to process runtime relocations (`R_*_JUMP_SLOT`, `R_*_GLOB_DAT`, `R_*_RELATIVE`).

### Phase 9: DWARF 4 Debug Information (`-g`)
- [ ] 9.1 `src/zero_cc/dwarf/dwarf.c`:
  - Configure target pointer/address size (4 vs 8 bytes).
  - Map physical registers to DWARF register numbers (`DW_OP_reg*`).
  - Ensure correct frame unwinding CFI (`.debug_frame`).

### Phase 10: Verification & Differential Conformance Gates
- [ ] 10.1 `tools/toolchain/setup_musl_cross.sh`: Add target static-musl toolchain setup.
- [ ] 10.2 `tools/verify/isa_qemu_diff.py`: Add target to automated QEMU differential gate (`make test-isa-qemu`).
- [ ] 10.3 Run test suite across all `-O0`..`-O3` optimization levels:
  - JIT mode (`--run`) if host-supported, or QEMU user-mode runner.
  - AOT mode with host assembler/linker.
  - AOT mode with ZeroCC's own linker (`--zerocc-link`).

## Decoupled ABI Lowering & Target Traits (Sprint 214 — Item #16 Phase 2 & 3)

Sprint 214 refactored residual architecture checks in register allocation and AST lowering into declarative `TargetTraits` tables and backend vtable hooks:
- **Declarative Register Pool & Scratch (`TargetRegDesc`)**:
  - `num_fp_regs`, `fp_scratch_reg1`, and `fp_scratch_reg2` defined per-target in `target_traits.h`.
  - Replaces hardcoded macros (`REGALLOC_FP_SCRATCH1/2`) with dynamic initialization of `ra->xmm_free` and `ra->vec_free`.
  - Callee-saved register initial masks (e.g. AArch32 r8-r10 frame scratch reservation) defined declaratively via `initial_used_callee_saved_mask`.
- **Decoupled Sret & Return Classification (`zero_cc_abi_type_uses_sret`)**:
  - Replaced hardcoded `ret_struct_sz > 16` and `call_ret_type.size > 16` checks in AST lowering with `zero_cc_abi_type_uses_sret(c, type)` in `abi_classify.h`.
  - Queries `c->isa_backend->classify_return(type)`, handling ABI-specific struct return thresholds across targets (e.g. 16B on x86-64/AArch64/RV64, 8B on RV32, 4B on AArch32).
  - Sret incoming parameter save is decoupled via `traits->sret_consumes_arg0` and `traits->sret_gpr`.
- **Variadic Setup Hook (`emit_variadic_setup`)**:
  - Added `.emit_variadic_setup` to `ZeroCCISABackend`.
  - Extracted architecture-specific `va_start` stream initialization from `ast_lowering_varargs.c` into backend registry implementations (`generic_emit_variadic_setup` and `aarch32_emit_variadic_setup`).

## Modular Linker Relocations & ASM Backend Hooks (Sprint 217 — Item #16 Phase 4 & 5)

Sprint 217 completes Item #16 multi-ISA backend refactoring across linker relocations and text assembly emission:
- **Phase 4: Modular Linker Relocation Ops (`TargetLinkerOps`)**:
  - Extracted architecture-specific far-relocation thunk allocation and PCREL pre-collection from `src/zero_cc/linker/linker.c` into `TargetLinkerOps` vtable hooks (`reserve_thunks` and `precollect_pcrel`) in `src/zero_cc/linker/linker_reloc.h`.
  - Implemented `linker_reserve_thunks_x86_64` for x86-64 far-rel32 thunk reservation and PIE `.rela.dyn` sizing.
  - Implemented `linker_precollect_riscv_pcrel` for RISC-V `R_RISCV_PCREL_HI20` target pre-collection across relocation passes.
  - Completely eliminated hardcoded `L->machine == EM_X86_64` and `L->machine == EM_RISCV` conditional branches in linker section layout and relocation dispatch.
- **Phase 5: Consolidated ASM Emitter Backend Hooks (`ZeroCCISABackend`)**:
  - Added declarative hooks for sized memory access (`emit_load_sized_asm`, `emit_store_sized_asm`) and comparisons (`emit_test_zero_asm`, `emit_cmp_regs_asm`) to `ZeroCCISABackend` in `src/zero_cc/isa/isa_backend.h`.
  - Implemented and registered hooks across all 4 target ISAs (`x86_64_asm.c`, `aarch64_asm.c`, `riscv_asm.c`, `aarch32.c`).
  - Refactored `src/zero_cc/ir/asm_emitter.c` to delegate directly to `BE(c)->emit_*_asm`, removing 120 lines of inline target ISA switching.

---

## Related Concepts
- Return to [Root Index](./index.md)
- See [RV32 64-bit Integer Support Roadmap](./rv32-i64-roadmap.md)
- See [System V AMD64 ABI & Struct Calling Conventions](./abi-sysv-x86_64.md)
- See [Two-Pass IR & Register Allocation](./ir-and-regalloc.md)
- See [Optimization Passes & Safety Invariants](./optimization-passes.md)

### Cortex-M Thumb-2 Instruction Set & Baremetal Execution (-mthumb, Sprint 275)

- **Target Detection & Options (`driver_opts.h`, `symbols.h`, `main.c`)**: `-mthumb` and `-marm` flags, auto-detected on `cortex-m`, `armv7-m`, `armv6-m`, `armv7e-m`, `thumb`. Sets `c->is_thumb = true`.
- **Text Assembly & Function Symbols**:
  - Emits `.syntax unified` and `.thumb` header in Thumb mode.
  - Emits `.thumb_func` before function definitions.
  - Sets bit 0 on defined function symbols in ELF symbol table (`fn_val |= 1`) indicating Thumb execution state.
- **Instruction Encodings (`aarch32_asm.c`, `aarch32.c`)**:
  - Text assembly extracted to `src/zero_cc/isa/aarch32_asm.c` with Thumb-2 prologue (`push {r11, lr}`, `mov r11, sp`), epilogue (`pop {r11, pc}` or `bx lr`), and Cortex-M ISR returns.
  - IT blocks inserted for conditional operations in Thumb mode (`cond_set_asm` and `64_pred_asm`).
  - MC encoders check `c->is_thumb` and emit 16-bit or 32-bit Thumb-2 opcodes (`thumb_emit_u16`, `thumb_emit_u32`).
- **Relocations & Interworking (`linker_reloc.h`)**:
  - Implemented `R_ARM_THM_CALL` (10), `R_ARM_THM_MOVW_ABS_NC` (47), `R_ARM_THM_MOVT_ABS` (48), and `R_ARM_THM_JUMP24` (30).
  - `linker_stub_arm` checks `main_vma & 1` and emits `blx` for ARM-to-Thumb interworking.
- **Verification**: `python3 tools/verify/baremetal_test.py` validates `thumb` target with entry `0x8000001` (bit 0 set), Flash LMA, RAM VMA, and MMIO UART output.

### RV32 Standalone IEEE-754 Soft-Float Runtime (Sprint 276)

- **Freestanding Soft-Float Implementation (`zero_rt/core/soft_float.c`)**:
  - Single precision (binary32): `__addsf3`, `__subsf3`, `__mulsf3`, `__divsf3`, `__negsf2`, `__extendsfdf2`, `__fixsfsi`, `__fixunssfsi`, `__floatsisf`, `__floatunsisf`, `__fixsfdi`, `__fixunssfdi`, `__floatdisf`, `__floatundisf`, comparisons `__eqsf2`..`__unordsf2`.
  - Double precision (binary64): `__adddf3`, `__subdf3`, `__muldf3`, `__divdf3`, `__negdf2`, `__truncdfsf2`, `__fixdfsi`, `__fixunsdfsi`, `__floatsidf`, `__floatunsidf`, `__fixdfdi`, `__fixunsdfdi`, `__floatdidf`, `__floatundidf`, comparisons `__eqdf2`..`__unorddf2`.
  - IEEE-754 round-to-nearest-even semantics with guard, round, and sticky bits.
  - Restoring binary division (27-bit for single, 56-bit for double) completely avoids calls to 64-bit software integer division helpers (`__udivdi3`/`__umoddi3`).
  - Built into both `zero_rt_riscv32.a` and `zero_rt_baremetal_rv32.a`.
- **Verification**: 2,512 test vectors covering basic arithmetic, edge cases (subnormals, NaNs, infinities, zeroes), conversions, and comparisons; executed natively and under `qemu-riscv32-static`.

### AVX-512 Verification Gate & Min/Max/Permute (Sprint 277)

- **No execution oracle available**: QEMU TCG does not implement AVX-512 and the dev host lacks it. `make test-avx512` (`tools/verify/avx512_diff.py`) instead diffs the `objdump -d` of the raw EVEX object against the GNU-as assembly of the `-S` output (EVEX-bearing lines only), rejects `(bad)` decodes, and enforces a mnemonic coverage floor. On a host with real `avx512f` it also runs and diffs against `gcc-15 -O2`.
- **New ops** (`x86_emit_v512_int_binop`, mm=0F38, pp=66, W0): `vpminsd` 39, `vpmaxsd` 3D, `vpminud` 3B, `vpmaxud` 3F, `vpermd` 36, `vpermps` 16 (`vvvv`=index, `rm`=data for the permutes). Scalar fallback in `ast_lowering_builtins.c`: `zc_simd_emul_minmax32` (64-bit-extended `m=(a-b)&((a-b)>>63)`), `zc_simd_emul_permute32` (index `&15`). Pitfall: `OP_SEXT32_64`/`OP_ZEXT32_64` are 32-bit-target register-pair ops - never use them for x86-64/AArch64/RV64 emulation; use in-place `OP_SEXT32`/`OP_ZEXT32`.
- **Not yet implemented**: AVX-512VL / high vector registers (`x86_evex` never sets EVEX.X for rm>=16), CD, gather/scatter, compress/expand, `{1toN}`/`{sae}`.

