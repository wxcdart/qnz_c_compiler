---
type: Concept
title: GCC c-torture Conformance Gate
description: Differential conformance gate against the vendored GCC gcc.c-torture corpus (~3,940 tests), sweeping execute tests across -O0..-O3 in JIT and AOT and compile tests at -O0/-O2 with a ratcheting baseline.
resource: workflows/c-torture-gate
tags: [testing, verification, conformance, c-torture, gcc, differential, baseline]
timestamp: 2026-10-31T20:53:55Z
generated: antigravity-agent
verified: engineering-lead
status: active
---

# GCC `c-torture` Conformance Gate (`make test-torture`)

The GCC `gcc.c-torture` test suite is vendored at [`third_party/vendor/c-torture/`](../../third_party/vendor/c-torture/) (from `gcc-mirror` commit `442eb35f`, GPL-2.0-or-later, test corpus only — never linked into compiler or runtime binaries) and driven by [`tools/torture_run.py`](../../tools/torture_run.py) against a ratcheting baseline at [`test/torture/baseline.json`](../../test/torture/baseline.json).

---

## 1. Corpus & Scope Rules

The vendored corpus contains **3,940** active test files across two trees (excluding `compat/` and `unsorted/` multi-file harness tests):
- `compile/` (2,016 `.c` files): compile-only tests (`-c` at `-O0` and `-O2`).
- `execute/` (1,924 `.c` files, including `execute/builtins/` and `execute/ieee/`): compile-and-run tests swept across **8 configurations** (`-O0`, `-O1`, `-O2`, `-O3` × `{JIT (--run), AOT (-o)}`) and diffed on `(exit_code, stdout)` against a cached `gcc -O1 -w -lm` reference binary.

### Pre-Skip Directives (`parse_directives`)
Because `gcc.c-torture` mixes standard C89/C99/C11 tests with GCC-internal target extensions, `tools/torture_run.py` scans DejaGnu (`dg-*`) directives in the first 60 lines of each file and pre-skips:
- **`SKIP-TARGET:<feature>`**: `dg-require-effective-target` for GNU/target extensions outside ZeroCC's target contract (`untyped_assembly`, `label_values`, `indirect_jumps`, `trampolines`, `return_address`, `int128`, `named_sections`, `nonlocal_goto`, `unwrapped`, `arm`, `freestanding`, `alloca`, `bitint`, `dfp`, `fixed_point`, `tls_native`, `vector`, `vxworks_kernel`).
- **`SKIP-BOGUS`**: negative diagnostic tests (`dg-bogus`).
- **`SKIP-REFSEM`**: tests requiring reference-changing floating-point semantics (`-mfpmath=`, `-ffast-math`, `-frounding-math`, `-fsignaling-nans`, `-fexcess-precision=`).
- **`SKIP-PTR`**: tests printing raw `%p` pointer addresses (ASLR-nondeterministic across runs).

### No Pre-Filter for C89/GNU Extensions — Triage on `REJECT` Only
An early `gcc -std=c11 -Wpedantic` pre-filter was rejected because GCC's pedantic mode flags many valid constructs and over-excluded 1,082 files. Instead:
1. Every non-skipped test is compiled with a permissive `gcc -O1 -w` reference (`NO-REF` if host GCC itself rejects the file on x86-64).
2. ZeroCC runs on every test that passes the reference build.
3. Only when ZeroCC **rejects** a file (`REJECT`) does `_triage_reject` run `gcc -std=c11 -pedantic-errors -fsyntax-only` and `gcc -std=c99 -pedantic-errors -fsyntax-only`:
   - If GCC `-pedantic-errors` also rejects the file in both C99 and C11 modes, it is classified as **`SKIP-C89`** (K&R definitions, implicit `int`, obsolete GNU extensions) rather than a ZeroCC conformance failure.
   - If the file has a `dg-error` directive, it is classified as **`EXPECTED-REJECT`**.
   - Otherwise it remains **`REJECT` (`[clean -std=c11]`)** — a true C99/C11 frontend/semantic gap.

---

## 2. Classification Buckets & Baseline (Sprint 68)

Recorded in [`test/torture/baseline.json`](../../test/torture/baseline.json) across all **3,940** tests:

| Bucket | Count | Meaning |
| :--- | ---: | :--- |
| **`PASS`** | **2,865** | Every tested `(opt_level, mode)` configuration succeeded and matched GCC's `(rc, stdout)` (+76 since Sprint 66: 56 REJECT→PASS + 20 complex/latent-MISCOMPILE→PASS) |
| **`REJECT`** | **151** | ZeroCC failed to compile a valid C99/C11 test (`[clean -std=c11]`) (was 298) — the ongoing burn-down target (dominant: `expected identifier in declaration` ~76, `unexpected token 'X' in statement`, `incompatible types in assignment`, `expected identifier in extern declaration`, `unexpected token in struct/union definition`) |
| **`SKIP-C89`** | **571** | ZeroCC rejected a C89/GNU-extension test that `gcc -std=c99/-c11 -pedantic-errors` also rejects |
| **`MISCOMPILE`** | **0** | **Zero MISCOMPILEs in the entire corpus.** Sprint 68 fixed the last one (`execute/20071120-1.c` at `-O2/-O3`): the `NODE_MEMBER` rvalue lowering assumed a frame-local struct base when the base identifier resolved to neither a local nor a global, so `vec_->num` inside an **inlined** function read `[fp+0]` (an unrelated, never-written frame slot) instead of `[vec_+0]` — the inlined `VEC_..._last` assert then fired/segfaulted. The unresolvable-base path now lowers the base expression (inliner parameter substitution) and loads through the real pointer. |
| **`REF-BUG`** | **2** | GCC reference binary itself crashes (differential comparison impossible; not a ZeroCC failure) |
| **`NO-REF`** | **186** | Host `gcc -O1 -w` rejected the file (32-bit/target-specific tests without explicit `dg-require-effective-target`) |
| **`SKIP-TARGET:*`** | **142** | Explicit DejaGnu target/extension skips (`untyped_assembly`: 40, `label_values`: 34, `indirect_jumps`: 32, `trampolines`: 16, `int128`: 7, `return_address`: 7, `named_sections`: 3, `nonlocal_goto`: 2, `arm`: 1) |
| **`TIMEOUT`** | **10** | Compile/run beyond the 5 s budget — load-sensitive on the shared box (the count ratchets with machine load; re-run `--only-class TIMEOUT` under low load before treating as a real regression) |
| **`SKIP-REFSEM`** | **5** | Special FP flags (`-ffast-math`, `-mfpmath=`) |
| **`EXPECTED-REJECT`** | **4** | Has `dg-error` directive and was rejected |
| **`SKIP-BOGUS` / `SKIP-PTR`** | **2** | 1 `dg-bogus`, 1 `%p` stdout test |

