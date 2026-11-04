---
type: Architecture
title: Multi-ISA Backends (x86-64, AArch64, RISC-V) & Linker/JIT
description: Backend vtable interface (ZeroCCISABackend), JIT machine code emission, AOT assembly generation, parallel argument moves, and symbol relocation resolution.
resource: backend/isa-backends
tags: [backend, x86_64, aarch64, riscv, jit, aot, elf, relocations]
timestamp: 2026-11-04T16:35:33Z
generated: antigravity-agent
verified: engineering-lead
status: active
---

# Multi-ISA Backends (x86-64, AArch64, RISC-V) & Linker/JIT

ZeroCC abstracts target-specific code generation behind `ZeroCCISABackend` (`src/zero_cc/isa/isa_backend.h`), supporting three architectures:
- **x86-64**: `src/zero_cc/isa/x86_64.c` (JIT binary emission) & `src/zero_cc/isa/x86_64_asm.c` (AOT `.s` assembly emission)
- **AArch64**: `src/zero_cc/isa/aarch64.c` & `src/zero_cc/isa/aarch64_asm.c`
- **RISC-V (RV64GC)**: `src/zero_cc/isa/riscv.c` & `src/zero_cc/isa/riscv_asm.c`

## Lazy Backend Initialization

Backend structs (`x86_64_backend`, `aarch64_backend`, `riscv64_backend`) are non-`const` and lazily wired with their assembly emitter callbacks via `zero_cc_init_backends_once()` inside `zero_cc_get_backend()` and `zero_cc_get_native_backend()`.

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
   - **C11 `_Alignas(N>16)` realign mode** (Sprint 95 x86-64, Sprint 96 all ISAs): when the max local alignment N > 16 (power of two, ≤ 4096; no VLA/variadic/frame-address), the frame is rounded to N and SP itself is realigned at entry so slots are actually N-aligned. `LoweringState.frame_align` holds N; all four backends emit the realign prefix and use the omit-FP slot geometry (`frame_delta = frame`, base = SP):
     - x86-64: `push rbp; mov rbp, rsp; and $-N, %rsp; sub $frame, %rsp`; epilogue = plain `leave` (rbp kept at entry-SP-8).
     - AArch64: `stp x29,x30,[sp,#-16]!; mov x29,sp; mov x16,sp; and x16,x16,#-N; sub x16,x29,x16; sub sp,sp,x16; sub sp,sp,#frame` (SP cannot be the destination of a logical op → mask through x16/IP0); epilogue = plain FP-mode `mov sp,x29; ldp x29,x30,[sp],#16`.
     - RISC-V (64 & 32): `addi sp,sp,-16; sd/sw ra,8(sp)/4(sp); sd/sw s0,0(sp); mv s0,sp; andi sp,sp,-N (N<4096) or li t0,-N; and sp,sp,t0 (N=4096); sub sp,sp,#frame` (register-sub for frame > 2047); epilogue = plain leave (`addi sp,s0,0; lw/ld ra; lw/ld s0; addi sp,sp,16`). **RV32 note**: the ra save slot is at +4 (not +8) to match `riscv32_emit_leave`.
     - **AArch64 AND-mask encodings**: the realign mask MUST be the 64-bit logical-immediate form (`and x16, x16, #-N`, SF=0, e.g. N=32 → `0x927BEA10`). The 32-bit literal form (`#0xffffffe0`, SF=1) zero-extends into x16 and destroys SP's upper half — QEMU matches real hardware. The `aarch64.c` table carries a comment about this pitfall.
     - **RISC-V N=4096**: `(~(N-1)) & 0xFFF == 0` would emit `andi sp, sp, 0` (zeroes SP) — both MC (`riscv.c`) and ASM (`riscv_asm.c`) fall back to `li t0, -N; and sp, sp, t0` (t0 is free at function entry).
     - **RISC-V RV64 funct7 traps** (hit while porting): `sub` = f7 `0x20` (0b0100000), NOT f7 1 (that is `mul`); `and` = f3 7, f7 0. The `rv64_R(rd, rs1, rs2, f3, f7)` helper takes f7 last — passing the opcode `0x33` there silently produces an illegal instruction (SIGILL under QEMU).
   - **Large SP-relative displacements** (frames > 4KB, e.g. `_Alignas(4096)` locals):
     - AArch64 `aarch64_emit_lea` (MC) must keep large offsets in the ADD/SUB *immediate* form — SP is **not** a legal Rm of ADD (register) (register 31 there is XZR, so `add dst, dst, sp` silently becomes `add dst, dst, xzr` and the SP vanishes). Offsets > 4095 use the scaled-immediate form (`#imm, LSL #12`, bit 22), or a two-instruction immediate split; only non-SP bases may use mov+register-add. `emit_load`/`emit_store`'s large-offset fallback had the same latent bug (a pre-set Rm bit in the constant made it `add x16, sp, x18`) — fixed.
     - RISC-V S/I-type immediates are 12-bit (±2047): `riscv64_emit_load/store` (both RV64 and RV32 — the RV32 vtable now uses the ISA-aware RV64-named helpers) and the ASM-path load/store/param-save/CSR-restore all fall back to a scratch-register base (`li t0, off; add t0, base, t0; st/ld, 0(t0)`) when the displacement exceeds the immediate range.
     - **binutils 2.41 RISC-V `sym+offset` bug**: `lla dst, .Lrodata+off` (and `%pcrel_hi(sym+off)`/`%pcrel_lo(1b)`) compute the wrong address (LO12 reloc references a phantom `.L0`; verified off-by-one against a plain-label control). The `-S` RISC-V rodata pointer loads (`asm_emitter.c` OP_LEA_RIP / OP_FMOV_IMM) therefore emit `lla t0, .Lrodata` + `addi/add` with the offset instead of a symbol+offset reference. AArch64 `adrp x9, .Lrodata+off` is unaffected (verified).
   - **Scope**: the frame-pointer register (`%rbp` / `x29` / `s0`) is currently only *not set up*; it is not yet handed to the register allocator as a general-purpose register, so the win is prologue/epilogue cost, not register pressure.
   - Verified by the QEMU raw-emitter battery (59/59) and the extended `prologue_epilogue_verifier.py` (see below).
