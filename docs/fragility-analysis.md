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

### C6. Float locals broken in the AOT `-S` path (pre-existing, all ISAs) — **RESOLVED**

Float/double locals lowering to `OP_FLOAD`/`OP_FSTORE` (float stack access) are now implemented across all three ISA assembly backends (`x86_64_asm.c`, `aarch64_asm.c`, `riscv_asm.c`) via `emit_load_fp_asm` and `emit_store_fp_asm` vtable callbacks. Float call arguments in `x86_emit_call_asm`, `aarch64_emit_call_asm`, and `riscv_emit_call_asm` now also resolve their source register names from `fp_regs` rather than GPR tables. Verified with `-S` output roundtrip assembled and linked via `gas`/`gcc` with exact results.

*Closure verification (Sprint 25+) surfaced two adjacent, pre-existing `-S`-only gaps (the `-o` ELF path is unaffected): (1) the asm emitter never emits a `.data` section, so any program with a global fails to link from `-S` output (all ISAs); (2) `CVTSI2SD` (int→double) is unimplemented in all three asm emitters (`# UNKNOWN:`). **Both resolved in `b0975fc`** (Sprint 26 era) — `-S` output now emits `.data`/`.rodata` globals and `CVTSI2SD`/`CVTTSD2SI`, and roundtrips through `gas`+`gcc`. Remaining `-S` float gap found while re-running the float probes: float constants are stored as 8-byte doubles in `.rodata` but the asm emitter loads them with 4-byte `movss` (reads the low 4 bytes = 0.0), and float `!=` compares emit integer `cmpq` — repro `float a=2.0f+0.25f; if(a!=2.25f) return 1;` gives rc=1 via `-S` but rc=0 via `-o`. Tracked in `docs/zero-rt-plan.md` §9. **All resolved in Sprint 30** (see item 13 below).*

### C7. Compound assignment to struct members stored only the RHS — **RESOLVED (Sprint 27)**

`s.w += 5;`, `s.f += 0.5f;`, `s.d *= 2.0;` on single-segment members (local struct base, global struct base, local pointer base `p->m op= v`, general base expression) — the member-assignment lowering had compound handling only in the multi-segment chain path (added by C5); every single-member path stored the RHS directly, so `s.w += 5` stored 5 and `s.d *= 2.0` stored 2.0. Pre-existing (reproduces on the pre-C5 baseline).

- **Fix** (`ir/ast_lowering_expr.c`): new `member_apply_compound()` helper — width-correct load of the current value (byte/16/32/deref, or VLOADSS/VLOADSD for float members under the double-width FP model), integer op set (incl. `%`), float ops via FADD/FSUB/FMUL/FDIV with int→float RHS conversion — wired into all four single-segment paths. The shared tail's integer member store also became width-correct (it was an unconditional 64-bit `ir_emit_store`, which clobbers the adjacent member of a 1/2/4-byte field).
- **Verified**: d1–d8 (int/float/double members × local/pointer/global bases, adjacent-field survival) and c2/c3 pass JIT + AOT; 140/140, sret 6/6, C5 battery, c1-wrap, 3000-globals AOT all stay green.

### C8. Float function-call result clobbered by FP arithmetic in the same expression — **RESOLVED (Sprint 28)**

`if(use2(2.0f) != 1.0f+1.25f)` computed the call result in xmm0/F0, then the RHS lowering corrupted it — wrong result in both JIT and AOT. Pre-existing (reproduces on the pre-C5 baseline). The failure had **three stacked root causes**, all fixed:

