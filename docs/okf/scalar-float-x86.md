---
type: Architecture
title: Scalar Float Pipeline (x86-64) & WSL2 RX-Page 128-Byte Read Limitation
description: ZeroCC's scalar FP model (all float/double values live in 64-bit double form), the VLOADSS/SD and VSTORESS/SD emitters (movss/movsd + widen/narrow), the XMM-vs-VEC register-class rule for scalar V loads, and the WSL2 general-protection-fault limitation for 128-bit memory reads from RX pages.
resource: backend/scalar-float-x86
tags: [backend, x86_64, floating-point, xmm, wsl2, register-allocation, movss, movsd]
timestamp: 2026-10-18T10:55:02Z
generated: antigravity-agent
verified: engineering-lead
status: active
---

# Scalar Float Pipeline (x86-64) & WSL2 RX-Page 128-Byte Read Limitation

## 1. The double-width scalar FP model

ZeroCC keeps **every** scalar float value (both `float` and `double`) in 64-bit
double form in XMM registers. The entire scalar FP instruction set
(`x86_emit_fp_add/sub/mul/div`, `x86_emit_fp_cmp` = `ucomisd`,
`x86_emit_fp_cvtsi` = `cvttsd2si`, `x86_emit_fp_mov` = `movsd`) operates on the
full 64 bits. Consequences:

- A 32-bit float load must **widen** into double form, otherwise the upper 32
  bits of the XMM register are stale and every subsequent double-width op reads
  garbage (this silently corrupted `(int)float_global` results).
- A 32-bit float store must **narrow** (double → single) before storing, because
  the register holds a double and the low 32 bits of a double are its mantissa.
- Float globals are therefore stored as 4-byte IEEE-754 `float` patterns in
  `.data` (convert `double → float` first when emitting initializer bytes — the
  low 32 bits of a `double` bit pattern are often zero and are NOT the float
  pattern).

## 2. Scalar V-load/V-store emitters (`src/zero_cc/isa/x86_64.c`)

`OP_VLOADSS/OP_VLOADSD/OP_VSTORESS/OP_VSTORESD` (scalar float access through an
address register, e.g. global struct float members, `*fp = 1.5f`) use dedicated
vtable entries — `emit_vloadss / emit_vloadsd / emit_vstoress / emit_vstoredsd`
in `ZeroCCISABackend`:

| op | sequence |
|----|----------|
| `x86_emit_vloadss` | `movss xmm,[m]` (F3 0F 10) + `cvtss2sd xmm,xmm` (F3 0F 5A) widen |
| `x86_emit_vloadsd` | `movsd xmm,[m]` (F2 0F 10) |
| `x86_emit_vstoress` | `movdqa` to xmm7 scratch, `cvttsd2ss` (F2 0F 5A), `movss [m],xmm7` (F3 0F 11) |
| `x86_emit_vstoredsd` | `movsd [m],xmm` (F2 0F 11) |

`VSTORESS` narrows through the **xmm7 scratch** (the same convention used by
`x86_emit_fp_sub/fp_div`) so the source double temp is not clobbered.
`emit_vload/emit_vstore` (128-bit `movups`) remain **exclusively** for
`OP_VLOAD/OP_VSTORE` (vector data, always in RW pages).

Encoding pitfalls fixed: `movss` is `F3 0F 10/11` (NOT `0F 5A` — that is
`cvtss2sd`, and unprefixed `0F 5A` is `cvtps2pd`).

## 3. Register class: scalar V-loads are XMM, not VEC

`ir_opcode_is_float()` (ir.h) classifies `OP_VLOADSS/OP_VLOADSD` destinations as
**REG_CLASS_XMM**; `ir_opcode_is_simd()` no longer lists them. The linear-scan
allocator manages XMM (xmm0-7 pool) and VEC (xmm0-15 pool) as separate pools
that alias the same physical registers — classing a scalar float load as VEC
let it alias an XMM temp on the same physical register (silent value
corruption). Scalar float ops (`FMUL`, `CVTSI2SD`, …) all use the XMM pool, so
their operands must share the class.