7. **Verification Tools**:
   - `python3 tools/prologue_epilogue_verifier.py` verifies selective CSR omission, frameless leaf emission (both pure leaf and leaf with promoted arguments), Red Zone usage, omit-frame-pointer shapes (FP-free prologue/epilogue with balanced SP adjustments and LR/ra save-restore on all four ISAs), and store-load symmetry for both FP-based (`[x29, #-N]` / `N(s0)` / `N(%rbp)`) and SP-based (`[sp, #N]` / `N(sp)` / `N(%rsp)`) CSR slots.
   - `python3 tools/extension_verifier.py` verifies GNU statement expressions, switch case ranges, designated initializer ranges, and `__builtin_offsetof`.

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
3. **`tools/vtable_check.py` (T3: VTable Conformance & Caller Auditor)**:
   - Audits `ZeroCCISABackend` definitions against `isa_backend.h` and scans all `BE(c)->` / `backend->` call sites across `src/`.
   - **Full VTable Parity (Sprint 89)**: All **221/221** backend slots in `ZeroCCISABackend` are now initialized across all 4 targets (`x86_64`, `aarch64`, `riscv64`, `riscv32`), with **0 missing slots** between `riscv64_backend` and `riscv32_backend` (`python3 tools/vtable_check.py --diff riscv64 riscv32`).
   - `riscv32_backend` is wired to `riscv64_init_asm_emission(&riscv32_backend)` in `zero_cc_init_backends_once()`, with word-sized `sw`/`lw` awareness (`BE(c)->isa == ISA_RISCV32`) in `riscv_asm.c`, `riscv.c`, and `asm_emitter.c`.

## Cross-ISA `OP_SYSCALL` (`__zerocc_syscall`, Sprint 89)

`__zerocc_syscall(nr, a1, a2, a3, a4, a5, a6)` lowers to `OP_SYSCALL` (`ir_emit_syscall`) and is implemented in both the raw machine-code emitters and GAS `.s` assembly emitters across all 4 target ISAs:

- **x86-64 (`x86_emit_syscall`, `x86_emit_syscall_asm`)**: Linux System V AMD64 ABI (`rax = nr`, `rdi, rsi, rdx, r10, r8, r9 = a1..a6`, `syscall`, result in `rax` / `VREG_0`). Preserves caller-saved GPRs (`rcx`, `r11`) clobbered by `syscall`.
- **AArch64 (`aarch64_emit_syscall`, `aarch64_emit_syscall_asm`)**: Linux AArch64 ABI (`x8 = nr`, `x0..x5 = a1..a6`, `svc #0`, result in `x0` / `VREG_0`). Saves/restores 13 allocatable GPRs on a 16-byte aligned frame (`112` bytes) and loads spilled temporaries via the pre-decrement `sp` frame.
- **RISC-V 64 & 32 (`riscv64_emit_syscall`, `riscv_emit_syscall_asm`)**: Linux RISC-V ABI (`a7 = nr`, `a0..a5 = a1..a6`, `ecall`, result in `a0` / `VREG_0`). Uses word-sized `sw`/`lw` (`4`-byte slots) on `ISA_RISCV32` and `sd`/`ld` (`8`-byte slots) on `ISA_RISCV64`.
- **Differential Verification (`test/isa_qemu/13_syscall.c`)**: Exercises `SYS_getpid`, `SYS_write`, and `SYS_brk` across all 4 ISAs under `make test-isa-qemu` (52/52 when the battery had 13 tests, 13/13 per ISA in both raw and `-S` assembly modes; the battery has since grown to 15 tests - 59/59 as of Sprint 100, `15_nested_tramp.c` skipped on riscv32).

## Trampoline Stubs (Sprint 93 Phase 3b; cross-ISA Sprint 97)

Real computed-goto trampolines for indirect calls through nested function pointers (`f = inner; f(x)`), on **x86-64, AArch64 and RISC-V64** (RISC-V32 still rejects capturing nested functions with a clean diagnostic):

