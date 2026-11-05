---
type: Concept
title: Target-Side Sanitizer (zero_san) Design & Roadmap
description: Analysis and phased roadmap for an optional, parked target-side safety-check feature (zero_san) — a codegen-first UBSan-lite signed-overflow check, with bounds, leaks, and ASan/LSan scoped out and justified.
resource: sanitizer/roadmap
tags: [sanitizer, zero_san, codegen, ubsan, overflow, bounds, asan, lsan, leak, roadmap, design, safety]
timestamp: 2026-11-05T02:19:30Z
generated: antigravity-agent
verified: engineering-lead
status: active
---

# Target-Side Sanitizer (`zero_san`) — Design & Roadmap

> **Status: analysis / planned, PARKED.** This is a design-and-roadmap artifact, not live
> architecture. **No code exists yet.** It is deliberately *not* on the critical path (see
> [§6 Gating](#6-gating-when-this-actually-starts)). Written to preserve the traced
> invariants and cost analysis so the decision can be revisited without re-deriving them.

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
1. ✅ **Conformance `REJECT` burn-down (Completed in Sprint 89):** `REJECT` is now **0** and `MISCOMPILE` is **0** across the entire 3,940-test `c-torture` corpus (**3,676 `PASS`** as of Sprint 109; see [c-torture Conformance Gate](./c-torture-gate.md)).
2. ✅ **Header-monolith modularization (Completed in Sprints 76–83):** `preprocessor.h` (`preprocessor.c` + `virtual_headers.h`), `definitions.h` (`decl_struct.c`, `decl_local.h`, `type_parser.h`), `compile_unit.h` (`compile_unit.c`), and `ast_lowering_expr.c` (`ast_lowering_{expr,call,builtins,assign,binop}.c`) are now split into dedicated translation units (see [Architecture](./architecture-pipeline.md)).
3. **Remaining priority before `zero_san`:** closing out the `SKIP-TARGET:int128` bucket (Sprint 109) and the "compile a real project" differential gate (a higher-value miscompile oracle than target-side sanitizers).

**Entry condition for Phase 1:** after `int128` and the real-project gate, *and* only if the team wants (a) the small, safe, conformant piece that de-risks the `zero_san` architecture, or (b) a freestanding-debug differentiator. **v1 is optional and niche** — worth doing as a bounded, conformant, architecture-de-risking step, *not* as a strategic priority.

## 7. Open decisions

1. **Trap:** zerocc builtin (current lean — one lowering, works everywhere) vs a `zero_san.a`.
2. **Flag name:** `-fsanitize=undefined` (GCC drop-in) vs a custom `-fsafety-checks`.
3. **Persistent checks-on test config:** add to the harness (lean *yes* — it is the ongoing
   soundness guarantee; see [Build & Verification](./build-and-verification.md)).
4. **Report:** accept op-level for v1, defer DWARF (lean *yes* — don't scope-creep into a
   DWARF emitter).

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
