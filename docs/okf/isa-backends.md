---
type: Architecture
title: Multi-ISA Backends (x86-64, AArch64, RISC-V) & Linker/JIT
description: Backend vtable interface (ZeroCCISABackend), JIT machine code emission, AOT assembly generation, parallel argument moves, and symbol relocation resolution.
resource: backend/isa-backends
tags: [backend, x86_64, aarch64, riscv, jit, aot, elf, relocations]
timestamp: 2026-10-20T22:06:45Z
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

### Cross-target output (aarch64 / riscv) goes through the text emitter (Sprint 41)

The **raw binary** emitters for aarch64 (`aarch64.c`) and riscv (`riscv.c`) are incomplete parallel backends (aarch64's has wrong encodings *and* register-allocation bugs: it loads uninitialized registers and clobbers values early). Rather than repair ~90 hand-coded raw encodings with no test harness, **`main.c` routes cross-target `-o` (executable) and `-c` (object) through the complete text emitter** (`aarch64_asm.c` / `riscv_asm.c`) + the **system cross toolchain**:

- **`-o`** (`compile_and_link`): emit text to a temp `.s`, then `aarch64-linux-gnu-gcc -mabi=lp64 -march=armv8-a -static -o out tmp.s -lc` (assembles + links). x86-64 still emits a native binary `.o` and links with `gcc -no-pie`.
- **`-c`** (`compile_object_cross`): emit text to a temp `.s`, then `aarch64-linux-gnu-as tmp.s -o out.o`. x86-64 still writes a binary `.o` via `elf_writer`.
- `-static` is required because the target dynamic loader isn't installed (the binary runs under `qemu-*-static`); the aarch64 link previously failed with an invalid `-mabi=lp64d` (now `-mabi=lp64`).

The raw emitters are only still reachable via the **JIT** path (`--run`) for those ISAs. Verified: aarch64 `-o` + `qemu-aarch64-static` matches `gcc -O2` for common C (see CHANGELOG Sprint 41). A **shared** (all-target) logical-AND (`&&`) lowering bug remains (e.g. `while(s[n]&&n<4)` ignores `n<4`) — separate, not aarch64-specific.

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

## Related Concepts
- Return to [Root Index](./index.md)
- See [System V AMD64 ABI & Struct Calling Conventions](./abi-sysv-x86_64.md)
- See [Two-Pass IR & Register Allocation](./ir-and-regalloc.md)
- See [Optimization Passes & Safety Invariants](./optimization-passes.md)
