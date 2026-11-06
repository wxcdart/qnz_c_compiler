---
type: Concept
title: Target-Side Sanitizer (zero_san) Design & Roadmap
description: Analysis and phased roadmap for an optional, parked target-side safety-check feature (zero_san) — a codegen-first UBSan-lite signed-overflow check, with bounds, leaks, and ASan/LSan scoped out and justified.
resource: sanitizer/roadmap
tags: [sanitizer, zero_san, codegen, ubsan, overflow, bounds, asan, lsan, leak, roadmap, design, safety]
timestamp: 2026-11-06T20:22:39Z
generated: antigravity-agent
verified: engineering-lead
status: active
---

# Target-Side Sanitizer (`zero_san`) — Design & Roadmap

> **Status: Phase 1 IMPLEMENTED (Sprint 116, x86-64, both emitters, `-fsanitize=undefined`).**
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