### Ratcheting Invariants
When `tools/torture_run.py` runs against `test/torture/baseline.json`:
1. **`MISCOMPILE` count may never increase** (`cur_mc <= base_mc`).
2. **`REJECT` count may never increase** (`cur_rj <= base_rj`).
3. **`PASS` count may never decrease** (`cur_pass >= base_pass`).
4. **No per-test regression**: any test recorded as `PASS` in `baseline.json` that becomes `MISCOMPILE`, `REJECT`, or `TIMEOUT` fails the gate immediately, even if aggregate counts improved elsewhere.

---

## 3. Running the Gate

```bash
# Full sweep (~4 minutes at --parallel 16), gated against test/torture/baseline.json
make test-torture

# Quick dev sweep (-O1 AOT only on execute/, -O0 on compile/)
python3 tools/torture_run.py --quick

# Filter by substring, limit count, baseline class, or burn-down chunk (never overwrites baseline.json)
python3 tools/torture_run.py --only execute/pr --limit 100
python3 tools/torture_run.py --only pr109386,mzero4,fp-cmp-7  # comma-separated pattern list
python3 tools/torture_run.py --chunk m2                       # batch-verify the 16 partial MISCOMPILE tests
python3 tools/torture_run.py --only-class TIMEOUT             # re-run by baseline class (e.g. TIMEOUT)

# M1 hang triage (TIMEOUT burn-down): exact repro commands per failing config,
# artifact retention, timeout override, and automated hang capture
python3 tools/torture_run.py --only "execute/930529-1" --repro
python3 tools/torture_run.py --keep                  # retain objects/binaries in /tmp/torture_work
python3 tools/torture_run.py --timeout 30            # override run/build timeout (sec)
python3 tools/torture_run.py --hang-debug "execute/930529-1"        # capture a hang under gdb
python3 tools/torture_run.py --hang-debug T --hang-config O2-jit --hang-all

# Re-record baseline after fixing MISCOMPILE / REJECT / TIMEOUT bugs
python3 tools/torture_run.py --update-baseline
```

### M1 Hang Triage (`--hang-debug`)

`--hang-debug TEST` (standalone mode; no sweep, no baseline update) reproduces a test's
hanging configurations and captures the hang point. Per config it runs in two phases:
(1) a plain run confirms the hang (and reports the exact `zerocc` invocation), then
(2) a re-run under `gdb` — with gdb as the inferior's **parent**, so tracing works even
under `yama/ptrace_scope=1` — interrupts the hung process (SIGINT, like terminal
Ctrl-C) and prints `bt` + source line + `x/4i $pc` disasm. Config order is the
baseline's `TIMEOUT` configs for that test (override with repeatable
`--hang-config CFG`); it stops after the first hang unless `--hang-all`. AOT binaries
are kept under `/tmp/torture_work/hangdebug/<tid>/` for manual `gdb`/`objdump`.
For JIT-mode hangs the frames are frameless (JIT image) but the pc disasm shows the
loop; for AOT binaries the kept `bin_O<n>` is the debuggable artifact.

First findings (2026-10-27, before any M1 fixes):
`compile/pr94238.c` hangs in `zero_cc_stmt_enum` (`src/zero_cc/parser/definitions.h:1180`
— the enum-constant loop, confirming the "large enum chains" hypothesis); and
`execute/20011126-2.c`'s "optimizer hang" is actually an **optimizer-produced runtime
infinite loop** — the `-O2` build succeeds, and the generated `test()` body spins on a
`je`-backed loop (miscompile, not a compile-time hang).

---

## 4. Bugs Uncovered & Fixed in Sprint 57

Bringing up `tools/torture_run.py` exposed and fixed five compiler bugs (reducing `execute/` `MISCOMPILE` from **252 → 218** and eliminating parser hangs):

1. **Struct/Union `enum` Bitfields & Non-Literal Bitfield Widths** (`src/zero_cc/parser/definitions.h`, `ast_expr.h`, `compile_unit.h`):
   - `enum Lisp_Type type: 3L;` inside a `struct`/`union` (`compile/20000224-1.c`) previously hung the parser because the `TK_STRUCT | TK_UNION | TK_ENUM` member branch did not consume `: width`, and bitfield widths only accepted plain `TK_NUM` instead of `zero_cc_eval_const_expr(c)`. Added progress guards in struct body and brace-initializer loops so unrecognized tokens advance instead of spinning.
2. **C99 §5.1.2.2.3 Implicit `return 0;` in `main`** (`src/zero_cc/ir/ast_lowering.c`):
   - Emitted `ir_emit_mov_imm(VREG_0, 0)` before `epilogue_label` when lowering `main`, fixing ~20 execute tests that fell off the end of `main` without an explicit `return 0;`.
3. **Inlined Function Return Integer Width Extension** (`src/zero_cc/ir/lowering_state.h`, `ast_lowering_expr.c`, `ast_lowering_stmt.c`):
   - AST-inlined functions (`-O2`/`-O3`) lowered `return expr;` via `ir_emit_mov_reg(return_temp, res)` without truncating/sign-extending `res` to `callee_fn->return_type`. Recording `inlining.return_type` and calling `zero_cc_emit_int_extend` on inlined returns fixed `execute/pr108064.c` and `execute/pr44828.c`.
4. **Bitfield Brace-Initializer Packing (Local AST & Global `.data`)** (`src/zero_cc/types/type_funcs.h`, `parser/init_common.h`, `parser/ast_expr.h`, `jit/compile_unit.h`, `ir/ast_lowering_expr.c`):
   - Aggregate brace initializers previously resolved every bitfield to byte offset `0` with full storage-unit width, overwriting earlier bitfields. Added `ZC_INIT_BF_STAMP(off, sz, boff, bw)` and `zc_object_resolve_scalar_leaf_bf` so both local compound-literal/brace initializers (`OP_SHL` + `OP_OR` RMW) and global `.data` initializers pack bitfields into their shared storage unit. Fixed `execute/pr71700.c`.
5. **C99 Short-Circuit `&&` / `||` Lowering + Optimizer Multi-Def Guards** (`src/zero_cc/ir/ast_lowering_expr.c`, `opt/opt_loop.c`, `opt/opt_constant_fold.c`):
   - Replaced eager both-sides evaluation of `&&` and `||` with branching short-circuit evaluation (`execute/20000314-1.c` and null-guard tests).
   - Fixed the two optimizer passes that previously broke on multi-definition short-circuit destination temps: `instr_is_invariant` in `opt_loop.c` (where `OP_MOV_IMM` returned `true` before checking `def_dest_count > 1`) and `opt_fold_constants` in `opt_constant_fold.c` (which now tracks `g_def_count[temp]` and only folds constants with a single definition).

