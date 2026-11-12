---
type: Concept
title: Target-Side Sanitizer (zero_san) Design & Roadmap
description: Analysis and phased roadmap for an optional, parked target-side safety-check feature (zero_san) — a codegen-first UBSan-lite signed-overflow check, with bounds, leaks, and ASan/LSan scoped out and justified.
resource: sanitizer/roadmap
tags: [sanitizer, zero_san, codegen, ubsan, overflow, bounds, asan, lsan, leak, roadmap, design, safety]
timestamp: 2026-11-12T06:47:04Z
generated: antigravity-agent
verified: engineering-lead
status: active
---

# Target-Side Sanitizer (`zero_san`) — Design & Roadmap

> **Status: Phase 1 IMPLEMENTED (Sprint 116, x86-64, both emitters, `-fsanitize=undefined`); cross-ISA extension (aarch64 / riscv64 / riscv32 width-4) SHIPPED in Sprint 163, §4c; div-by-zero / rem-by-zero / shift-count checks (all 4 ISAs) SHIPPED in Sprint 166 follow-on, §4e.**
> Sections below preserve the traced invariants and cost analysis; **§4-as-built** records the
> shipped mechanism and the traps found while landing it. Phases 2–3 (bounds, leaks) remain
> parked; ASan/LSan remain out of scope per §8.

## 4-as-built. Phase 1 as shipped (Sprint 116)

