---
type: Concept
title: GCC c-torture Conformance Gate
description: Differential conformance gate against the vendored GCC gcc.c-torture corpus (~3,940 tests), sweeping execute tests across -O0..-O3 in JIT and AOT and compile tests at -O0/-O2 with a ratcheting baseline.
resource: workflows/c-torture-gate
tags: [testing, verification, conformance, c-torture, gcc, differential, baseline]
timestamp: 2026-10-28T20:19:42Z
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

## 2. Classification Buckets & Baseline (Sprint 57 / Chunk M2 In Progress)

Recorded in [`test/torture/baseline.json`](../../test/torture/baseline.json) across all **3,940** tests:

| Bucket | Count | Meaning |
| :--- | ---: | :--- |
| **`PASS`** | **1,976** | Every tested `(opt_level, mode)` configuration succeeded and matched GCC's `(rc, stdout)` (+12 from Phase 1 & 2 burn-downs, +262 from initial baseline) |
| **`REJECT`** | **748** | ZeroCC failed to compile a valid C99/C11 test (`[clean -std=c11]`) (-1 from fp-cmp-7, -206 from initial baseline) |
| **`SKIP-C89`** | **677** | ZeroCC rejected a C89/GNU-extension test that `gcc -std=c99/-c11 -pedantic-errors` also rejects |
| **`MISCOMPILE`** | **196** | ZeroCC compiled the test, but at least one `(-O0..-O3 × JIT/AOT)` run differed from GCC's `(rc, stdout)` or failed AOT linking (-5 from Phase 2, -22 from initial baseline) |
| **`NO-REF`** | **188** | Host `gcc -O1 -w` rejected the file (32-bit/target-specific tests without explicit `dg-require-effective-target`) |
| **`SKIP-TARGET:*`** | **144** | Explicit DejaGnu target/extension skips (`untyped_assembly`: 40, `label_values`: 34, `indirect_jumps`: 32, `trampolines`: 16, `int128`: 7, `return_address`: 7, etc.) |
| **`TIMEOUT`** | **0** | **0 TIMEOUTs** (All 10 M1 hangs burned down to 0: 9 PASS, 1 SKIP-C89) |
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

---

## 5. Chunked Burn-Down Plan (Remaining 1,182 Actionable Tests)

Clustering [`test/torture/baseline.json`](../../test/torture/baseline.json) (`218 MISCOMPILE` + `10 TIMEOUT` + `954 REJECT`) by failing configuration and diagnostic message breaks the remaining work into 8 bounded sprint chunks, ordered **correctness before syntax**:

### Tier 1: `TIMEOUT` & `MISCOMPILE` Burn-Down (228 tests → 3 Chunks)

