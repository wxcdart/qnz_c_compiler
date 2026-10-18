# ZeroCC Fragility Analysis

Date: 2026-07-09 · Method: static pattern scan + boundary bisecting with 20+ generated probes, each cross-checked against host `gcc` (JIT `--run` and AOT `-o` modes).

Legend: **CRIT** = silently wrong code or compiler crash on valid C · **HIGH** = compiler crash/hang on valid C · **MED** = silent limit with no diagnostic · **LOW** = documented constraint.

---

## CRITICAL

### C1. 32-bit `int` overflow does not wrap (64-bit residue leaks into `int` values)

`int a=2000000000; int b=a+a; return b==-294967296?0:1;` → gcc 0, zerocc 1.

Integer arithmetic is performed in 64-bit registers and 32-bit `int` results are never truncated. Reloading a local `int` from its stack slot also lacks sign-extension (unsigned reloads zero-extend, which is why the unsigned variant passes). Every `int` computation whose true value exceeds the 31-bit range is silently wrong: comparisons, shifts, stores, return values.

- Probe: `p12a` (fails), `p12b` (unsigned, passes), `p12c` (in-range, passes).
- Fix direction: truncate to 32 bits when storing/reloading 32-bit signed int locals (sign-extend on load), or emit 32-bit ALU ops for `int`-typed values.

### C2. Temp-id exhaustion at 255 → silent miscompilation — **RESOLVED (C2a + C2b, Sprints 23–24)**

`Instr` temp fields were `uint8_t` with `0xFF` = "no temp". `next_temp_id` is an unbounded `int` that truncated on store: temp id 255 aliased the 0xFF sentinel (LRA skips it, `regalloc_get_reg_for_temp` returns the default register), and 256+ wrapped into 0–15, colliding with the real VREG_0–15 (return/arg registers).

- **Diagnosis (Sprint 23)**: *silent miscompilation*, not a compiler crash — the compiler finished (even `-S` succeeded) and the **generated program** segfaulted (AOT) or infinite-looped (JIT). A bare `-dump-ir` also "hangs" because `compile_jit` always calls `zero_cc_execute_jit`. The LRA was **not** the problem: nested-call results are only live across one outer call (~2–4 temps simultaneously live).
- **C2a (Sprint 23, `ast_lowering.c`)**: diagnostic guard. **C2b (Sprint 24)**: temps widened to 16-bit end-to-end (`Instr` fields, `IRCallArgs.temps`, `TEMP_NONE` = 0xFFFF sentinel at 140+ sites, 115 truncating casts, LRA 256-cap removed, optimizer tables widened/guarded, dual 0xFF base-register markers in FLOAD/FSTORE/LEA moved to TEMP_NONE, legacy 8-bit OP_CALL imm arg-packing removed in favor of the always-present side table).
- **Result**: 117 and 190 nested calls compile and run correctly in both modes; 400+-temp shallow programs work; the guard now bounds at the 16-bit edge (65534) as a safety net. The new ceiling for nested *expressions* is H1 (parser stack, ~depth 200).

### C3. Switch with >128 cases silently drops the rest — **RESOLVED (Sprint 24)**

`ast_lowering_stmt.c` dispatch loop: `while (s != AST_NULL && n_cases < 128)` — cases 129+ never got labels (unreachable, no diagnostic).

- The 119/120 "switch" boundary measured in Sprint 22 was C2 in disguise (~2.5 temps per case; 100 cases reach temp 255). After C2a the ~95-case temp error masked it further.
- **Fixed**: cap removed - dispatch is O(n) CMP/JNE, and the per-function temp guard bounds the case count (~2 temps/case → ~32k, under the 16-bit label space). Verified: 129/500/1000/1500-case switches pass JIT+AOT (128 OK / 129 WRONG before). (2000-case probe trips an unrelated "non-void function should return a value" diagnostic - probe artifact, separate issue.)