## 4. WSL2 limitation: 128-bit memory reads from RX pages raise a GPF

On WSL2 kernel `6.18.54.1-microsoft-standard-WSL2` (reproduced with a minimal
standalone program), **any SSE memory instruction with a 16-byte operand
(`movdqa`, `movapd`, `movups`, `andpd`, `orpd`, `xorpd`) reading from a
`PROT_READ|PROT_EXEC` page raises a general protection fault** (GPF, error:0),
while 32/64-bit reads from the same page work fine. ZeroCC's JIT maps
`[text][rodata]` into an RX code page (W^X partitioning, see
`jit/execution.h`), so:

- **Never emit 128-bit memory reads that resolve into the RX code page.**
- Float negation therefore uses `(0.0 - x)` (`FMOV_IMM` of a 0.0 double +
  `FSUB`) in `NODE_UNOP`/`TK_MINUS` instead of a sign-mask `xorpd [m128]`
  (the former `x86_emit_fp_neg` still exists in the vtable but is unused from
  the IR; its encoding was also fixed: `xorpd` is `0F 57`, REX.R for the
  destination, ModRM reg = dst[0:2]).
- 128-bit `movups` vector loads/stores remain safe **only** while their address
  points at RW pages (stack/data), which holds for all `OP_VLOAD/OP_VSTORE`
  uses (local arrays/structs).

## 5. Related lowering rules (ast_lowering_expr.c)

- **Cast grammar**: a cast's operand is a `cast-expression` (unary level) —
  `parse_unary_ast`, not `parse_assignment_ast`. `(int)x == 1` must group as
  `((int)x) == 1`.
- **Mixed float/int binary ops**: exactly-one-float `+ - * /` converts the int
  side with `CVTSI2SD` and takes the float path.
- **Global struct member rvalue**: the 64-bit `DEREF` result must fall through
  to the NODE_MEMBER width-narrowing (sign/zero extend for 1/2/4-byte members)
  — a packed layout like `struct {int a; float f;}` leaves the upper bytes of
  the load non-zero, so early-returns leak neighbor bytes into the value.
- **Global float/double compound assignment** (`g += 0.5f`): uses
  `VLOADSS/SD` + `FADD/FSUB/FMUL/FDIV` + `VSTORESS/SD`; integer bit-pattern
  arithmetic on the IEEE-754 value is wrong.

## 6. AOT Assembly Path (`-S`) Float Stack Load/Store & Call Args (C6)

In the AOT assembly emitter path (`src/zero_cc/ir/asm_emitter.c`), scalar float stack accesses (`OP_FLOAD` and `OP_FSTORE`) are handled via target-specific vtable hooks `emit_load_fp_asm` and `emit_store_fp_asm`:
- **x86-64** (`x86_64_asm.c`): `movsd offset(%base), %xmm` and `movsd %xmm, offset(%base)`.
- **AArch64** (`aarch64_asm.c`): `ldr dN, [base, #offset]` and `str dN, [base, #offset]` (with `#offset < -256` or large offset expansion via `x9`).
- **RISC-V** (`riscv_asm.c`): `fld fa, offset(base)` and `fsd fa, offset(base)`.
- **Function call argument register lookup**: In `x86_emit_call_asm`, `aarch64_emit_call_asm`, and `riscv_emit_call_asm`, float arguments (`is_fp[i] == 1`) query `BE(c)->regs->fp_regs` rather than the general-purpose register array (`get_hw_reg_name`), ensuring valid FP register operands are emitted.

## Related Concepts

- [Multi-ISA Backends & Linker/JIT](./isa-backends.md) — vtable, W^X JIT layout, `OP_MOV_GLOBAL`
- [IR & Register Allocation](./ir-and-regalloc.md) — opcode set, linear-scan pools
- [Global Data Relocs](./global-data-relocs.md) — data-page initializers (float patterns)