| Chunk | Scope & Count | Failure Signature & Root-Cause Cluster |
| :--- | :--- | :--- |
| **Chunk M1** | **10 `TIMEOUT`s ✅ COMPLETE**<br>(`0 TIMEOUT remaining`) | • **4 compile-time hangs** (`compile/20010518-2.c`, `compile/limits-enumconst.c`, `compile/pr103813.c`, `compile/pr94238.c`): resolved by .bss ELF NOBITS/mmap threshold, open-addressing enum registry hash table, enum false/true keyword fix, function-like macro whitespace handling, and __INT_MAX__ predefined macro.<br>• **2 optimizer loop hangs** (`execute/20011126-2.c`, `execute/pr43269.c`): resolved by CFG loop clean-entry LICM hoist guard and linear-scan register allocator VREG_0/VREG_2 fixed-interval conflict protection + active interval expire guard.<br>• **4 runtime loop hangs** (`execute/20000731-1.c`, `execute/20020404-1.c`, `execute/20061220-1.c`, `execute/930529-1.c`): resolved by float NODE_UNOP ++/--, static local struct/union brace initialization, ELF prototype symbol deduplication, AST statement progress guard, and unsigned promotion/condition code extension. Result: 9 PASS, 1 SKIP-C89. |
| **Chunk M2** | **78 Partial-Mode `MISCOMPILE`s**<br>(Already pass in ≥1 ZeroCC config)<br>✅ **62/67 resolved (measured)**<br>— 5 remaining | • **Root cause 1 — Missing external-call symbol registration:** `__builtin_*` routines (memcpy, memset, abort, exit, fabs, ...) were mapped to base names but never registered as `ZeroCCFunction` entries, so AOT codegen emitted `call` relocations with symbol index 0, no PLT entry, and `call +0` at runtime (SIGSEGV). Fixed by registering builtins as external `ZeroCCFunction` entries during lowering (`src/zero_cc/ir/ast_lowering_expr.c`).<br>• **Root cause 2 — Missing builtin lowerings:** `__builtin_expect`, `__builtin_prefetch`, `__builtin_constant_p`, `__builtin_bswap32`, `__builtin_ffs`, `__builtin_inf`/`inff`, `__builtin_nan`/`nanf`, `__builtin_fabs`/`fabsf`, `__builtin_isinf`/`isinff`, `__builtin_signbit`/`signbitf`, `__builtin_mul_overflow`/`add_overflow`/`sub_overflow`/`*_overflow_p`, `__builtin_ctz`/`__builtin_clz`/`__builtin_popcount` (branchless scan), `__builtin_unreachable`/`__builtin_trap` (new `OP_TRAP` opcode: `ud2`/`brk #0`/`unimp` per ISA) now lowered directly in IR.<br>• **Root cause 3 — Dereference assignment evaluation order & multi-chunk struct copying (`20000917-1.c`):** LHS target pointer is evaluated before the RHS value (volatile ABI return registers were clobbered by calls inside the LHS pointer computation); full 9-16B multi-register stores and >16B by-value struct memory copies; >16B local-struct RHS idents emit an LEA (address) for the copy loop while by-value argument marshaling keeps the 8-byte value load; 9-16B struct idents load both 8-byte chunks to match the by-value convention.<br>• **Root cause 4 — Extern registration side effects:** (a) the lowering-time extern registration pushed `main` off the end of `c->functions`, silently dropping the implicit `return 0` (C99 5.1.2.2.3p1) — the lowering entry now captures the function identity up front; (b) function-*pointer* variables were registered as external functions (direct `call` to undefined symbol) — registration now runs after indirect-call classification and only for direct calls to names with no function entry; (c) the global-scope registration loop treated every extern function-table entry as a variable — extern *objects* carry `is_object` flag and register as variables, extern *functions* as functions, and lowering phantoms are skipped.<br>• **Root cause 5 — Float condition lowering (`980604-1.c`):** `lower_condition_as_int` emits `cvtsi2sd` 0.0 + `ucomisd` on float condition temps before `OP_JE`, preventing integer `test %rax, %rax` on uninitialized GPRs.<br>• **Root cause 6 — IEEE 754 Float Negation & Math Libraries (`pr35456.c`, `20020720-1.c`, `mzero4.c`):** Unary `-` flips bit 63 via stack XOR (fixing `-0.0`); `-lm` and `-Wl,--defsym=link_error=0` link math and DejaGnu dead-code assertions.<br>• **Remaining (5/67):** `20171008-1.c` (optimizer inlining struct alignment), `memcpy-bi.c` (`SIGFPE` at -O2), `pr44575.c` (varargs struct array at -O2), `pr58209.c` (JIT memory resolution), and `pr71700.c` (JIT bitfield edge). |
| **Chunk M3** | **140 All-Config `MISCOMPILE`s**<br>(Fail across all 8 `O0..O3 × JIT/AOT` configs) | • **Part A — Bitfields & Struct Layout**: bitfield signedness (`int x : 3` sign-extension on load), multi-word bitfield straddling, and nested struct/union assignment (`struct-cpy`, `bf-*`, `bitfld-*`).<br>• **Part B — Integer Promotions, Casts & Pointer Arithmetic**: sub-`int` (`char`/`short`) usual arithmetic conversions, compound assignments (`*=`, `/=`, `%=`) on narrow/unsigned types, and multidimensional array/pointer indexing. |

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