- **Flag:** `-fsanitize=undefined` (GCC-compatible name; parsed in `driver_opts.h`, propagated to
  `ZeroCCCompiler.sanitize_overflow` in both the JIT and AOT paths of `main.c`). Only the
  signed-integer-overflow check is implemented under it (GCC's full UBSan is not claimed).
- **Emission:** `ast_lowering_binop.c` (`zc_maybe_emit_ovf_trap`), immediately after
  `ir_emit_add/sub/mul` for `TK_PLUS`/`TK_MINUS`/`TK_STAR`, x86-64 only (lowering guard +
  defensive guard in the shared raw-emitter loop + AOT-asm skip). **Check width = the usual-
  arithmetic-conversion result width** (wider operand wins; equal widths require both signed):
  `int + long` checks 64-bit, not 32-bit — checking at the narrower operand's width false-traps
  on valid code (caught in review before it shipped). Pointers/arrays excluded; widths the
  register model can't express (bit-fields, sub-byte, `__int128`) are skipped, not mis-checked.
- **Trap:** `ud2` (SIGILL), fully inline in **both** emitters — `ast_lowering_mc.c` (raw bytes,
  `jno +2; ud2` for width 8 via the CPU OF flag; sign-extension round-trip via scratch regs for
  width 4) and `asm_emitter.c` (text asm with self-contained `.Lovf_N` labels). This resolved
  §7 open decision 1 in a third way: no `__zerocsan_trap` builtin and no `zero_san.a` — the trap
  is a bare instruction, which is strictly more portable (JIT + host AOT + `--zerocc-link`)
  and costs one check site instead of a call. Report is op-level only (decision 4: no DWARF).
- **Emitter post-pass exclusions:** both emitters' generic width-extension + spill store-back
  post-passes skip `OP_OVF_TRAP` (its `width` is check metadata, not a value mark — with 8 the
  post-pass's `width & 7` mask yields 0 and the extend emitters' 16-bit default truncated the
  64-bit sum; the ALU op's own store-back already persisted the value). The extend emitters on
  all four ISAs were hardened so `width >= 8 || width == 0` is a no-op/move instead of falling
  through to a narrow path.
- **Pass interaction (verified, not assumed):** `opt_run_passes` runs on the IR after lowering,
  so the adjacency contract depends on pass conservatism — confirmed structurally (OP_OVF_TRAP
  is in none of the LICM/CSE/GVN pure-op whitelists; DCE shares the checked op's dest liveness)
  *and* empirically (`san_trap_loop.c` traps at -O0..-O3, JIT + AOT).
- **Gate:** `make test-san` (decision 3: yes — `tools/san_sweep.py` over `test/san/`: `san_ok_*`
  = valid code must exit 0 with the flag at -O0..-O3 in JIT + AOT *and* behave identically
  without it; `san_trap_*` = real overflow must SIGILL at every level; `san_ok_cross_smoke.c`
  compiled raw-AOT for aarch64/riscv64/riscv32 and run under qemu guards "no x86 check bytes
  leak into other ISAs"). c-torture + `make test-opt` with the flag OFF stay the conformance
  backstop; the sweep is the flag-ON soundness gate.
- **Pre-existing issues found while landing (separate follow-ups, not zero_san bugs):**
  1. RISC-V (both) miscompile of `int * long` mixed-width multiply: the sign-extension copy
     clobbers the long operand's register (computes `i * i`). **Fixed in Sprint 117** —
     the IR was correct all along; the allocator had no notion that an ABI argument is
     live in its slot register from function entry, so a slot could be handed to another
     interval and the `OP_PARAM_LOAD` materialization clobbered the live value. ABI
     parameter pre-coloring (`LiveInterval.fixed_reg`, see `ir-and-regalloc.md` →
     Register Allocation Rules) closes it on all 4 ISAs; pinned by `test/jit/wide_param_regs.c`
     and the `mul_il` case now restored in this file's cross-ISA smoke.
  2. ~~`(unsigned long)0xFFFFFFFFFFFFFFF0UL + (-100)` yields 140~~ — **retracted, not a bug**
     (the correct 2^64−116 is what zerocc computes, verified byte-discriminating against gcc;
     the original "repro" could only observe the low 8 bits, identical for both candidates).
  3. A 32-bit `mov %reg32,%reg32` **zero-extends** (writes clear bits 32–63) — the width-4
     "sign" post-extension must be `movslq`/MOVSXD, never `movl` (a c-torture 1-bit-bitfield
     test, `execute/pr49123.c`, regressed for one iteration when the post-pass's `width & 7`
     zero-extend-flag strip was mistaken for a bug and the mask removed).

### 4b-as-built. Phase 1 extended to `++`/`--` and compound assignment (Sprint 161)

Sprint 116 shipped the check for the *binary* `+ - *` only. Sprint 161 extends the **same**
`OP_OVF_TRAP` mechanism to the two other forms C spells for the identical arithmetic, so a
program that overflows via `x++`, `x--`, `x += y`, `x -= y`, or `x *= y` traps exactly when the
binary form would:

- **Shared width rule, one source of truth:** the UAC check-width logic is factored out of
  `zc_maybe_emit_ovf_trap` into `zc_ovf_uac_width(lw,lu,rw,ru)` (+ `zc_emit_ovf_trap`,
  `zc_node_is_pointer`) in `ast_lowering_binop.c`. Three entry points now call it:
  - `zc_maybe_emit_ovf_trap(c,arena,node,res)` — a **binary op OR a compound assignment**
    (both carry the lvalue in `node->left` and the RHS in `node->right`, so the UAC is
    identical). The compound form is emitted at every integer `+= -= *=` lowering site
    (`ast_lowering_assign.c`: plain local, global, pointer-deref, member — via
    `member_apply_compound`, which now takes the `node` — and array-element).
  - `zc_maybe_emit_ovf_trap_incdec(c,arena,node,res)` — the **`++`/`--`** form; the op is
    `lvalue + 1` (or `- 1`), the literal `1` is a signed `int`, so the check width is the
    lvalue's *promoted* width when it is signed. Emitted at every integer inc/dec site
    (`ast_lowering_unop.c`: local, global, and the non-ident lvalue path — array element,
    `*p`, `s.f`, `p->f`).
- **What is deliberately NOT checked (consistent with Sprint 116):** unsigned lvalues
  (defined wrap); **pointer** `++`/`--` and pointer compound (no signed-overflow semantics);
  bit-fields, sub-byte, and `__int128` (widths the register model can't express); the
  float-RHS compound path (the C math is floating-point — a later truncation to int is a
  different, unimplemented UBSan category); and a **decayed** array used in pointer
  arithmetic (e.g. `arr + 1` — reads as unsigned-8/pointer via `zero_cc_expr_int_info`, and
  pointer arithmetic has no signed-overflow semantics). An **array-element** lvalue is the
  opposite case and IS checked: `arr[i]`, `a[i][j]`, and `p[i]` all carry the *element*
  `TypeInfo`, so `zero_cc_expr_int_info` resolves them to the element's width + signedness
  and an element overflow traps (`san_trap_arr_elem` pins int/long/`a[i][j]`/`p[i]`).
  Narrow lvalues (`short`/`char`) promote to `int` and are checked at width 4, so a
  store-back wrap (e.g. `short s=32000; s+=100;`) is correctly *not* a trap.
- **Verified:** 9 new `test/san/` cases (`san_ok_incdec`, `san_ok_cassign`;
  `san_trap_{,long_}incdec`, `san_trap_cassign{,_sub}`, `san_trap_long_cassign{,_mul}`, and
  `san_trap_arr_elem`) trap/SIGILL and pass at
  `-O0..-O3` in JIT + AOT; `make test-san` green (0 FAIL). Soundness backstop unchanged — the
  flag is OFF in c-torture/test-opt, so those gates' output is byte-identical to pre-161.

### 4c. Sprint 163 — cross-ISA `OP_OVF_TRAP` (aarch64, riscv64, riscv32) — AS BUILT

Today the check is x86-64 only: the lowering guard (`c->isa != ISA_X86_64` at the two
`ast_lowering_binop.c` entry points) is the single gate, `OP_OVF_TRAP` is lowered only by the x86
MC emitter (`ast_lowering_mc.c`, CPU OF flag) and the AOT asm emitter (`asm_emitter.c`), and
`san_ok_cross_smoke.c` only proves valid code does not false-trap elsewhere. Original plan (branch
`sprint-163-san-cross-isa`; as-built notes follow it):

1. **Drop the x86-only guard** (lowering + defensive guard in the shared raw-emitter loop + AOT-asm skip).
2. **aarch64 + riscv64, compare-based (no OF flag), MC + AOT-asm:** add — result out of signed
   range relative to operands (`(a^r)&(b^r) < 0`, or `adds`/`b.vc` on aarch64 where the flags exist);
   sub — `(a^b)&(a^r) < 0`; mul — `smulh` vs sign of low half (aarch64/riscv64 `mulh`), or
   division bound for width 4 via widening to 64-bit and a sign-extension round-trip. Width 4 on
   64-bit ISAs: compute in 64-bit and compare against `sext32(res)` (mirror of the x86 width-4 path).
   Trap = `brk #0` (aarch64) / `ebreak` (riscv) -> SIGTRAP; the sweep must accept the per-ISA signal.
3. **riscv32:** width 4 first (native compare-based lowering). Width 8 is a lo/hi GPR pair — follow-up,
   reusing the GPR128 (lo,hi) machinery; until then width-8 sites on rv32 are *skipped, not mis-checked*
   (same rule as bit-fields/`__int128`).
4. **Gate:** extend the cross-ISA qemu smoke from "no false trap" to also assert real traps fire under
   `qemu-*-static` (new `san_trap_cross_*` cases: add/sub/mul/incdec/cassign at widths 4 and 8, run at
   -O0..-O3 on aarch64/riscv64/riscv32-w4). Adjacency/pass-interaction invariants of §4-as-built carry
   over unchanged (OP_OVF_TRAP is in no pure-op whitelist).
5. **Pitfalls to pin:** emitter post-pass width-mask exclusion (`width & 7`) on every new emitter;
   scratch-register clobber on spilled operands; sign-extension copy must not clobber the other operand.

**As built (Sprint 163):**

- **No x86-only gate remains.** `zc_emit_ovf_trap(c, arena, a, b, op, ...)` keeps the operand-less
  x86 form; non-x86 ISAs carry both operand temps plus `imm = ZC_OVF_OP_{ADD,SUB,MUL}`
  (`isa_backend.h`) in `OP_OVF_TRAP`. All compound-assign (24) and inc/dec (3) sites pass operands.
  New vtable hooks `emit_ovf_trap` / `emit_ovf_trap_asm` (NULL on x86-64, which stays inline in the
  shared emitters); `ast_lowering_mc.c` / `asm_emitter.c` dispatch to them for non-x86.
- **aarch64** (`aarch64.c`, `aarch64_asm.c`): width 4 = `sxtw`/`cmp`/`b.eq`/`brk #0`; width-8 add/sub =
  `eor/eor/and/asr/cbz/brk` (register-based `cbz` rather than flag-based `b.pl` — found empirically
  under qemu); width-8 mul = `smulh`/`asr`/`cmp`/`b.eq`. Self-contained `.Lovf_N` labels via
  `c->ir_next_label`. Trap = `brk #0` -> **SIGTRAP (rc 133)**.
- **riscv64 / riscv32** (`riscv64.c`, `riscv_asm.c`): RV64 width 4 = `slli/srai/beq/unimp`; RV32 width 4
  and RV64 width 8 = operand formula (`mulh`/`xor`/`and`/`srai`/`bge`/`unimp`). Trap = `unimp` ->
  **SIGILL (rc 132)**.
- **Pre-existing trap-encoding bugs fixed on the way:** the aarch64 `emit_trap` encoding is now the real
  `brk #0` (0xD4200000); the riscv `emit_trap` was `0x00002007` — a *float load* that SIGSEGV'd —
  now `0xC0001073` (`unimp`).
- **Known false-negative (safe direction, never false traps):** riscv32 **width 8**. See §4d.
- **Gate:** `tools/san_sweep.py` gained a cross-trap section: `san_trap_cross_int.c` (32-bit) on
  aarch64/riscv64/riscv32 and `san_trap_cross_wide.c` (64-bit) on aarch64/riscv64, each at -O0..-O3
  under `qemu-*-static`, asserting the per-ISA signal (`CROSS_TRAP_RC`; accepts shell `128+sig` or
  Popen `-sig`). `make test-san`: 213 ok / 0 FAIL (23 cross). riscv32 needs `ZC_RV32_MUSL` when run from
  a worktree without the in-tree musl-cross libs.

### 4d. Sprint 164 — the "riscv raw-path width-4" gap was an RV64 immediate-loader miscompile; rv32 width-8 is blocked on rv32 i64

- **riscv64 raw path, width-4 `++`/`--`/compound (FIXED):** not a sanitizer bug. `riscv64_emit_mov_imm_32`
  materialized 32-bit constants with `lui`+`addi`; for `hi == 0x80000` (values `0x7FFFF800..0x7FFFFFFF`,
  e.g. `INT_MAX`) `lui` yields `0xFFFFFFFF80000000` and the positive `addi` remainder never carries
  into bits 32-63, so `INT_MAX` loaded as `0xFFFFFFFF7FFFFFFF`. The 32-bit sign-extension round-trip then
  saw a fitting sum and (correctly, for that wrong value) did not trap. Fix: `addiw` (opcode 0x1B) after
  `lui` on RV64 (RV32 keeps `addi`). This was a **general RV64 miscompile** of any int constant in that
  range (the text path uses the assembler's `li`, which is why only the raw path was affected). Pinned by
  `test/isa_qemu/16_imm32_edges.c` (all 4 ISAs, vs musl reference) and `san_trap_cross_{incdec,cassign}.c`
  (all 3 cross ISAs, -O0..-O3).
- **riscv32 width-8 (still skipped, and correctly so):** zerocc has **no 64-bit integer lowering on
  riscv32**. `long long` is `sizeof 8` but is added/compared in a single 32-bit register (observed:
  `LLONG_MAX + 1` prints `286027642044416` vs gcc's `-9223372036854775808`; a 64-bit constant is
  truncated by `riscv64_emit_mov_imm`'s RV32 path). A width-8 overflow check on rv32 would therefore
  check a truncated value and be meaningless. The lowering skip (`ISA_RISCV32 && w == 8`) stays until
  rv32 i64 add/sub/mul/shift/compare/div and the two-register argument/return ABI exist (the GPR128
  lo/hi machinery is the natural model). Tracked in the development plan as its own item — it is a
  conformance gap far larger than the sanitizer.

### 4e. Sprint 166 follow-on — div-by-zero, rem-by-zero, shift-count checks (all 4 ISAs)

The v1 check covered signed `+ - *` (incl. compound/inc-dec) only. This follow-on extends the same
`OP_OVF_TRAP` mechanism to the other two UB classes that share the per-op trap design:
**division/remainder by zero** and **out-of-range shift counts** (C 6.5.8p5: count < 0 or ≥ the
shifted operand's bit width). Op codes `ZC_OVF_OP_{DIV_ZERO,REM_ZERO,SHIFT_COUNT}` = 3/4/5 in
`isa_backend.h`; lowering entry points `zc_maybe_emit_{div_zero,rem_zero,shift_count}_trap` in
`ast_lowering_binop.c` (same UAC width rule — signed only, widths 4/8 only; unsigned div/mod/shift
are defined and unchecked, as with unsigned arithmetic).

**Pre-op placement (the load-bearing decision).** The check is emitted **before** the guarded op
(`ir_emit_div/mod/shl/shr/sar`), not after — because on x86-64 `idiv` by zero raises **#DE (SIGFPE)
at the instruction itself**, so a post-check is unreachable. (aarch64 `sdiv`/riscv `div` by zero are
*defined* results, so a post-check would work there, but pre-op is uniform and strictly earlier.)
The checked temp (divisor/count) rides in `temp_dest` with `temp_src1 = TEMP_NONE` on **all** ISAs
(the x86 branch of `zc_emit_ovf_trap` now carries the op code in `.imm` instead of dropping it — the
original v1 x86 form predates these ops); the emitter tests `temp_dest`'s register. Liveness is safe
without extra use-marking: the temp's interval is [first def, last use] and its last real use is the
guarded op immediately after the trap, so the divisor/count is still in its register at the check
(a post-shift placement was tried and fails on x86 — after the shift the count temp is dead and its
register is reused, so the check read a stale value).

**Per-ISA emission (skip-when-ok, each guard = one branch + one trap):**
- **x86-64** (`asm_emitter.c` + `ast_lowering_mc.c`): div/rem = `test %r,%r; jnz skip; ud2`;
  shift = `cmp $0,%r; jge L; ud2; L: cmp $W,%r; jl L2; ud2; L2:` where **W = width*8 bits**
  (4/8-byte width → bound 32/64, *not* 4/8). The MC path encodes the compares as `0x83` (imm8),
  never `0x81` (imm32) — `0x81` with one immediate byte eats three instruction bytes.
- **aarch64** (`aarch64.c`/`aarch64_asm.c`): div/rem = `cbnz res,+4; brk #0`; shift =
  `cmp res,#0; b.ge +4; brk; cmp res,#W; b.lt +4; brk`. The `cmp Xn,#imm` encoding is
  **0xF100001F | imm<<10 | Rn<<5** (Rn in bits 9:5, Rd=XZR) — the tempting 0xF1000000|Rn is
  `subs Rn_field-as-Rd, XZR, #imm`, which zeros the checked register (found empirically: a
  false-trap + corrupted shift in the cross smoke).
- **riscv64/riscv32** (`riscv64.c` raw + `riscv_asm.c` text, RV32 reuses both): div/rem =
  `bne res,zero,+8; unimp`; shift = `li t0,W; bge res,zero,+8; unimp; blt res,t0,+8; unimp`
  (t0 = compiler scratch, never allocated). **Both new-op blocks run before the
  `width < 8 && is64` round-trip early-return** — that early-return is ADD/SUB/MUL-only and would
  otherwise swallow the width-4 div/shift checks on RV64 (checking "does the divisor fit in 32
  bits" instead of "is it zero").
- **riscv32 width 8:** still skipped at lowering (rv32 i64 gap, §4d) — 32-bit div/shift checks
  work natively.

**Invariants pinned by the new gate corpus** (8 new `test/san/` files, all in `make test-san`):
`san_ok_divshift.c` (non-zero divisors incl. −1, counts 0..30/0..62 — counts 4..30 in particular
false-trapped while the bound was the byte width), `san_trap_{divzero,remzero,shiftcount,
shiftcount_neg,shiftcount64}.c` (runtime zeros/counts via volatile loads so no pass can fold them
away; negative count and width-8 bound get their own cases), and cross-ISA
`san_trap_cross_{divzero,shift}.c` (32-bit, all 3 ISAs × -O0..-O3 × raw+text under qemu;
`san_ok_cross_smoke.c` gained 32-bit div/rem/shift cases so the cross smoke exercises the new
emitters' skip path). `tools/san_sweep.py`'s cross-trap list extended accordingly.

**Pre-existing bug found and fixed on the way (not a sanitizer defect):** the **raw** AArch64
div/mod emitters were hardcoded to `udiv` (0x9AC00800) — every *signed* division on raw-AOT
aarch64 was miscompiled (e.g. `−100 / −7` → 1 instead of 14); the text path already selected
`sdiv` via `c->lowering.div_is_unsigned`. `aarch64_emit_div/emit_mod` now select the encoding from
that flag (`sdiv` = 0x9AC00C00); the dead `emit_idiv` helper got the correct sdiv encoding too.
Surfaced by the new `div32(-100,-7)` smoke case. (The original cross smoke had no division, which
is why it was never caught.)

**Known pre-existing issue (documented, not fixed here):** `san_ok_cross_smoke.c`'s `f64loop` case
fails at **-O2/-O3 on all three cross ISAs** (returns 7; `f64loop(10) != 123450L`) — reproduced on
the pre-change file, so unrelated to this work. The sweep runs the ok-smoke at -O1 only (the
cross-trap files cover -O0..-O3), so the gate stays green; a separate cross-ISA -O2 loop bug to
track.

**Type-tag verifier (I2) extended for the new ops:** `ir.c` now prints `OVF_TRAP rN [ovf=K]` in
`-dump-ir`; `tools/verify/type_tag_verifier.py` I2 accepts `[ovf=3/4/5]` as a **pre-op** check —
it must sit immediately before its guarded DIV/MOD/SHL/SHR/SAR and check that op's src2, and the
`[ovf=5]` count tag may be a signed int *narrower* than the check width (int count on a long
shift). `[ovf=0/1/2]` keep the original post-ALU rule.

**Not yet started (roadmap items, unchanged):** `-fsanitize-trap` diagnostics (op-level site
reporting when a trap fires — needs the runtime shim §4-as-built deliberately avoided) and the
Phase 3 at-exit leak report in `zero_rt/heap.c` (§4.3).

ZeroCC today has **host-side** sanitizers only — `make asan` builds the *compiler* with
`-fsanitize=address,undefined`, which finds bugs in ZeroCC's own C, not in compiled programs.
It has **no target-side** instrumentation: `zerocc -fsanitize=address prog.c` is tolerated and
ignored, and the output contains no `__asan` symbols. This page plans the (optional)
target-side feature.

---

## 1. What `zero_san` is (and isn't)

- **It is codegen-first, not a runtime.** The trap/handler runtime is trivial; the value and
  the cost are in a **compiler instrumentation pass**. Do not frame `zero_san` as "a runtime".
- **Layout** (mirrors `zero_rt/`, see [Zero_RT](./zero-rt.md)):
  - `zero_san/` at repo root (sibling of `zero_rt/`) — the runtime artifact, if any.
  - `src/zero_cc/san/` (or woven into `src/zero_cc/ir/` / `src/zero_cc/isa/`) — the codegen pass.
  - Shared ABI header `zero_san/san.h`, analogous to `zero_rt/rt_os.h`.
- **Do not** create `src/zero_san/` for the runtime; the runtime is a root-level archive like
  `zero_rt.a`.

## 2. Runtime follows link mode

The target runtime follows the link mode (the same split as [Zero_RT](./zero-rt.md)):

| Mode | Target runtime | `zero_san` trap target |
|---|---|---|
| JIT `--run` | host libc (via `zero_cc_resolve_symbol` / `dlsym`) | host function via a resolver shim |
| Host AOT `-o` | host glibc (system linker) | a tiny `zero_san.a` **or** a zerocc builtin |
| Freestanding `--zerocc-link` | `zero_rt.a` | a zerocc builtin (cleanest, works everywhere) |

**A zerocc builtin** (`__zerocsan_trap`, lowered to a trap/abort) is the recommended trap for
v1: one lowering, works in JIT + host AOT + freestanding, no separate `.a` to link.

**Caveats:** the JIT resolver's `dlsym(RTLD_DEFAULT)` is ambient-dependent; `zero_rt` (and thus
the freestanding path) sits on a thinner test track than the host path.

## 3. The check spectrum — where the cost actually is

| Check class | Mechanism | Cost | Class |
|---|---|---|---|
| **Signed integer overflow** | per-op codegen (`OP_OVF_TRAP`) | **Low** | v1 |
| **Alignment** | per-access (size in opcode) | Low, **but non-conformant to trap on x86** | excluded from v1 |
| **Type-based bounds** | thread array-length to codegen, all backends | **High**, and *inherently partial* (array-lvalues only, not pointers) | later / parked |
| **Leaks** | allocator + at-exit (in `zero_rt/heap.c`) | Low (freestanding), **no codegen** | later, separate |
| **ASan / LSan-grade** | shadow memory + per-access ×3 + redzone allocator + DWARF | **Major** | **out of scope** |

Two structural facts drive the costs:
- **The per-access surface is the wall.** There are ~49 memory-access emission sites in the
  x86-64 backend alone; a per-access sanitizer touches all of them, ×4 for the four target ISAs
  (`x86_64`, `aarch64`, `riscv64`, `riscv32` — see [ISA Backends](./isa-backends.md)).
- **LSan-grade requires ASan's shadow memory** (pointer provenance for the reachability scan),
  so "full LSan" and "full ASan" are the *same* infrastructure. They are not independently cheap.
- **Leaks are a different class** (allocator + at-exit, not per-instruction codegen) — see §4.3.

## 4. v1 design: signed-integer-overflow (corrected scope)

**Scope correction (re-review):** v1 is **signed-integer-overflow only**. The original
"overflow + alignment" scope is *shrunk* because a **trapping** alignment check is
**non-conformant on x86-64** (unaligned access is legal there, just slow — which is exactly why
GCC keeps UBSan's `alignment` check *opt-in*). Alignment is dropped from v1.

**Mechanism:**
- New `OP_OVF_TRAP` IR op carrying the result temp + width (reuse the existing
  [`Instr.width`](./ir-and-regalloc.md) field *on the check op*). Emitted at lowering
  **immediately after** a signed narrow `add`/`sub`/`mul`.
- **The arithmetic op itself must NOT be width-marked** — the width-extension pass would
  clobber the overflow (truncate to N bits) before the check could observe the raw wide result.
- **Two emitters (correction):** the check must be lowered in **both** the JIT byte-emitter
  (`x86_emit_*`, raw `me_emit_u8` bytes) **and** the AOT asm-emitter (`x86_emit_*_asm`, driven
  by `asm_emitter.c`). The earlier "one shared lowering covers both" assumption was **wrong**;
  these are two paths that can drift.
- **Trap:** a zerocc builtin `__zerocsan_trap` (print + abort).
- **Reuses** the `zc_ovf_*` primitives already proven by `__builtin_{add,sub,mul}_overflow` in [`src/zero_cc/ir/ast_lowering_builtins.c`](../../src/zero_cc/ir/ast_lowering_builtins.c).
- **Flag:** opt-in (`-fsanitize=undefined` for GCC parity, or `-fsafety-checks`); **not**
  default-on for users.
- **Report:** op-level only (no file:line — ZeroCC emits **no DWARF** today).

### 4.1 Soundness invariants (traced and confirmed)

The check is **sound** (fires iff a real signed overflow) because, at the point of the op:
1. **Every integer ALU op is REX.W 64-bit** (`addq`/`subq`/`imulq`), so the full 64-bit
   pre-truncation sum is in the destination register.
2. **The clean-value invariant holds at every value origin** — local/global loads, `*p`
   deref, subscript, and call returns are all sign/zero-extended via
   `zero_cc_emit_int_extend`; **arithmetic results are deliberately NOT re-extended** (raw wide
   sum). So the operands at the op are clean sign-extended N-bit values, and the result is the
   raw wide sum → comparing it against the N-bit **signed** range fires **iff** a real signed
   overflow occurred. (Unsigned overflow is defined wrap → no trap.)
3. **Conformance:** signed integer overflow is undefined behavior (C 6.5p2), so trapping on it
   is conformant.

**The empirical backstop:** run the [c-torture gate](./c-torture-gate.md) and `make test-opt`
**with the flag ON**. Any spurious abort on a *valid* test = an unsound check. This is both the
soundness proof and a persistent test-harness config. (Exotic value producers — complex/vector
lanes, VLA-sized temps — were not individually traced, but a missed path would surface as a loud
conformance failure, not a silent miscompile.)

### 4.3 Leaks are a separate, cheaper, non-codegen workstream

Leaks do **not** fit `OP_OVF_TRAP` (they're about allocation *lifetime*, not a single
instruction). `zero_rt/core/heap.c` is a 180-line single-arena allocator (one
`malloc`/`calloc`/`free`/`realloc`) and `zero_rt/core/exit.c` has a working `atexit` list +
exit wrapper, so a **crude "still-allocated-at-exit" detector** is cheap: tag live blocks,
report them at exit, with a root/ignore set for intentional resident buffers. It is
**freestanding-only** (the host path uses host libc → use host LSan/Valgrind; short-lived
`--run` tests don't care about leaks) and needs **no codegen**. LSan-grade (reachability) is
ASan-class and stays out of scope.

## 5. Phased roadmap

| Phase | Scope | Cost | Value tag |
|---|---|---|---|
| **1 — v1** | Signed-int-overflow, x86-64, *both* emitters, opt-in, checks-on validation | Small-to-medium, low-risk (additive) | **Optional** — architecture de-risk + freestanding-debug niche |
| **2 — bounds** | Thread array-length to codegen, 3 backends, array-lvalue-only (partial) | Large, cross-cutting | Parked — only if v1 proves out *and* there's demand |
| **3 — leaks** | Crude at-exit report in `zero_rt/heap.c` + root/ignore set. No codegen | Low (runtime-only) | Later — once the freestanding path is a real deliverable |
| **ASan/LSan** | Shadow memory + per-access ×3 + redzone allocator + DWARF | Major, high-risk | **Out of scope** — re-enter only if the goal shifts to "production toolchain" |

## 6. Gating — when this actually starts

`zero_san` was originally parked behind three prerequisites, two of which are now **complete**:
1. ✅ **Conformance `REJECT` burn-down (Completed in Sprint 89):** `REJECT` is now **0** and `MISCOMPILE` is **0** across the entire 3,940-test `c-torture` corpus (**3,676 `PASS`** as of Sprint 109; see [c-torture Conformance Gate](./c-torture-gate.md) — since raised to **3,740 `PASS`** at Sprint 115l, still 0/0).
2. ✅ **Header-monolith modularization (Completed in Sprints 76–83):** `preprocessor.h` (`preprocessor.c` + `virtual_headers.h`), `definitions.h` (`decl_struct.c`, `decl_local.h`, `type_parser.h`), `compile_unit.h` (`compile_unit.c`), and `ast_lowering_expr.c` (`ast_lowering_{expr,call,builtins,assign,binop}.c`) are now split into dedicated translation units (see [Architecture](./architecture-pipeline.md)).
3. **Remaining priority before `zero_san`:** the `SKIP-TARGET:int128` item is effectively closed — the cross-ISA `__int128` gate (`make test-isa128`) is green (144/144) and the residual c-torture `SKIP-TARGET` entries are explicit DejaGnu target/extension skips. The remaining listed prerequisite is the "compile a real project" differential gate (a higher-value miscompile oracle than target-side sanitizers).

**Entry condition for Phase 1:** after `int128` and the real-project gate, *and* only if the team wants (a) the small, safe, conformant piece that de-risks the `zero_san` architecture, or (b) a freestanding-debug differentiator. **v1 is optional and niche** — worth doing as a bounded, conformant, architecture-de-risking step, *not* as a strategic priority.

> **Re-evaluated (promoted):** `zero_san` Phase 1 (signed-integer-overflow) is now **Current Priorities #1** in [`docs/ZEROCC_Development_Plan.md`](../../docs/ZEROCC_Development_Plan.md), started under clause (a) above (architecture de-risking) — the real-project differential gate remains open in parallel and is no longer a hard blocker. The companion canary-stack work (`-fstack-protector`) is Current Priorities #2 and follows this page’s *runtime-follows-link-mode* pattern (cookie/`__stack_chk_fail` in `zero_rt` for the freestanding path).

## 7. Open decisions

1. ~~**Trap:** zerocc builtin vs a `zero_san.a`.~~ **Resolved (Sprint 116): neither** — a bare
   `ud2` inline in both emitters (see §4-as-built): no symbol, no archive, works everywhere.
2. ~~**Flag name:** `-fsanitize=undefined` vs a custom `-fsafety-checks`.~~ **Resolved:
   `-fsanitize=undefined`** (GCC drop-in; only the overflow check is implemented under it).
3. ~~**Persistent checks-on test config.**~~ **Resolved: yes** — `make test-san`
   (`tools/san_sweep.py`, `test/san/`), the flag-ON soundness gate; c-torture + test-opt stay
   the flag-OFF conformance backstop.
4. ~~**Report:** op-level for v1, defer DWARF.~~ **Resolved: op-level** (the trap is a bare
   `ud2` — no diagnostic at all in v1; a reporting hook would need the runtime shim this design
   deliberately avoids).

## 8. Why ASan/LSan is out of scope (cost decomposition)

Full parity requires: **per-access codegen** (~49 memory-op sites in x86-64 ×3 ISAs, all
correctness-critical), **shadow memory** laid out per-ISA, a **redzone allocator**,
**stack-frame redzone** instrumentation, a **runtime** (`libasan` equivalent + libc
interceptors), and — for a *useful* report — a **DWARF emitter**, which **ZeroCC does not
currently emit at all**.

The decisive reason isn't line count; it's **risk × opportunity cost, specific to ZeroCC**:
a feature that rewrites *every memory op in three backends* is disproportionately likely to
*introduce* the exact failure mode this project exists to prevent (miscompiles), and the effort
earns more on conformance and multi-ISA correctness. Re-enter only if the goal shifts from
"correct multi-ISA research compiler" to "production toolchain for large codebases," where ASan
becomes table stakes.

---

## 9. Related

- [Two-Pass IR & Register Allocation](./ir-and-regalloc.md) — `Instr`, the `width` field, temp-id budget.
- [Multi-ISA Backends](./isa-backends.md) — the JIT/AOT dual emitters; per-ISA cost of per-access work.
- [SysV AMD64 ABI](./abi-sysv-x86_64.md) — the call/return conventions the clean-value invariant rides on.
- [Zero_RT](./zero-rt.md) — the runtime model `zero_san` mirrors; the freestanding leak-detection home.
- [c-torture Conformance Gate](./c-torture-gate.md) — the soundness proof (run with checks ON).
- [Build & Verification](./build-and-verification.md) — the verification gates; the checks-on harness config.