1. **Untracked FP return**: the float/double call result was referenced directly as the physical return register (`VREG_F0`) with no tracked temp, so the LRA never saw the value live across subsequent instructions. **Fix** (`ir/ast_lowering_expr.c`): materialize the result into a fresh temp via `OP_FMOV` immediately after the call.
2. **XMM intervals ignored `spans_call`**: `alloc_reg()` respected the `spans_call` flag only for GPRs; an FP interval spanning a call was happily assigned a caller-saved XMM, which the call clobbers. **Fix** (`ir/regalloc.h`): `REG_CLASS_XMM` (and the steal guards) now force a spill when `spans_call` is set — all FP/SIMD registers are caller-saved on every supported ABI.
3. **Register steal from an already-spilled interval**: the spill-selection "steal" branch handed the current interval the stolen interval's `assigned_reg` — which is `-1` when the stolen interval was already spilled, producing a **dangling temp** (no register, no spill slot) that silently read the fallback register. **Fix**: skip `is_spilled` intervals in both spill-selection loops, never allow an FP/VEC interval spanning a call to steal (no callee-saved FP registers exist), and fall back to spilling the current interval when the stolen register is `-1`.

Supporting fixes in the same change:
- **Class-aware spill reload/store** (`ir/ast_lowering_mc.c`, `ir/asm_emitter.c`): spilled FP temps reload/store through `emit_load_fp`/`emit_store_fp` (movsd) into new `fp_scratch_reg1/2` (x86 `xmm15/xmm14`, aarch64 `d15/d14`, riscv `f15/f14` — outside the 0–7 XMM pool, shared constants `REGALLOC_FP_SCRATCH1/2`), instead of the GPR 64-bit scratch path.
- **XMM↔VEC pool aliasing** (pre-existing, exposed by #1): VEC-class temporaries (scalar float locals use 16-byte `OP_VLOAD`) allocated from a separate `vec_free[]` array that aliases the same physical xmm registers as the XMM pool — an overlapping VEC load could clobber an XMM temp (e.g. a float local load clobbering a call result). **Fix**: on shared-hardware ISAs (x86/AArch64; RISC-V v-registers are separate hardware) the two pools cross-mark indices 0–7 and VEC allocation skips the FP scratch indices.

- **Verified** (all JIT `-O0`/`-O1` + AOT `-o`): e10 (original repro), e11 (call result in `+ * - / !=` with a later FP op), e12 (int `f()+f()` regression), e13 (nested FP calls `id(use2(1.0f))`), e14 (three-call sum), e15 (FP value live across an intervening int call), e16/e17 (isolated two-call / FDIV), e22 (XMM/VEC aliasing repro — failed on baseline), plus the C5/d-batteries, 140/140 `make test`, sret 6/6, c1-wrap, 500-globals. The `-S` path contains the C8 mechanism correctly (materialize → spill store → call → reload); e11 still fails **only** via `-S`+gcc because of the separate, pre-existing `-S` float-emit gaps (see C6 note: 2-operand `addsd`, integer `cmpq` for float compares, `movss` on 8-byte rodata doubles).

### C9. Inline int→float cast dropped in call-argument position — **RESOLVED (Sprint 29)**

`use2((float)k)` with `k` an int local: the `CVTSI2SD` conversion was **absent from the IR** — the raw integer bits were stored to the float-argument slot and read back as a double bit pattern (int `3` → `3.0e-323` → `use2` returns `0.25`). The cast worked when it landed in its own statement (`float f=(float)k; use2(f);`) because the float-local *assignment* path supplies its own conversion; the call-argument path trusts `last_expr_is_float`, which the cast never set. Pre-existing (reproduces on the pre-C5 baseline).

- **Fix** (`ir/ast_lowering_expr.c`, `NODE_CAST`): the cast handler now detects a float/double *target* type. Int/pointer→float/double emits `CVTSI2SD` into a fresh temp and sets `last_expr_is_float = 1`; float→float/double is a no-op under the 64-bit double FP model. The second half of the bug (also fixed): a float *source* cast previously fell into the float→int branch unconditionally, so `(double)a`/`(float)a` truncated the value through `cvttsd2si` — a float target now short-circuits before that path.
- **Verified**: e25 (original repro), e25p (value probe now prints `r=3.250`), e36 (float→double / float→float casts), e37 (15-assertion cast battery: int/unsigned/short/long→float/double as args, in expressions, in assignments, nested casts, float→int/unsigned/short truncation) — all rc=0 JIT `-O0`/`-O1` + AOT, gcc cross-check rc=0.

### C10. Float local as call argument inside a loop — **RESOLVED (Sprint 29, was C11 in disguise)**

`for(...) { float fk=(float)k; last = id(fk); }` produced wrong values. The symptom pointed at the float-argument path, but the actual cause was **C11** (the auto-vectorizer rewriting loops that contain calls) — the e30/e31 repros all contain a call in the loop body. Once C11 is fixed, e30/e31 (and every other call-in-loop repro) pass JIT + AOT unchanged.

### C11. Auto-vectorizer vectorized loops containing function calls — **RESOLVED (Sprint 29, pre-existing)**

Discovered while re-testing the C9 loop cases: `for(k...) { float t = use2(1.0f); s += t; }` (and every `s += use2((float)k)` variant) ran the body only **once per 4 loop steps** — the loop counter was being incremented by 4. Root cause in `opt_vectorize.c`: `analyze_loop_for_vectorization_full` counted loads/stores/arith and accepted the loop (its comment promised "no function calls" but never checked), and `vectorization_transform` then (a) rewrote the induction step `k += 1` → `k += vec_width` (4) and (b) converted the scalar float ops to 128-bit SIMD (`VADDPS`), while the `OP_CALL` in the body stayed scalar and ran 1/4 as often. Result: dropped loop iterations + packed-lane arithmetic on 16-byte loads of scalar floats. Fails identically on the pre-C5 baseline.

- **Fix** (`opt/opt_vectorize.c`): the analyzer now scans the loop body for `OP_CALL` / `OP_CALL_INDIRECT` / `OP_RET` and rejects such loops (`has_call` guard in the vectorization criteria). Legitimate element-wise array loops (`a[i] = b[i] + c[i]`) still vectorize and run correctly (e48: VADDPS still emitted, rc=0 JIT+AOT+gcc).
- **Verified**: e21/e23/e24/e29/e30/e31/e38/e39/e40/e41/e43/e46 (call-in-loop battery, incl. value probes) pass JIT + AOT; e42/e44/e45 (no-call loops) and e47/e48 (vectorizer sanity) pass; `make test` 140/140 unchanged.
- **Known residual (documented, not fixed)**: mid-body *conditional* branches in an otherwise array-shaped loop are still not rejected (the loop-condition `JE` is part of every for-loop's backedge span, so a blanket jump guard would disable the vectorizer entirely); and the transform's increment rewrite matches *any* `OP_ADD` with a constant-1 operand, which could corrupt a non-counter `x + 1` in a vectorized loop. Both are rarer patterns; tracked here for the vectorizer-hardening item.

---

## HIGH

### H1. ~200 nested expression depth → parser stack overflow (segfault/hang at compile time) — **RESOLVED**

**Refined (Sprint 23)**: the original ~115 boundary was C2 in disguise (temp exhaustion at ~118-deep `+0` chains). The *real* H1 was a **parser stack overflow**: recursive-descent expression parsing used ~18 stack frames per nesting level, so the default 8 MB main-thread stack exhausted near depth 200 (backtrace: 3600+ frames of `parse_primary_ast` → `parse_assignment_ast` cycles; non-deterministic segfault/hang depending where the overflow lands).
- **Resolution**:
  1. Raised process stack limit (`RLIMIT_STACK`) automatically in `main()` to 64 MB when permitted by OS limits.
  2. Added an explicit recursion guard in `parse_primary_ast()` that tracks `c->expr_depth` and cleanly diagnoses `expression nesting depth exceeds parser limit` if depth exceeds 150 before thread stack exhaustion can crash.
  3. Probes verify `expr-depth 150` passes JIT & AOT, and `151` cleanly errors (`COMPILE-ERR(rc=1)`) instead of crashing/hanging.

### H2. Diagnostics do not stop compilation — **RESOLVED**

Errors such as `undefined local variable`, `failed to insert symbol`, `use of undeclared identifier`, or syntax errors in declarations and statements were previously printed, but compilation could continue and produce an invalid binary or execute partial JIT code.

**Resolution**:
1. **Immediate Error Propagation**: Whenever semantic analysis fails in `zero_cc_compile_function()`, `c->had_error` is immediately asserted (`c->had_error = 1`), preventing subsequent function compilation or code emission from continuing with broken state.
2. **JIT Execution Gate**: `zero_cc_execute_jit()` now explicitly guards `if (!c || c->had_error || c->text_section.length == 0) return -1;`, guaranteeing that no partially generated code is executed when diagnostics have fired.
3. **Fatal Syntax Errors**: Top-level declaration parser now reports diagnostics (`diag_report_error_at_token()`) and halts on unconsumed file-scope tokens, syntax errors in `extern`, `_Thread_local`, or global variable declarations, and `void` typed variables.
4. **Statement Synchronization Diagnostic**: Unparseable statements inside compound blocks now log an error diagnostic before synchronization, ensuring silent syntax errors cannot pass undetected.
5. **Lexer Error Propagation**: Tokenization errors now immediately set `c->had_error = 1` and record a diagnostic error.
6. **No Duplicate Diagnostics**: Removed duplicate diagnostic printing between `zero_cc_compile_function()` and `compile_unit.h` end-of-TU diagnostic sync.
7. **Complete Verification**: 140/140 tests pass in `make test`, SRET ABI verifier 6/6 pass, and invalid syntax/semantics reliably fail compilation with exit code 1 in both AOT and JIT modes.

---

## MEDIUM (silent limits, no diagnostics)

| # | Limit | Behavior past limit | Probe |
|---|-------|--------------------|-------|
| M1 | identifiers ≤ **127** chars | clean fatal diagnostic: `identifier exceeds maximum length of 127 characters` | **RESOLVED**: error diagnosed, no truncation |
| M2 | globals ≤ **256** | **RESOLVED (Sprint 27)**: global table is heap-allocated and grows (init 64, doubles); verified 3000 globals JIT+AOT with data-reloc inits | `globals` 3000 OK both modes |
| M3 | struct members ≤ **64** | 65th silently dropped (bounds-checked array, no diagnostic) | **FIXED (Sprint 31)** — `ZeroCCType.members` → heap table, grows on demand; OOM = clean fatal diagnostic |
| M4 | IR dump shows `imm` (int16) not `imm64` | large immediates displayed truncated (e.g. `2000000000` shown as `-27648`); **emission is correct** (uses `imm64`) — misleading for debugging only | `p11` IR dump |
| M5 | locals ≤ **~3000 per scope** (was C4) | clean fatal `failed to insert symbol` diagnostic, no binary (scope TPHT raised 64→256 blocks in Sprint 27; was ~758) | `locals` 1000 OK / ~3000 clean COMPILE-ERR |

---

## LOW (documented / accepted)

- **WSL2 GPF**: any 128-bit SIMD read from a `PROT_READ|PROT_EXEC` page faults on kernel 6.18.54 WSL2 (see `docs/okf/scalar-float-x86.md`). 32/64-bit reads fine.
- **`-S` path `.data` & `CVTSI2SD`**: resolved (Sprint 25); `.data` sections, relocations, and float/int conversion opcodes now emitted across all ISAs.
- Computed-size VLA, flexible-array `offsetof` (C99 audit leftovers).
- `>>` lowers to logical shift (implementation-defined for signed).

---

## Comparison vs gcc (Sprint 25 measurement)

The remaining ZeroCC limits (M1–M5, H1) are *clean-failing fixed-structure caps*; gcc's data structures for the same dimensions are dynamic and effectively unbounded. Measured (host gcc 13.3.0, `-O0`, same probes):

| Dimension | gcc | ZeroCC | Gap class |
|---|---|---|---|
| Locals per function | **200,000 compile & run rc=0** (frame only 4KB - unused `=0` locals are never allocated; symbol tables grow on demand) | ~3000 (Sprint 26: scope TPHT 256+247 blocks × 8 slots; clean diagnostic beyond) | M5 - capability, not correctness |
| Switch cases | unbounded (jump tables) | ~32k cap (16-bit labels, O(n) dispatch) - 1500 verified | capability (no practical C code approaches this) |
| Identifier length | unbounded (dynamic strings) | 127 (fixed `char name[128]`) — clean fatal diagnostic since Sprint 25 | M1 - **RESOLVED** (Sprint 25) |
| Globals per TU | unbounded | dynamic (verified 3000) | M2 - **RESOLVED** (Sprint 26) |
| Struct members | unbounded | 64 (silent drop) | M3 - **RESOLVED (Sprint 31)** |
| Nested-expression parse depth | very deep (gcc's recursive descent survives far more than ~200 on an 8MB stack) | ~200 (parser stack overflow, H1) | H1 - fix = bigger stack or iterative descent |

Key distinction after Sprints 22–29: every former **C-tier** correctness bug was a cap or liveness bug that *silently miscompiled*; the remaining limits now *fail clean* (diagnostic, no binary) except M3 which still drops silently (top candidate if more conformance headroom is wanted). **All C-tier items C1–C11 and both H-tier items (H1/H2) are RESOLVED** (C9/C11 fixed and C10 reclassified in Sprint 29).

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
5. **C5** ~~multi-segment member chain type resolution~~ — **Fixed (Sprint 25)**.
6. **C6** ~~implement `OP_FLOAD/OP_FSTORE` in the three asm emitters~~ — **Fixed (Sprint 25)**. (Residual `-S` gaps: float const width / float compare / 2-operand `addsd` — tracked in `docs/zero-rt-plan.md` §9.)
7. **H2** ~~make diagnostics fatal by default~~ — **Fixed (Sprint 25)**.
8. **C7** ~~member compound assignment stored only the RHS~~ — **Fixed (Sprint 27)**.
9. **C8** ~~FP call result clobbered by same-expression FP arithmetic~~ — **Fixed (Sprint 28)**: FMOV materialization of FP call returns + XMM `spans_call` forced spill + no-steal-from-spilled + class-aware FP spill + XMM↔VEC pool cross-marking.
10. **C9** ~~inline int→float cast dropped in call-argument position~~ — **Fixed (Sprint 29)**: `NODE_CAST` now emits `CVTSI2SD` for int/pointer→float/double targets (and no longer truncates float→float casts through `cvttsd2si`).
11. **C10** ~~float local as call argument inside a loop~~ — **Fixed (Sprint 29)**: was C11 in disguise (repros contained a call in the loop body).
12. **C11** ~~auto-vectorizer vectorized loops containing calls~~ — **Fixed (Sprint 29)**: `has_call` guard (OP_CALL/OP_CALL_INDIRECT/OP_RET) in the vectorization criteria; pre-existing, loop counter widened to `k += 4` while the scalar call-body ran 1/4 as often.
13. ~~**`-S` float emit completeness**~~ — **Fixed (Sprint 30)**: 3-operand `addsd`/`subsd`/`mulsd`/`divsd` aliasing, `ucomisd`/`fcmp` compares (`emit_fp_cmp_asm`), `movsd` for 8-byte rodata doubles, `VLOADSS` widen / `VSTORESS` narrow via `xmm15`, `OP_CMP_ZERO` (`testq`). Also surfaced two MC-path bugs: **C12** (`ucomisd` leaves SF/OF clear → signed setcc wrong; now `CC_FLT_*` → `setb/setbe/seta/setae`) and **C13** (REX emitted before the `0x66`/`F2` legacy prefix in `fp_cmp`/`vstoress`/`fp_neg`). `-S` float parity verified: 84/84 battery, a5/e11 rc=0.
8. **M1** ~~error (not truncate) on identifiers >127~~ — **Fixed**: emit clean fatal diagnostic on identifiers exceeding 127 chars.
9. **H1** ~~raise parser stack or iterate the expression recursion~~ — **Fixed**: raised stack to 64MB and added expression depth guard (clean error at 151+).
