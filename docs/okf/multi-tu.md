---
type: Concept
title: Multi-Translation-Unit Driver (T1) & JIT Image Merge
description: How zerocc compiles multiple .c inputs - per-TU compile, the multi-TU JIT image merge (zero_cc_tu_merge), the global-registry snapshot discipline, and the extern-object store/load paths that make cross-TU globals work.
resource: driver/multi-tu
tags: [driver, multi-tu, jit, merge, relocations, globals, linker]
timestamp: 2026-10-22T21:53:24Z
generated: pi-agent
verified: engineering-lead
status: active
---

# Multi-Translation-Unit Driver (T1) & JIT Image Merge

`zerocc a.c b.c …` compiles every input and combines them. AOT combines via
per-TU objects handed to the system linker; JIT combines by merging all TUs
into one executable image before running `main`.

## Driver modes (`src/main.c`)

| Invocation | Behavior |
|---|---|
| `zerocc a.c b.c -o out` | per-TU raw object (`/tmp/zerocc_PID_i.o`) → system linker (`gcc`/cross-gcc) combines them |
| `zerocc a.c b.c --run` | each TU compiles into its own `ZeroCCCompiler`; `zero_cc_tu_merge` folds them into the first; one `zero_cc_execute_jit` on the merged image |
| `zerocc -c a.c b.c` / `-S` | one `.o` / `.s` per input (derived next to the input file); `-o` with `-c/-S` + multiple inputs is an error |

Driver gate: `make test-driver` (`tools/driver_tests.py`, 19 cases in
`test/driver/` — multi-TU JIT/AOT, cross-TU globals & doubles, 3-file call
chains, per-input `-c`/`-S`, duplicate-definition & missing-`main` errors).
`test/driver/%` is excluded from the `make test` glob like `test/diag/%`.

## The merge (`src/zero_cc/core/multi_tu.h`)

The JIT runs **one** image (text+rodata on an RX page, `.data` on an RW page;
every relocation resolved by **name** at patch time in
`zero_cc_execute_jit`). Merging TU `tu` into `base`:

- `.text`/`.rodata`/`.data` are appended at 16/8/8-aligned bases; every
  `tu` offset shifts by the per-section base.
- Function table is appended (re-based); a duplicate *definition* is a
  multiple-definition error. Prototype-in-`base` + definition-in-`tu` is the
  expected cross-TU shape — `zero_cc_find_function` prefers definitions.
- Relocations need no retargeting: call targets and `&global`/`OP_MOV_GLOBAL`
  references are bare names resolved at patch time against the merged table
  and registry.
- `str_relocs` shift both fields; `data_relocs` shift by the data base
  (rodata targets by the rodata base).

### Global-registry discipline (the trap)

`reset_global_vars()` runs at the **start** of every `zero_cc_compile_c99`, so
compiling TU `i` wipes the merged registry. The driver therefore:

1. snapshots the merged registry (TUs 0..i-1, merged offsets) **before**
   compiling TU `i`;
2. after the compile, re-bases TU `i`'s own entries (the registry's current
   contents) by `align8(base->data.length)`;
3. re-appends the snapshot (via `zero_cc_global_vars_reserve`);
4. only then calls `zero_cc_tu_merge` (which no longer touches the registry).

## Cross-TU globals: extern-object load/store

An `extern int counter;` declaration is registered in the **function table**
with `is_external=true` (no separate global-var table entry in that TU). The
lowering must therefore handle extern objects on both sides:

- **Load** (`NODE_IDENT`, `ast_lowering_expr.c`): function-table fallback —
  `OP_MOV_GLOBAL` (address patched at run/link time) + deref; FP externs
  (`float`/`double`) must take the `vloadss`/`vloadsd` path, not the integer
  deref (raw IEEE-754 bits).
- **Store** (`NODE_ASSIGN`, same file): the same fallback emits
  `OP_MOV_GLOBAL` + width/FP-correct store. **Pre-T1 this path did not
  exist** — a store to an extern-only global fell off the end of
  `NODE_ASSIGN` and was silently dropped, so `g += 1` on a cross-TU global
  compiled to a no-op.

## Unnamed prototype parameters (C99 6.7.6.3)

`int f(int, int *);` — parameter nodes must be created for **unnamed**
parameters too (empty name), in **both** parameter-parse loops:
`zero_cc_compile_function` (`jit/compile.h`, file scope — the one that feeds
the function table + semantic scope) and `zero_cc_synchronize_ast`
(`parser/ast_stmt.h`). Pre-fix, unnamed params were silently dropped, so
`int f(int);` parsed as a **0-parameter prototype** and legal calls were
rejected by the Sprint-42 arity checker (and argument promotion to the param
type was skipped). `(void)` remains the zero-parameter marker (explicit
prototype, no param).

## Known limitations (documented, out of T1 scope)

- `static` file-scope globals from different TUs share the bare-name global
  registry (no per-TU mangling) — same-name statics alias.
- `_Thread_local` storage is per-compiler and not merged.
- `int *p = &g;` initializers referencing a *cross-TU* global stay 0 (the
  initializer rebase only covers same-TU targets).
- The AOT `-o` path links through the **system** linker; an own linker is T2.