### C4. Many locals in one function → hang/crash — **RECLASSIFIED (Sprint 24): was C2 in disguise + clean 758-cap**

Original report: 230 `int` locals OK, 232 → hang, 400+ → segfault. **Refined**: each `int l_i=0;` consumes a temp, so 232–400 locals hit the C2 temp-id bug (255 sentinel / truncation) - the "hang" was the miscompiled *executed* code, and the segfault the same UB elsewhere.

- After C2b: 231/300/400/700 locals all pass. True boundary re-measured: **757 OK / 758** → the scope's `zero_flatten_tpht` capacity (64 primary + 61 overflow blocks × 8 slots), which already emits a clean `error: failed to insert symbol 'l757' into scope` (rc=1, no binary). No silent miscompilation, no hang - acceptable behavior at an extreme limit; raising it means growing the tpht (M-tier, not C).

### C5. Member chains through pointer members fail to resolve — **RESOLVED**

`b.p->v` (local struct), `b->p->v` (param), and deeper multi-segment `->`/`.` chains (including nested pointers, floats, and compound assignments like `o.p->f += 0.5f`) now lower correctly via `lower_member_chain_addr`. Pointer dereference store width is also type-sized (1/2/4/8 bytes) to avoid adjacent struct field clobbering. All 21 checks in `/tmp/frag/c5/c5_all.c` pass in both AOT (`-o`) and JIT (`--run`).

### C6. Float locals broken in the AOT `-S` path (pre-existing, all ISAs) — **OPEN**

Float/double locals lower to `OP_FLOAD`/`OP_FSTORE` (float stack access). The three ISA **asm** emitters (`x86_64_asm.c`, `aarch64_asm.c`, `riscv_asm.c`) never implemented them: the `-S` output contains literal `# UNKNOWN: FLOAD/FSTORE` comment lines and the computed value is garbage. Pre-existing on the pre-C2b baseline (byte-identical `-S`); the JIT and AOT-ELF (`-o`) paths implement the ops via the MC backend and work correctly. Discovered during C2b `-S` roundtrip verification (which also uncovered the extend-width bug in `asm_emitter.c`, now fixed).

---

## HIGH

### H1. ~200 nested expression depth → parser stack overflow (segfault/hang at compile time)

**Refined (Sprint 23)**: the original ~115 boundary was C2 in disguise (temp exhaustion at ~118-deep `+0` chains). The *real* H1 is a **parser stack overflow**: recursive-descent expression parsing uses ~18 stack frames per nesting level, so the default 8 MB main-thread stack exhausts near depth 200 (backtrace: 3600+ frames of `parse_primary_ast` → `parse_assignment_ast` cycles; non-deterministic segfault/hang depending where the overflow lands). Verified: with `ulimit -s 65536`, depth-200 parses fine and reaches the C2a temp guard. Pre-existing; independent of C2. gcc handles 120 (and far more) trivially.

### H2. Diagnostics do not stop compilation

Errors such as `undefined local variable`, `failed to insert symbol`, `use of undeclared identifier` are printed, but compilation **continues and produces a binary** (often with wrong semantics, rc=0). Broken builds pass silently.

---

## MEDIUM (silent limits, no diagnostics)

| # | Limit | Behavior past limit | Probe |
|---|-------|--------------------|-------|
| M1 | identifiers ≤ **127** chars | name truncated → **collision**, silently wrong code (error diagnostics fire on the split token) | `id.c`: 127 OK, 128 fails |
| M2 | globals ≤ **256** | 257th silently dropped (`add_global_var` returns -1, ignored) | `p3` 260 globals "passes" (only checked first 13) |
| M3 | struct members ≤ **64** | 65th silently dropped (bounds-checked array, no diagnostic) | `p4` |
| M4 | IR dump shows `imm` (int16) not `imm64` | large immediates displayed truncated (e.g. `2000000000` shown as `-27648`); **emission is correct** (uses `imm64`) — misleading for debugging only | `p11` IR dump |
| M5 | locals ≤ **~758 per scope** (was C4) | clean fatal `failed to insert symbol` diagnostic, no binary (reclassified from C in Sprint 24 - the original hang was C2 in disguise) | `locals` 757 OK / 758 COMPILE-ERR |