6. **Overflow Builtin Return Temp ID, Missing Overflow Intrinsics & Float Branch Lowering** (`src/zero_cc/ir/ast_lowering_expr.c`, `ast_lowering_stmt.c`):
   - `__builtin_mul_overflow` previously ended with `return 0;`, returning temp ID 0 (`VREG_0` / `%rdi`, caller's 1st argument value) instead of a fresh temp initialized to 0. Fixed `execute/pr68381.c` and `execute/pr71554.c`.
   - Lowered `__builtin_add_overflow`, `__builtin_sub_overflow`, and `__builtin_*_overflow_p` with target pointee width detection (1, 2, 4, 8 bytes). Fixed `execute/pr90311.c`, `execute/pr93494.c`, and `execute/pr105984.c`.
   - Float condition lowering in `lower_stmt_if`, `while`, `do_while`, `for`, `ternary`, and `!`: emitted float comparison against `0.0` (`ucomisd`/`cvtsi2sd`) when `c->last_expr_is_float` rather than testing integer `%rax`. Fixed `execute/980604-1.c`.
   - Result: 6 tests promoted to PASS on all 8 configs; Chunk M2 reduced from 16 → 10 remaining.

7. **Float Builtins (`fabsf`, `isinf`, `signbit`, `nanf`), IEEE-754 Float Negation, `-lm` & `link_error` Stub** (`src/zero_cc/ir/ast_lowering_expr.c`, `src/zero_cc/types/symbol_mgmt.h`, `src/main.c`):
   - Added `-lm` and `-Wl,--defsym=link_error=0` to system linker invocation (`src/main.c`) and dynamic `dlopen("libm.so.6")` + `link_error` stub to JIT `zero_cc_resolve_symbol` (`src/zero_cc/types/symbol_mgmt.h`). Fixed `execute/20020720-1.c`, `execute/ieee/20041213-1.c`, `execute/ieee/mzero4.c`, and promoted `execute/ieee/fp-cmp-7.c` from `REJECT` to `PASS`.
   - Directly lowered `__builtin_fabs`/`fabsf`, `__builtin_isinf`/`isinff`, `__builtin_signbit`/`signbitf`, `__builtin_nan`/`nanf`, and expanded `__builtin_inf`/`inff`/`huge_val`.
   - IEEE 754 float negation unary `-`: flipped bit 63 via stack XOR rather than `(0.0 - x)` (which produced `+0.0` on `-0.0` per IEEE 754 §6.3). Fixed `execute/pr35456.c` and `execute/ieee/pr109386.c`.
   - Result: 5 MISCOMPILEs + 1 REJECT promoted to PASS on all 8 configs; Chunk M2 reduced from 10 → 5 remaining.

8. **Chunk M2 Completion: LP64 System Type Macros, HW Division Regalloc Intervals, Struct Memory Lval/Rval Marshaling, and Current Function Index Tracking** (`src/zero_cc/core/preprocessor.h`, `parser/definitions.h`, `parser/const_eval.h`, `ir/live_range.h`, `ir/lowering_state.h`, `ir/ast_lowering.c`, `ir/ast_lowering_stmt.c`, `ir/ast_lowering_expr.c`, `ir/asm_emitter.c`, `isa/x86_64.c`):
   - LP64 standard type definitions (`__SIZE_TYPE__`, `__PTRDIFF_TYPE__`, `__INTPTR_TYPE__`, `__INT32_TYPE__`, `__INT64_TYPE__`, etc.) and multi-word scalar typedef parsing (`typedef long unsigned int`, `long int`, `unsigned short int`). Fixed `execute/pr58209.c`, `mul-sext.c`, `strlen-2.c`, `va-arg-2.c`, etc.
   - Hardware division clobber protection: x86-64 `idivq`/`divq` hard-clobbers `RAX` and `RDX`. Added fixed live-range intervals `[i, i]` for `VREG_0` (RAX) and `VREG_2` (RDX) at all division instructions in `live_range_compute`, preventing linear-scan regalloc from allocating loop invariants to `RDX` across division loops. Fixed `execute/memcpy-bi.c` (`SIGFPE`).
   - Local/global struct initialization and assignment from memory addresses: added `ast_expr_is_struct_addr()` helper to distinguish struct addresses (`NODE_INDEX`) from struct values, properly copying chunks instead of storing the pointer. Fixed `execute/pr71700.c` and `execute/921123-2.c`.
   - Float assignment evaluation order: saved `right_is_float` immediately after lowering RHS, preventing LHS array index expressions from clobbering `c->last_expr_is_float` and erroneously inserting `cvtsi2sd` conversions on float values. Also enhanced `NODE_CALL` and `va_arg` for 9-16B structs in memory. Fixed `execute/pr44575.c` and `execute/20021118-2.c`.
   - Function identity corruption & x86-64 MOVSXD operand bug: encapsulated `current_func_idx` in `LoweringState` so extern registrations during lowering (e.g. `abort`) do not overwrite the current function's label or prologue in `asm_emitter.c`; corrected `x86_emit_extend` to use destination register for both 32-bit and 64-bit MOVSXD operands. Fixed `execute/20171008-1.c`.
   - Result: Chunk M2 burned down to **0 failures (5/5 PASS)**; overall PASS increased to **2,042**, MISCOMPILE reduced to **170**.
   - *Note (Sprint 59)*: the original 78-test M2 batch was fully resolved at that point, but later baseline re-records (AArch64 IEEE-754 FP fix, Chunk M4/M5) **re-exposed 7 partial-mode `MISCOMPILE`s** (`execute/961213-1.c`, `execute/memchr-1.c`, `execute/pr15262-1.c`, `execute/pr36765.c`, `execute/pr43008.c`, `execute/pr84339.c`, `execute/strlen-3.c`) — each OK in AOT but crashing in JIT. Item 10 below clears them, bringing partial-mode MISCOMPILEs to **0**.

9. **Chunk M3: Generalized Lvalue Lowering, Non-Local `va_list` & `va_copy`, Multi-Chunk Struct Arguments & Prototype Conversions** (`src/zero_cc/core/preprocessor.h`, `parser/definitions.h`, `semantic/expr.c`, `ir/ast_lowering_internal.h`, `ir/ast_lowering_stmt.c`, `ir/ast_lowering_expr.c`):
   - **Bitfield C11 §6.3.1.1p2 Promotion & Multi-Word Specifiers**: bitfields with width < 32 or (width == 32 and signed) are promoted to signed `int`, else unsigned `int` (`execute/bf-sign-2.c`). Parser consumes trailing `long` and `int` in struct member specifiers (`execute/struct-aliasing-1.c`).
   - **`va_copy` & Macro Definitions**: predefined object/function macros `__va_copy` and `__builtin_va_copy` mapping to struct assignment (`execute/va-arg-14.c`).
   - **Generalized Lvalue Address Lowering (`zero_cc_lower_lvalue_addr`)**: unified computation of base addresses across local identifiers, global variables, struct members (`p->m`, `s.m`), array indexing, and pointer dereferences. Used in `NODE_VA_START`, `NODE_VA_ARG`, and compound assignments.
   - **Multi-Chunk Struct Copying (`zero_cc_emit_struct_copy_to_addr`)**: unified byte/chunk copying for struct assignments and initializations of arbitrary size (>16B, including 40B `va_list` objects) across local, member, global, and dereference destinations.
   - **By-Value Struct Arguments Across Registers & Stack**: generalized `ast_expr_is_struct_addr()` and `NODE_CALL` to marshal struct arguments > 16B chunk-by-chunk across GP argument registers and stack slots, correctly handling member struct arguments (`dummy(statep->ap)` in `execute/va-arg-13.c`) and global struct arguments (`execute/packed-aligned.c`).
   - **Function Prototype Argument Conversion**: implemented automatic conversion between scalar types (int to float/double via `cvtsi2sd`, float/double to int via `cvttsd2si`) during `NODE_CALL` argument marshaling according to the callee's prototype. Fixed `execute/va-arg-26.c`.
   - **Result**: 22 MISCOMPILE tests burned down to PASS across all configurations (`bf-sign-2`, `packed-aligned`, `struct-aliasing-1`, `va-arg-10`, `va-arg-13`, `va-arg-14`, `va-arg-26`, etc.). Overall PASS increased to **2,064**, MISCOMPILE reduced from **170 → 148**.

10. **Chunk M2 Final: Libc-Builtin Return Typing, JIT Param Specifier Runs & Pointer Width-Mark Exclusion** (`semantic/type.h`, `semantic/expr.c`, `semantic/visitor.c`, `jit/compile.h`, `ir/ast_lowering_expr.c`):
    - **Wrong return type for libc-mapped `__builtin_*`**: unrecognized `__builtin_*` identifiers were K&R-typed as returning `int`, so `__builtin_malloc`/`__builtin_memchr`/`__builtin_strlen` call results got `SEXT32` in lowering — truncating 64-bit pointers. Benign in non-PIE AOT (heap < 4 GiB); guaranteed SIGSEGV under 48-bit JIT addresses. Added `type_builtin_libc_return()` (`semantic/type.h`) mirroring the `__builtin_<name>` → libc `<name>` lowering map, called from both identifier-typing sites. Fixed `execute/pr15262-1.c`, `execute/pr36765.c`, `execute/pr43008.c`, `execute/pr84339.c`.
    - **JIT param parser left trailing specifier keywords**: `zero_cc_compile_function`'s hand-rolled parameter parser consumed `unsigned`/`signed` + at most one following keyword; in `unsigned long long int *v` the trailing `int` was re-parsed as a **second unnamed parameter** and the pointer param was recorded as `int *` (32-bit loads/stores on 64-bit values). The specifier block now consumes the full run of `signed`/`unsigned`/`char`/`short`/`long`/`int` in any order and resolves the base type (char > short > long > int). Fixed `execute/961213-1.c`.
    - **SEXT32 on pointer arithmetic results (width-mark leak)**: `zero_cc_operand_int_width`/`zero_cc_expr_int_info` recursed through `NODE_UNOP` for **every** unary op, so `&expr` reported the *pointed-to* value's narrow width (e.g. 4 for `&arr[0]` of a char/int array). `&arr[0] + i` was then marked as 32-bit int math and the MC layer's post-op width extension emitted an in-place `SEXT32` on the 64-bit pointer sum — truncating JIT data addresses (AOT at `0x400000` masked it). Both helpers now treat `TK_AMP` as 8-byte (no mark / width 8 unsigned). Fixed `execute/memchr-1.c` and `execute/strlen-3.c`; also promoted `execute/pr103405.c` O0-jit to OK (its O1–O3 JIT crash and AOT link failure — nested functions/trampolines — remain open, pre-existing).
    - **Result**: all 7 re-exposed partial-mode MISCOMPILEs promoted to PASS on all 8 configs; **partial-mode MISCOMPILEs: 0 remaining**. Overall PASS **2,170 → 2,209** (+8 incl. `compile/limits-structnest.c`), MISCOMPILE **75 → 48** (all 48 remaining are all-config failures → Chunk M3 scope). Baseline re-recorded (`--update-baseline`).

11. **Chunk M3 Part 0: Nested-Function Diagnostic (GNU Extension Rejection)** (`src/zero_cc/parser/ast_stmt.h`):
    - The statement parser accepted function **definitions** inside a block (the GNU nested-function / trampoline extension) as local `NODE_DECL`s: JIT silently compiled them with register-file aliasing (crashing on capture semantics — `execute/nestfunc-7.c`), and AOT emitted `call Foo` to a symbol never emitted (`undefined reference` link failure — `execute/pr103405.c`). The extension was simply unimplemented but silently accepted.
    - The definition path (function declarator + `TK_LBRACE` in statement context; file-scope definitions go through `zero_cc_compile_top_level_decl` and are untouched) now reports `error: nested function definition of '<name>' is not supported (GNU extension)` and skips the body with brace-depth counting so the enclosing function keeps parsing. Local function **prototypes** remain legal C (C99 allows declarations in blocks, only definitions are forbidden by 6.9p15).
    - `gcc -std=c11 -pedantic-errors` rejects nested definitions ("ISO C forbids nested functions"), so the gate triages all of them as `SKIP-C89`. Result: **7 `execute/` MISCOMPILEs** (`nestfunc-7`, `pr103405`, `20010209-1`, `20030501-1` (the runtime hang), `20040520-1`, `20090219-1`, `nest-stdar-1`) and **13 `compile/` tests** that only "passed" because compile-only mode never links/executes the extension all move to `SKIP-C89`. MISCOMPILE **48 → 41** (17 `__vector_size__` tests + 24 true C99/C11 bugs remain).

12. **`__attribute__((__vector_size__(N)))` SIMD Vector Types** (preprocessor + parser + semantic + IR lowering + all ISA backends):
    - **Preprocessor marker**: `__attribute__((__vector_size__(expr)))` (also among multiple comma-separated attributes, e.g. `(__vector_size__(16), __may_alias__)`) is preserved as the pseudo-keyword `__zc_vector_size__(expr)`; the preprocessor scans the top-level attribute list at paren depth 0. All other attribute handling unchanged.
    - **Types**: the marker wraps the element type in a `TYPE_USER_0` vector (`type_vector`): first-class type with `size = N`, `align = 16` when `N >= 16`. Vectors appear in every position GCC allows them: typedefs, locals, globals, parameters, return types, struct members, pointer targets, compound literals.
    - **Lowering model (per-lane scalar expansion)**: a vector *expression* always lowers to the **address** of a stack temp holding the lanes; consumers load through it. Element-wise binops/unops/comparisons expand lane-by-lane (`vlanes <= 32`), scalar operands broadcast. Float lanes run in the **double-width FP model** (`VLOADSS`/`VSTORESS` widen/narrow; `CVTSS2SD`/`CVTSD2SS` opcodes added to all three ISAs with both asm and raw emitters). Comparison results follow GCC semantics (float→int, double→long element vectors).
    - **ABI**: vectors ≤ 8 B pass/return in `VREG_0`; 9–16 B in the `VREG_0`/`VREG_2` pair; > 16 B use the `sret` hidden-pointer convention (`callee_uses_sret` extended). By-value vector **call results are materialized to a stack temp** (address form), which is why `ast_expr_is_vector_addr` reduces to `type_is_vector`.
    - **Casts**: vector→vector is a same-size bitwise reinterpret (raw-byte copy); vector→scalar requires matching sizes (GCC's "different size" rule) and extracts via a width-correct load; scalar→vector takes the scalar's **low `min(sizeof(vector), 8)` bytes as the vector's bit pattern** (NOT a broadcast — corrected in Sprint 64 by `execute/20050316-1/3`).
    - **`__builtin_shuffle(v1, v2, {idx...})`**: per-lane branchless lane select on the raw index bits (`m = idx & (n-1)`, `from_a = (idx & n) == 0`, bit-level select); the semantic layer types the call as the first argument's vector type.
    - **Compound assignment**: per-lane in-place `+=`/`-=`/`*=`/`/=` for both int and float element lanes, on local variables and through pointers (`*w += v`), vector RHS read per-lane or scalar RHS broadcast.
    - **Result**: all **17 `__vector_size__` c-torture tests** pass on every `(-O0..-O3 × JIT/AOT)` config (`scal-to-vec1..3`, `pr94524-1/2`, `pr94412`, `pr110817-1/2`, `pr85169`, `pr71626-1/2`, `pr123625-2`, `pr126405-3`, `pr85331`, `pr94591`, `pr92618`, `fp-cmp-cond-1`, `pr72824-2`, `simd-6`), plus `20060420-1` and `pr60960` regression fixes (float-vector compound assignment; ≤ 8 B vector assignment from address-form RHS). Overall PASS **2,196 → 2,216**, MISCOMPILE **41 → 23** (all 23 remaining are true C99/C11 correctness bugs). Baseline re-recorded (`--update-baseline`).

13. **Sprint 64: MISCOMPILE Burn-Down to Zero (12 + 5 newly-exposed + 2 re-exposed latent bugs)** (`src/zero_cc/parser/ast_stmt.h`, `parser/definitions.h`, `parser/ast_expr.h`, `parser/init_common.h`, `ir/ast_lowering.c`, `ir/ast_lowering_expr.c`, `ir/ast_lowering_stmt.c`, `ir/ast_lowering_internal.h`, `core/preprocessor.h`):
    - **12 originally-exposed `MISCOMPILE`s** (unblocked by the Sprint 63 predefined-macro batch) — every root cause a distinct latent codegen bug, now all fixed:
      - `pr32244-1`, `pr34971`: 40-bit bitfield **shift** results must be reduced per C11 6.5.7.3p2 (`zc_binop_bitfield_reduce` extended to `TK_SHL`/`TK_SHR`).
      - `pr108292`: 64-byte vector **compound assignment** (`v -= (V4DI)1`) — new per-lane in-place helper `zc_emit_vector_compound_inplace` (vector RHS read per-lane, scalar broadcast; float lanes via the double-width FP model).
      - `20010924-1`: **flexible-array-member initializers** — the trailing brace-group run past the struct's fixed size now grows the object's allocation (`zc_measure_fam_init_bytes`, `fam_bytes` threaded through `zc_init_resolve_positional` / `zero_cc_emit_brace_init_global` / all call sites).
      - `pr23135`, `stdarg-3`: **SysV >16B struct/union by-value arguments use the hidden-pointer convention** (one GP pointer to a 16-aligned caller buffer; callee copies from it into the parameter) — previously marshaled as 8-byte chunk streams, corrupting every following argument. Copies use the loop-based `zc_emit_mem_copy` (a 128 KB struct would otherwise need 16,384 temps — over the 16-bit per-function temp-id budget).
      - `pr82388`: **9–16B struct return values from address-form expressions** (`return h[29];`, `return *p;`) must load the value into the `VREG_0`/`VREG_2` pair, not return the address / first chunk only.
      - `pr55750`: `arr[i].n++` on a **global** struct array — the `++`/`--` lvalue path missed the `NODE_INDEX` base (`base + index*elem_size`).
      - `pr59014-2`, `pr61306-2`: integer **narrowing casts** (`(int)x`) must materialize a fresh temp — the in-place `sext`/`zext` destroyed the 64-bit source operand still needed later in the expression (`x + y` read zero).
      - `pr97386-1`: `__builtin_bswap16` implemented.
      - `20050604-1`: vector member/compound-assignment regression from the Sprint 63 vector work (stray `} else` split the member-assignment branches; global vector assignment from an address-form vector RHS needed a deref; vector member `+=` needed the in-place compound helper).
      - `921016-1`: **bitfield assignment expression value** — `l.m = j` on an 11-bit field must evaluate to the field-reduced value (masked + sign-extended), not the raw RHS (C99 6.5.16.1p2). Fixed in `assign_member_bitfield` (all 3 variants) and the member/compound-literal fallback paths.
      - `pr42833`: GCC-style `__INT*_TYPE__`/`__UINT*_TYPE__` predefined macros for LP64 (missing macros made `typedef __INT_LEAST8_TYPE__ int8_t;` resolve to `int`, silently widening all the test's arithmetic).
    - **5 newly-exposed latent `MISCOMPILE`s** (unblocked by the anon-union/macro fixes): `20050316-1/3` (**scalar→vector cast = low bytes, not broadcast** — new `NODE_CAST` path producing a fresh address-form vector object; float elements converted via the double-width FP model), `921016-1`, `pr42833` (above), `pr92904` (**9–16B struct-return copy register clobber** — `zero_cc_emit_struct_copy_to_addr` now captures the `VREG_0`/`VREG_2` pair into fresh allocatable temps immediately after the call, because the `dst+8` address arithmetic may legally reuse rax/rdx; vector-typed calls excluded since they return in address form).
    - **2 re-exposed latent `MISCOMPILE`s**: `pr85331`, `pr94591` — the same clobber hit `*r = __builtin_shuffle(y, m)` (16B vector call result stored through a pointer): the struct-return fast path must not fire for vector-typed calls.
    - **Local inline struct/union definitions** (`struct S { ... } v;`, anonymous `union { ... } u;` in block scope): parsed before base-type resolution; anonymous aggregates get generated tags (`__anon_struct_N`/`__anon_union_N`).
    - **Large hidden-pointer parameter copies deferred** in the callee prologue: the >16B hidden-pointer→param-storage copy must run **after all `PARAM_SAVE_INT`s** (and after `ir_emit_variadic_spill` for variadics) — an immediate copy let the regalloc place copy temps in not-yet-saved argument registers, corrupting later scalar parameters (`execute/20030914-2`, `va-arg-4` regression class).
    - **9 `REJECT`→PASS** from the same batch: `20000605-1`, `20030914-2`, `20040703-1`, `20050826-2`, `20071030-1`, `pr67226`, `pr93434`, `va-arg-4`, `961223-1` (GNU89 inline; needs `-fgnu89-inline` reference).
    - **Result**: MISCOMPILE **12 → 0** across all 3,940 tests; REJECT **519 → 480**; PASS **2,393 → 2,447**. All transitions are improvements (12 MISCOMPILE→PASS, 39 REJECT→PASS, 3 SKIP-C89→PASS); `make test` 156/156, `make test-opt` 117/117, `make test-isa-qemu` 44/44, diag battery baseline OK. Baseline re-recorded (`--update-baseline`).
    - **Next**: the REJECT burn-down (480 remaining; dominant signatures: `unexpected token at file scope`, `use of undeclared identifier`, `expected identifier in declaration`).

- **Sprint 65 — File-Scope REJECT Burn-Down + 5 MISCOMPILE Fixes** (2026-11-01):
    - **5 `MISCOMPILE` fixes**: `execute/conversion.c` (**unsigned 64-bit → float/double** — `cvtsi2sd` is a *signed* conversion; the shared `zc_emit_int_to_double` helper emits `v ≥ 0 ? cvtsi2sd(v) : cvtsi2sd((v>>1)|(v&1)) * 2.0` for u64/pointer sources, wired into float-return conversions; plus the **sticky-bit collapse join** bug — the uint64→float cast seeded its collapsed result only inside the taken branch, leaving the join temp undefined on the skip path, `(float) 2^63` → 0), `execute/zero-struct-1.c` (**zero-size struct** `struct g {}` — `*(struct g*)p = …` emitted phantom 8-byte load/store past the empty object, clobbering the adjacent global; zero-size deref now yields the pointer with no bytes moved), `execute/960215-1.c` (**`long double`** — `is_long_double` flag, 16B/16-align type lowered through the 64-bit double path, type-name + global-initializer support), `execute/ieee/mzero2.c` (**unary `+` on floats** must preserve `last_expr_is_float`, not `cvtsi2sd` the float bits), `execute/regstack-1.c` (K&R + constant-init batch).
    - **~230 `REJECT`→PASS**: **K&R old-style function definitions** (~85 tests, `zero_cc_parse_kr_decl` prototype registration, storage-class/qualifier-tolerant parameter detection), extern array suffixes + duplicate-global fix, pointer-leading comma declarators, `zero_cc_sync_globals_with_struct` (forward-declared struct completion), `__alignof__` constant expressions, array typedefs `typedef T name[N];`, multi-declarator function prototypes (per-declarator param slices, no `is_external`), mixed-declaration function prototypes, full constant pointer-expression initializers (`&g[N]`, `&"s"[N]`, pointer±pointer difference, cast prefixes), FP constant-expression initializers (`+1.0/0.0`), `? :` in constant array sizes, FNV-1a global-variable hash (the O(N) strcmp scan was a time bomb once hundreds of new globals were unblocked).
    - **Result**: REJECT **480 → 322**; PASS **2,447 → 2,682**; MISCOMPILE **0 → 0**; no PASS→bad, no TIMEOUT. `make test` 156/156, `make test-opt` 117/117, `make test-isa-qemu` 44/44, diag battery baseline OK, style 0 errors. Baseline re-recorded (`--update-baseline`).
    - **Next**: continue the REJECT burn-down (322 remaining; dominant: `expected identifier in declaration`, `use of undeclared identifier`, `unexpected token at file scope`, `unexpected token 'X' in statement`).

- **Sprint 66 — Function-Pointer Return Typing, O2 Inlining Correctness & Constant-Expr Burn-Down** (2026-11-01):
    - **3 `MISCOMPILE` fixes** (the last three in the corpus):
      - `execute/921208-1.c` / `execute/pr42231.c` (O0–O1): **function-pointer parameters carried no return type**. `zero_cc_parse_fn_param_block` modeled `double (*fp)(double)` as a generic 8-byte pointer (pointee = `int64`), so the semantic layer typed `(*fp)(a)` as returning `int` and the call lowering read the result from RAX instead of XMM0 (`Int(&f, 2.0)` printed 4198657.0). The parameter's base type is now computed **before** the function-pointer branch and used as the pointee (`fpt = ptr(ret)`, `is_func_ptr`), so the TypeInfo function carries the real return type and the indirect call picks the XMM0 path. (Side fix in the same branch: the named function-pointer parameter was **double-recorded** — the abstract-declarator fallback re-recorded `fp`, inflating `Int` to 3 parameters and REJECTing legal 2-arg calls; a `fn_ptr_param_done` flag guards the later branches.)
      - `execute/pr42231.c` (O2/O3): **small-function inlining corrupted indirect calls**. The O2 inliner clones the callee body with a param-name→arg-temp substitution, but the inlined `fun(depth)` (call through the inlined function-pointer parameter) failed `zero_cc_find_local(c, "fun")` in the caller's scope, so `is_indirect_call` was false and the call was emitted as a **direct** `call fun` (undefined symbol at link; wrong code when it did link). The inlining context now records `param_is_func_ptr[8]` (from `callee_fn->param_types[p].is_func_ptr`), and the call lowering treats a callee identifier matching an inlined function-pointer parameter as indirect (`ir/ast_lowering_expr.c`, `ir/lowering_state.h`).
      - `execute/pr19449.c`: **`__builtin_choose_expr`** — new lowering in `ast_lowering_expr.c`: when `__builtin_constant_p(cond)` holds, only the selected operand is lowered (the unselected must not run side effects); dynamic conditions evaluate both and select at runtime. `__builtin_constant_p` itself was upgraded from an immediate-literal check to a **structural** one (`zc_node_is_const_expr` recognizes literal classes, pure casts, unary/binary ops, ternaries, constant-indexed string access) with a matching `zc_ast_const_eval` constant evaluator; `const_eval.h` gained the same builtins for pre-lowering constant contexts.
    - **23 `REJECT`→PASS** (constant expressions, declarators, initializers):
      - **`sizeof` operand forms** (`const_eval.h` + `ast_expr.h`): `sizeof (type) { init }` compound literal (skips the balanced brace initializer — the size is the type's, `20010124-1`); `sizeof (a, b)` uses the **last** comma-operand's type (C99 6.5.17, `20011106-1`); a function name decays to a function pointer in `sizeof`/`_Alignof` (8 bytes on LP64, `20040909-1`).
      - **Abstract/multi-dimension parameter declarators** (`zero_cc_declarator_stack.h`, `compile.h`): `int ( int foo )` as an abstract pointer-to-function parameter (`20010313-1`); multi-bracket array parameters `int m[][4]` decay only the outermost dimension (named **and** unnamed, `pr77754-1/2/5`); `void` recognized as a parameter type; `zero_cc_declarator_is_function()` requires no pointer prefix at the name level so `int (*fp)(void)` is not misread as a function.
      - **Unsized array initializers** (`compile_unit.h`): first-dimension inference for `char ca[][4] = {…}` (top-level brace groups × known inner size) and unsized function-pointer arrays `int (*a[])(void) = { f0, f1 }` (top-level element counting with trailing-comma reset); global parenthesized pointer declarators and function-pointer arrays (data-reloc `target_kind == 3` against the function symbol in AOT/ELF/JIT).
      - **Constant pointer initializers** (`compile_unit.h`): `&((B + N)->m)` parenthesized/cast-prefixed forms fold to data-reloc addends (`const-addr-expr-1`, `20001116-1`); dot/arrow member chains after `&global`; `zc_try_paren_ptr_init` distinguishes a cast prefix `(int *)` from a transparent wrapper paren.
      - **Declarator clusters** (`definitions.h`, `compile.h`, `preprocessor.h`): array-suffixed struct typedefs, function-pointer typedefs over opaque structs (`typedef struct S *(*Op)(…)`), pointer-leading multi-declarator function prototypes (per-declarator star re-derivation), K&R `void * data;` parameter blocks, and the preprocessor no longer macro-expands string/char literal prefixes (`L"…"`, `u8'…'` are one lexical unit).
      - **JIT data-reloc BSS placement** (`execution.h`): global-object relocs now patch against the target's actual placement (`bss_offset` when `is_bss`, else `data_offset`) — `&bss_global` pointers were landing in the `.data` page and segfaulting (`pr43784`).
    - **Result**: MISCOMPILE **3 → 0**; REJECT **322 → 298**; PASS **2,682 → 2,708**; no PASS→bad, no TIMEOUT. `make test` 156/156, `make test-opt` 117/117 (1 nondeterministic), `make test-isa-qemu` 44/44, diag battery baseline OK (8/8 categories), style 0 errors. Baseline re-recorded (`--update-baseline`).
    - **Next**: continue the REJECT burn-down (298 remaining; dominant: `expected identifier in declaration`, `use of undeclared identifier`, `incompatible types in assignment`, `unexpected token 'X' in statement`, `static local initializer must be a constant literal`).

- **Sprint 67 — C99 `_Complex` Support, Complex Members/Initializers & Function-Name Hash** (2026-11-01):
    - **C99 `_Complex` implemented end-to-end** (the dominant remaining `use of undeclared identifier` cluster): type representation (`ZeroCCType.is_complex`, `zero_cc_type_complex`, `TYPE_KIND_COMPLEX`), prefix/suffix parsing of `_Complex`/`__complex__`/`__complex` (block decls incl. all comma declarators, function params/returns, file scope), complex arithmetic/assignment/equality typing, `__real`/`__imag` part extractors (parenthesis-less GNU forms desugar at **unary** level), address-form two-part lowering with register-indirect SSE halves, complex call/return ABI (two consecutive FP registers, `xmm0`/`xmm1`), complex casts, part-extractor lvalue assignment. **`_Complex double` division delegates to libgcc `__divdc3`** (in-compiler formulas can't match libgcc subnormal accuracy for `ieee/cdivchk*`); JIT resolves `libgcc_s.so.1` via `dlopen`. Integer-element complex is a clean REJECT (FP-only pipeline) covering globals as well. Side fixes: logical `&&`/`||` with pointer operands must yield `int` (they hit the pointer-arithmetic branch first, REJECTing `pr10352-1`); math `__builtin_*` remaps + return typing.
    - **Complex struct members** (`execute/pr42248.c`): `ZeroCCStructMember.is_complex` + prefix/suffix complex in `zero_cc_parse_struct_body` (previously `_Complex double a;` in a struct silently became a plain 8-byte double — sizeof 16→8), member TypeInfo mapping preserves complex, `NODE_MEMBER` value lowering returns the member **address** for complex members, and complex members initialize as **two scalar leaves** (`{ re, im }` positional/designated, bare-scalar elision `(v, 0)`, top-level `_Complex double z = { re, im };`, local brace inits store in place).
    - **Self-referential aggregate initializers** (`execute/20020529-1.c`): `struct xx s = {0, &s, 23};` — `local_lookup_bound` now raised to the declaration's own symbol index before lowering the initializer, so `&s` resolves to the new local.
    - **Function-name hash** (`types/symbol_mgmt.h`, `types/symbols.h`): `compile/limits-externdecl.c` (100,000 `extern int x();`) was the last `TIMEOUT` — `zero_cc_find_function`'s O(n) strcmp scan on every registration ≈ 5×10⁹ comparisons (8.6 s). Incremental open-addressing hash with same-name chains (proto/def prefer-definition semantics preserved) makes it O(1): 0.31 s parse, fast clean REJECT on the 100k-element initializer (test stays `SKIP-C89`-class). **Gate-caught bug**: the first lookup probed only the home bucket, so collision-displaced names were silently unfindable → 30 latent `MISCOMPILE`s (crashes) in the first full run; the lookup now probes the full linear sequence until an empty slot. Small-TU tests never collide at the 256-bucket initial cap, so unit suites stayed green while the gate caught it.
    - **Result**: PASS **2,708 → 2,789**; REJECT **298 → 223**; MISCOMPILE **0 → 0**; TIMEOUT **0 → 0**; no PASS→bad. `make test` 156/156, `make test-opt` 117/117 (1 nondeterministic), `make test-isa-qemu` 44/44, diag battery baseline OK (8/8 categories), style 0 errors. Baseline re-recorded (`--update-baseline`), confirmed on two consecutive full runs.
    - **Next**: continue the REJECT burn-down (223 remaining; dominant: `expected identifier in declaration` ~76, `unexpected token 'X' in statement`, `incompatible types in assignment`, `expected identifier in extern declaration`, `unexpected token in struct/union definition`, `expected expression in argument list`).

---

## 5. Chunked Burn-Down Plan (Remaining 1,160 Actionable Tests)

Clustering [`test/torture/baseline.json`](../../test/torture/baseline.json) (`148 MISCOMPILE` + `0 TIMEOUT` + `714 REJECT`) by failing configuration and diagnostic message breaks the remaining work into bounded sprint chunks, ordered **correctness before syntax**:

### Tier 1: `TIMEOUT` & `MISCOMPILE` Burn-Down (148 tests → 1 Chunk Remaining)

| Chunk | Scope & Count | Failure Signature & Root-Cause Cluster |
| :--- | :--- | :--- |
| **Chunk M1** | **10 `TIMEOUT`s ✅ COMPLETE**<br>(`0 TIMEOUT remaining`) | • **4 compile-time hangs** (`compile/20010518-2.c`, `compile/limits-enumconst.c`, `compile/pr103813.c`, `compile/pr94238.c`): resolved by .bss ELF NOBITS/mmap threshold, open-addressing enum registry hash table, enum false/true keyword fix, function-like macro whitespace handling, and __INT_MAX__ predefined macro.<br>• **2 optimizer loop hangs** (`execute/20011126-2.c`, `execute/pr43269.c`): resolved by CFG loop clean-entry LICM hoist guard and linear-scan register allocator VREG_0/VREG_2 fixed-interval conflict protection + active interval expire guard.<br>• **4 runtime loop hangs** (`execute/20000731-1.c`, `execute/20020404-1.c`, `execute/20061220-1.c`, `execute/930529-1.c`): resolved by float NODE_UNOP ++/--, static local struct/union brace initialization, ELF prototype symbol deduplication, AST statement progress guard, and unsigned promotion/condition code extension. Result: 9 PASS, 1 SKIP-C89. |
| **Chunk M2** | **78 Partial-Mode `MISCOMPILE`s ✅ COMPLETE**<br>(`0 MISCOMPILE remaining / 74 resolved` incl. 7 re-exposed in Sprint 59) | • **Root causes resolved:** Missing external-call symbol registration, missing builtin lowerings, dereference assignment evaluation order & multi-chunk struct copying, extern registration side effects / `current_func_idx`, float condition lowering, IEEE-754 float negation unary `-`, LP64 type macros, division fixed intervals in regalloc, float array element assignment order, libc-`__builtin_*` return typing, JIT param specifier runs, and pointer width-mark exclusion (Sprint 59 final batch). Result: **zero partial-mode MISCOMPILEs remain** in the re-recorded baseline. |
| **Chunk M3** | **41 All-Config `MISCOMPILE`s ✅ COMPLETE**<br>(`0 MISCOMPILE remaining`; was 48, 7 nested-function cases moved to `SKIP-C89` in M3 Part 0) | • **Vector family (17)**: `__attribute__((__vector_size__(N)))` tests (`pr94524-*`, `pr94412`, `pr110817-*`, `scal-to-vec*`, `ieee/fp-cmp-cond-1`, ...) — implemented as first-class vector types (M3 Part 1).<br>• **True C99/C11 bugs (24)**: all resolved across M3 Parts 2–3 — `aligned(N)` attribute, real overflow builtins, bit-field result reduction (`bitfld-3`), nested member array types + whole-member initializer placement + global struct returns (`pr60017`), int→float assignment conversion (`pr47538`), struct-member value copies (`20071202-1`), string+offset pointer initializers (`pr53084`), and the GNU `alias` attribute (`alias-3/4`, M3 Part 3). **MISCOMPILE: 0 in the entire corpus.** |

### Tier 2: `REJECT` Burn-Down (954 tests → 4 Chunks)

85% of all 954 `REJECT` cases (`419` in `compile/`, `535` in `execute/`) originate from 5 diagnostic sites:

| Chunk | Scope & Count | Diagnostic Signature & Feature Gap |
| :--- | :--- | :--- |
| **Chunk R1** | **~194 tests** | `use of undeclared identifier '<id>'` — GCC `__builtin_*` functions used in `execute/` and `execute/builtins/` (`__builtin_expect`, `__builtin_unreachable`, `__builtin_clz/ctz/ffs/popcount`, `__builtin_abs/labs`, `__builtin_alloca`, `__builtin_isnan/isinf`). |
| **Chunk R2** | **~210 tests** | `expected identifier in (extern) declaration` (**129**) + `unexpected token '<id>' in statement` (**80**) — Arbitrary declaration-specifier ordering (`int unsigned const`, `register`, `auto`, `__extension__`, `__inline__`) and parenthesized declarators (`int (x)`, `int (*p)[4]`). |
| **Chunk R3** | **~60 tests** | `unexpected token in struct/union definition` (**27**) + `incompatible types in assignment/initializer` (**30**) — Unnamed bitfields (`int : 0;`, `int : 4;`), mixed struct declarator lists (`int a, *b, c[2];`), and function/array decay edge cases. |
| **Chunk R4** | **~320+ tests** | `unexpected token at file scope` (**321**) + `expected expression in argument list` (**58**) — File-scope declarators returning function/array pointers, type-generic builtins (`__builtin_types_compatible_p`, `__builtin_choose_expr`), and K&R-compatible definitions accepted by `gcc -std=c11`. |

---

## Related Concepts
- Return to [Root Index](./index.md)
- See [Build, Test & Verification Guide](./build-and-verification.md)
- See [Optimization Passes & Safety Invariants](./optimization-passes.md)
- See [Frontend, Type System & Semantic Analysis](./frontend-and-types.md)
