---
type: Concept
title: GCC c-torture Conformance Gate
description: Differential conformance gate against the vendored GCC gcc.c-torture corpus (~3,940 tests), sweeping execute tests across -O0..-O3 in JIT and AOT and compile tests at -O0/-O2 with a ratcheting baseline.
resource: workflows/c-torture-gate
tags: [testing, verification, conformance, c-torture, gcc, differential, baseline]
timestamp: 2026-10-26T21:52:49Z
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

## 2. Classification Buckets & Baseline (Sprint 57)

Recorded in [`test/torture/baseline.json`](../../test/torture/baseline.json) across all **3,940** tests:

| Bucket | Count | Meaning |
| :--- | ---: | :--- |
| **`PASS`** | **1,714** | Every tested `(opt_level, mode)` configuration succeeded and matched GCC's `(rc, stdout)` |
| **`REJECT`** | **954** | ZeroCC failed to compile a valid C99/C11 test (`[clean -std=c11]`) |
| **`SKIP-C89`** | **699** | ZeroCC rejected a C89/GNU-extension test that `gcc -std=c99/-c11 -pedantic-errors` also rejects |
| **`MISCOMPILE`** | **218** | ZeroCC compiled the test, but at least one `(-O0..-O3 × JIT/AOT)` run differed from GCC's `(rc, stdout)` or failed AOT linking |
| **`NO-REF`** | **190** | Host `gcc -O1 -w` rejected the file (32-bit/target-specific tests without explicit `dg-require-effective-target`) |
| **`SKIP-TARGET:*`** | **144** | Explicit DejaGnu target/extension skips (`untyped_assembly`: 40, `label_values`: 34, `indirect_jumps`: 32, `trampolines`: 16, `int128`: 7, `return_address`: 7, etc.) |
| **`TIMEOUT`** | **10** | Exceeded per-step compile (10s) or run (5s) timeout |
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

# Filter by substring or limit count during bug triage (never overwrites baseline.json)
python3 tools/torture_run.py --only execute/pr --limit 100
python3 tools/torture_run.py --only execute/2000

# Re-record baseline after fixing MISCOMPILE / REJECT / TIMEOUT bugs
python3 tools/torture_run.py --update-baseline
```

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

---

## Related Concepts
- Return to [Root Index](./index.md)
- See [Build, Test & Verification Guide](./build-and-verification.md)
- See [Optimization Passes & Safety Invariants](./optimization-passes.md)
- See [Frontend, Type System & Semantic Analysis](./frontend-and-types.md)