---

## LOW (documented / accepted)

- **WSL2 GPF**: any 128-bit SIMD read from a `PROT_READ|PROT_EXEC` page faults on kernel 6.18.54 WSL2 (see `docs/okf/scalar-float-x86.md`). 32/64-bit reads fine.
- **`-S` path** lacks `.data` section emission (AOT `-o` fine).
- Computed-size VLA, flexible-array `offsetof` (C99 audit leftovers).
- `>>` lowers to logical shift (implementation-defined for signed).

---

## Comparison vs gcc (Sprint 25 measurement)

The remaining ZeroCC limits (M1–M5, H1) are *clean-failing fixed-structure caps*; gcc's data structures for the same dimensions are dynamic and effectively unbounded. Measured (host gcc 13.3.0, `-O0`, same probes):

| Dimension | gcc | ZeroCC | Gap class |
|---|---|---|---|
| Locals per function | **200,000 compile & run rc=0** (frame only 4KB - unused `=0` locals are never allocated; symbol tables grow on demand) | 757 OK / 758 clean diagnostic (scope TPHT: 64+61 blocks × 8 slots) | M5 - capability, not correctness |
| Switch cases | unbounded (jump tables) | ~32k cap (16-bit labels, O(n) dispatch) - 1500 verified | capability (no practical C code approaches this) |
| Identifier length | unbounded (dynamic strings) | 127 (fixed `char name[128]`, silent truncation) | M1 - the one remaining *silent* cap; should become an error |
| Globals per TU | unbounded | 256 (silent drop) | M2 |
| Struct members | unbounded | 64 (silent drop) | M3 |
| Nested-expression parse depth | very deep (gcc's recursive descent survives far more than ~200 on an 8MB stack) | ~200 (parser stack overflow, H1) | H1 - fix = bigger stack or iterative descent |

Key distinction after Sprints 22–24: every former **C-tier** bug was a cap that *silently miscompiled*; every remaining limit now *fails clean* (diagnostic, no binary) except M1/M2/M3 which still truncate/drop silently — those are the top candidates if more conformance headroom is wanted. C5 (pointer-member chains) and C6 (float locals in `-S` asm) are the open C-tier items.

---

## Robustness positives (verified during this analysis)

- Global integer constants up to `INT_MAX` are exact (JIT + AOT).
- Struct-member width narrowing (1/2/4-byte) works for local and global bases.
- `members[64]` is bounds-checked — no OOB write from oversized structs.
- Long string literals (600B), 5000-element VLA, 1000-deep runtime recursion, `long long` INT64 boundaries, char/short sign boundaries — all correct.
- Reloc/label tables have generous caps (32K); LRA arrays grow dynamically.
- `make test` 140/140 and the C99 probe battery (30/30) still pass.

## Suggested fix order

1. **C1** ~~int32 wrap/extension~~ — **Fixed (Sprint 22)**. AOT-asm extend-width variant fixed (Sprint 24).
2. **C2** ~~temp-id bound~~ — **Fixed (Sprint 23 guard + Sprint 24 16-bit temp ids)**.
3. **C3** ~~switch case cap~~ — **Fixed (Sprint 24)**.
4. **C4** ~~~231-local hang~~ — **Reclassified (Sprint 24)**: C2 in disguise + clean 758 tpht cap; no action needed.
5. **C5** multi-segment member chain type resolution.
6. **C6** implement `OP_FLOAD/OP_FSTORE` in the three asm emitters (float locals in `-S` output).
7. **H2** make diagnostics fatal by default.
8. **M1** error (not truncate) on identifiers >127.
9. **H1** raise parser stack or iterate the expression recursion.