- Each capturing nested function gets a static **context object** (`__zf<N>_<name>_ctx`, 32 bytes per capture slot) populated at address-taking time (`zc_lower_fn_value` in `ast_lowering_expr.c`):
  - **By-value capture**: copies the captured value's words into the context object.
  - **By-reference capture**: stores the pointer value (the enclosing slot's address) - via `zc_sym_addr`, never `zc_sym_load` (that dereferences and stores the pointee's VALUE; a deeper function then dereferences an int as a pointer and segfaults).
  - **Multi-level capture**: stores the pointer value of the intermediate capture (mirror param).
  - **>32 bytes**: stores the address of the context object itself.

- **Stub function** with the user-facing signature (no hidden context param): emitted right after the real nested function in the same text image (`x86_64_emit_tramp_stub` / `aarch64_emit_tramp_stub` / `riscv64_emit_tramp_stub`, shared unit model in `src/zero_cc/isa/tramp_stub.h`). The stub:
  1. Rebuilds the full ABI argument stream (8-byte units per class: integer register slots + overflow, FP register slots + overflow, >16B struct hidden pointer; the entry geometry uses **separate namespaces** - register slot 0..7 vs shared caller-stack unit index 0.. - which is why the unit model carries `ue_reg[]` and `ue_stk[]` separately).
  2. Loads context object values into the appropriate argument registers (hidden capture params first, then user args).
  3. Ensures 16-byte stack alignment with a **pre-push pad** (odd unit count adds 8 to the frame BEFORE any push - matching the OP_CALL lowering model; a post-push pad would shift the shared stack units away from `[entry_sp]` where the real function's prologue reads them).
  4. `call`s the real nested function; the real function's `ret` lands on the stub's epilogue (`leave; ret` / `ldp x29,x30,[sp],#16; ret` / `ld ra; ld s0; addi sp,sp,16; ret`), returning to the ORIGINAL caller.

  **Sprint 100 (VLA struct members)**: a by-value parameter or capture of a struct/union with variably-modified members occupies **2 GPR units** in the unit model (`tramp_unit_gpr_slots` in `tramp_stub.h` + the x86 local model): unit 0 = object pointer, unit 1 = total runtime size (the by-value ABI is defined in `abi-sysv-x86_64.md`). This is what makes `compile/nested-1.c` (`int nested (struct s x)` with `struct s { int a; char b[argc]; }`) PASS: the stub must route both units into the same argument registers the real function's prologue reads.

- **ISA stub-encoding invariants** (each bug below was found via a QEMU miscompile/segfault):
  - **x86-64**: `movaps xmm,mem` (load) = `0F 28 /r` but `movaps mem,xmm` (store) = `0F 29 /r` - the entry-save loop must use the STORE form; XMM entry-save offsets beyond disp8 (-144) need the disp32 `41`/`44`/`45` REX form; loading XMM from the frame uses `movdqu` (`F3 0F 6F`, 16B) / `movsd` (`F2 0F 10`, 8B) - `F2 0F 6E` is not a valid 16-byte load; entry stack args at `[rbp+16+8m]`; `call rel32`'s PC is the NEXT instruction (the +5 relative to the `call` itself).
  - **AArch64**: LDUR/STUR are the **immediate-offset family** (imm9 signed, two's-complement wrap = 512: STUR int/FP `0xF8000000`/`0xFC000000`, LDUR `0xF8400000`/`0xFC400000`), distinct from the 12-bit 8x-scaled LDR/STR unsigned-offset family (`0xF94`/`0xF90`/`0xFD4`/`0xFD0`) - pasting one family into the other silently loads the wrong register; offsets >255 use `sub x10, x29, #off` + plain `[x10]` access; `bl` is PC-relative with PC = the `bl`'s own address (no +4) and does not push LR (first stack arg at `[x29+16]`); 8-byte `str/ldr` push/pop (`stp/ldp` pairs were wrong for the stub's single-register geometry).
  - **RISC-V**: `jal` is PC-relative with PC = the `jal`'s own address (no +4); the stub must `addi sp,sp,-D` after `mv s0,sp` (without it the entry saves overwrite the caller's live stack); D > 2047 uses `li t0, D; sub sp,sp,t0`; R-type `sub` is funct7 0x20 (funct7 0x01 is `mul`) and `and` is funct3 7 / funct7 0.
  - **Direct calls to nested functions on RISC-V** (`riscv64_emit_call`): the early-patch path (baking `fn->text_offset - pos` into the `jal`) is only valid for already-emitted functions. Nested functions are emitted AFTER their enclosing function (deferred), so their `text_offset` is still 0 - early-patching bakes a jump to text offset 0 (the first function = the enclosing scope) and infinite-recurses until the stack overflows. Nested targets are routed through the `R_RISCV_JAL` relocation instead (the .o + system-linker path resolves it at final offsets); AArch64/x86 already use call relocations and are unaffected.

## Related Concepts
- Return to [Root Index](./index.md)
- See [System V AMD64 ABI & Struct Calling Conventions](./abi-sysv-x86_64.md)
- See [Two-Pass IR & Register Allocation](./ir-and-regalloc.md)
- See [Optimization Passes & Safety Invariants](./optimization-passes.md)
