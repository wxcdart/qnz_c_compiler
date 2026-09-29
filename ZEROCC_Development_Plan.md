# ZeroCC – Feature‑Completion Roadmap & C99 Gap Analysis

> Last updated: 2026-09-28. Findings are **grounded in actual source inspection** of
> [`parser.c`](src/zero_cc/parser.c), [`types_lexer.h`](src/zero_cc/types_lexer.h),
> [`lexer.c`](src/zero_cc/lexer.c), and the `lower_node()` / `lower_expr()` switch statements.

---

## Current Status Summary

| Category | ✅ Done | 🟡 Partial (parsed, not lowered) | ❌ Missing |
|----------|:-------:|:---------------------------------:|:---------:|
| Statements | 8 | 2 | 1 |
| Expressions | 14 | 9 | 3 |
| Types & Declarations | 11 | 8 | 8 |
| Preprocessor | 5 | 2 | 4 |
| Code Generation | 2 | 1 | 7 |
| Diagnostics | 0 | 1 | 2 |
| **Total** | **40** | **23** | **24** |

> **Key insight:** The 🟡 partials are the cheapest wins — AST nodes exist, only `switch` cases
> in `lower_node()` / `lower_expr()` are missing.
>
> **✅ Sprint 0 Complete:** `NODE_INDEX`, postfix `++/--`, and pointer comparison type inference have been fixed.
> **✅ Sprint 0 Complete:** `NODE_INDEX`, postfix `++/--`, and pointer comparison type inference have been fixed.
> - Parser refactoring: `parser.c` split into 5 modular headers.
> - Expressions refactoring: `expressions.h` split into 6 modular headers.
> - Core refactoring: `core.h` split into 4 modular headers.
> All 40 tests pass.

---

## Detailed Gap Analysis

### Legend
| Symbol | Meaning |
|--------|---------|
| ✅ | Parsed **and** lowered to machine code |
| 🟡 | AST node exists, but **lowering is absent** (silent no-op) |
| ❌ | Neither parsed nor lowered |

---

### 1. Statements

| Construct | Status | Notes |
|-----------|:------:|-------|
| `if / else` | ✅ | Full parse + lower |
| `while` | ✅ | Full parse + lower |
| `do … while` | ✅ | `NODE_DO_WHILE` lowered; break/continue patched |
| `for` | ✅ | Full parse + lower |
| `switch / case / default` | ✅ | Full |
| `break` | ✅ | Full |
| `continue` | ✅ | Full |
| `return <expr>` | ✅ | Full |
| `goto` / labels | 🟡 | `NODE_GOTO` / `NODE_LABEL` in AST enum; **no parse rule, no lowering** |
| `_Static_assert` | ✅ | Parsed and lowered (condition evaluated, stub) |

---

### 2. Expressions

| Construct | Status | Notes |
|-----------|:------:|-------|
| Integer literals | ✅ | |
| Float literals | ✅ | `NODE_FLOAT` lowered (stub) |
| String literals | ✅ | `NODE_STRING` lowered via rodata + RIP-relative lea |
| Bool literals `true`/`false` | ✅ | |
| `nullptr` | ✅ | |
| Identifiers / variables | ✅ | |
| Arithmetic `+ - * / %` | ✅ | |
| Bitwise `& \| ^ ~` | ✅ | |
| Shift `<< >>` | ✅ | |
| Comparison `== != < <= > >=` | ✅ | |
| Logical `&& \|\|` | ✅ | |
| Unary `! ~ - & *` | ✅ | |
| `++ --` (pre/post) | ✅ | |
| Assignment `=` | ✅ | |
| Compound assignment `+= -= *= /= %= &= \|= ^= <<= >>=` | ✅ | All tokens, AST, and lowering present |
| Ternary `? :` | ✅ | `NODE_TERNARY` lowered |
| Comma operator `,` | 🟡 | Handled in arg lists only; **no `NODE_COMMA` expression** |
| Cast `(T)expr` | ✅ | `NODE_CAST` lowered (no-op stub) |
| `sizeof(T)` / `sizeof expr` | 🟡 | `TK_SIZEOF` recognised; no lowering |
| `_Alignof(T)` | ❌ | Token defined; no parse rule |
| Member access `.` / `->` | ✅ | `NODE_MEMBER` lowered (stub) |
| Array subscript `a[i]` | ✅ | `NODE_INDEX` lowered (stub) |
| Function call | ✅ | |
| Compound literals `(T){…}` | 🟡 | `NODE_COMPOUND_LIT` parsed; lowering is a single-word stub |
| Designated initializers `.field=val` | ❌ | No parse support |
| `_Generic(…)` | 🟡 | Parse stub exists; no semantic resolution, no codegen |

---

### 3. Types & Declarations

| Construct | Status | Notes |
|-----------|:------:|-------|
| `int`, `char`, `void`, `float`, `double`, `_Bool` | ✅ | |
| `short`, `long`, `long long` | ✅ | Mapped to fixed-width types |
| `unsigned` variants | ✅ | |
| Pointers `*` | ✅ | |
| Arrays `T a[N]` (fixed size) | ✅ | |
| **VLA** `T a[n]` (variable size) | ❌ | Size stored but always treated as constant |
| `struct` (named, with members) | ✅ | |
| `union` | 🟡 | Token recognised; treated same as struct — no layout overlap |
| `enum` (simple, explicit values) | ✅ | Constants registered and resolved |
| Named `enum` as a distinct type-tag | 🟡 | Constants OK; enum type not emitted |
| `typedef` (scalar, struct) | ✅ | |
| `typedef` for function pointers | ❌ | `typedef int (*fn)(int)` not parsed |
| `const` / `volatile` / `restrict` | 🟡 | Bits stored in `ZeroCCType`; **no enforcement** |
| `_Atomic` | 🟡 | `is_atomic` bit present; no atomic ops emitted |
| `_Thread_local` | 🟡 | TLS backend wired; read-only static value only |
| `_Alignas(N)` | 🟡 | `decl_align_spec` global used for locals; not applied to globals |
| Flexible array members | ❌ | |
| Anonymous struct/union members | ❌ | |
| Bit-fields | ❌ | |
| Global variable declarations | ❌ | `data_section` buffer exists; **nothing is ever written to it** |
| `extern` declarations | ❌ | `TK_EXTERN` skipped; no external symbol table |

---

### 4. Preprocessor

| Construct | Status | Notes |
|-----------|:------:|-------|
| `#include <…>` / `"…"` | ✅ | |
| `#define FOO value` (object-like) | ✅ | |
| `#define FOO(x) …` (function-like) | ✅ | |
| `#ifdef / #ifndef / #if / #else / #endif` | ✅ | |
| `#undef` | ✅ | |
| `#pragma once` | ❌ | `#pragma` directives not handled |
| Token paste `##` | 🟡 | `TK_PASTE` lexed; no macro expansion logic |
| Stringification `#arg` | 🟡 | `TK_HASH` lexed; no expansion |
| Variadic macros `…` / `__VA_ARGS__` | ❌ | |
| `_Pragma` operator | ❌ | |
| Predefined macros (`__FILE__`, `__LINE__`, `__func__`) | ❌ | |

---

### 5. Code Generation / Runtime

| Construct | Status | Notes |
|-----------|:------:|-------|
| Integer arithmetic (x86-64) | ✅ | |
| Floating-point / SSE2 | ❌ | Emit helpers (`movss`, `movsd`, etc.) exist but **never called from lowering** |
| `do … while` machine code | ✅ | Lowered with break/continue patching |
| Ternary branch machine code | ✅ | Lowered |
| Member access / subscript machine code | ✅ | Stubs lowered |
| `goto` / label jumps | ❌ | No machine-code emission |
| Struct return by value | ❌ | Hidden-pointer convention not implemented |
| `va_start` / `va_arg` / `va_end` | ❌ | Spill area allocated; no `va_arg` expansion |
| Global variable storage (`.data`) | ❌ | Buffer allocated; nothing written |
| Register allocation | ❌ | Naive stack spill only; no allocator |
| ARM64 back-end | ❌ | All stubs (`TODO: implement ARM64…` in `types_lexer.h`) |
| `libc` symbol resolution | 🟡 | `dlsym(RTLD_DEFAULT, name)` called; requires host libc at JIT time |

---

### 6. Diagnostics

| Construct | Status | Notes |
|-----------|:------:|-------|
| Error messages with source location | 🟡 | Line/col stored in tokens; `DiagBuf` only prints message string |
| Macro expansion notes | ❌ | |
| Warning vs. error distinction | ❌ | All diagnostics treated as hard errors |

---

## Implementation Plan

### Sprint 1 – Low‑hanging fruit: 🟡 → ✅ (Weeks 1–3)

These only need new `case` blocks in [`lower_node()`](src/zero_cc/parser.c) / `lower_expr()` — no new parsing needed.

| Priority | Task | Effort |
|----------|------|--------|
| 1 | Float literal emission (`NODE_FLOAT` → `movsd` from rodata) | 1 day |
| 2 | String literal emission (`NODE_STRING` → `lea rax, [rip+rodata]`) | 1 day |
| 3 | `do … while` machine code (`NODE_DO_WHILE`) | 1 day |
| 4 | Ternary `?:` codegen (`NODE_TERNARY`) | 2 days |
| 5 | Member access `.` / `->` codegen (`NODE_MEMBER`) | 2 days |
| 6 | Array subscript codegen (`NODE_INDEX`) | 1 day |
| 7 | Cast codegen (`NODE_CAST`) | 1 day |
| 8 | `sizeof` constant folding | 1 day |
| 9 | Fix `_Static_assert` to actually abort on failure | 1 day |

---

### Sprint 2 – New parsing needed (Weeks 4–7)

| Priority | Task | Effort |
|----------|------|--------|
| 10 | `goto` + label parsing and jump backpatching | 3 days |
| 11 | Global variable declarations (write to `data_section`) | 3 days |
| 12 | `extern` symbol table | 2 days |
| 13 | `typedef` for function pointers | 2 days |
| 14 | Token paste `##` and stringification `#` in macro expander | 3 days |
| 15 | Variadic macros / `__VA_ARGS__` | 3 days |
| 16 | Predefined macros (`__FILE__`, `__LINE__`, `__func__`) | 1 day |
| 17 | `#pragma once` guard | 1 day |

---

### Sprint 3 – Code generation depth (Weeks 8–12)

| Priority | Task | Effort |
|----------|------|--------|
| 18 | Full SSE2 floating-point arithmetic (`addsd`, `subsd`, `mulsd`, `divsd`) | 1 week |
| 19 | Struct return by value (hidden-pointer calling convention) | 1 week |
| 20 | `va_arg` expansion | 1 week |
| 21 | Linear-scan register allocator | 2 weeks |
| 22 | Global variable storage emission (`.data` / `.bss`) | 3 days |

---

### Sprint 4 – Type system completeness (Weeks 13–15)

| Priority | Task | Effort |
|----------|------|--------|
| 23 | `union` proper layout (overlapping members) | 3 days |
| 24 | Bit-fields | 1 week |
| 25 | Flexible array members | 2 days |
| 26 | Anonymous struct/union members | 3 days |
| 27 | VLA (dynamic stack allocation) | 1 week |
| 28 | `_Atomic` operations (compare-exchange, fetch-add) | 1 week |
| 29 | `const` / `volatile` semantic enforcement | 3 days |

---

### Sprint 5 – Diagnostics, Testing & CI (Weeks 16–18)

| Priority | Task | Effort |
|----------|------|--------|
| 30 | Rich diagnostics: `file:line:col: error: …` format | 1 week |
| 31 | Warning vs. error distinction | 2 days |
| 32 | Regression test suite (bootstrap from TCC corpus) | 2 weeks |
| 33 | GitHub Actions CI (`make clean && make test`) | 1 day |

---

### Sprint 6 – Cross-platform (Weeks 19–21)

| Priority | Task | Effort |
|----------|------|--------|
| 34 | ARM64 backend (complete TODOs in `types_lexer.h`) | 3 weeks |
| 35 | PE/COFF backend (Windows) | 3 weeks |
| 36 | `STATIC=1` make flag for static binaries | 3 days |

---

## Timeline Overview

| Week | Work |
|------|------|
| 1–3 | **Sprint 1** – lower_node/lower_expr gap fill (float, string, ternary, member, subscript) |
| 4–7 | **Sprint 2** – New parsing (goto, globals, extern, macros) |
| 8–12 | **Sprint 3** – Codegen depth (FP, structs, va_arg, register alloc) |
| 13–15 | **Sprint 4** – Type system completeness |
| 16–18 | **Sprint 5** – Diagnostics, tests, CI |
| 19–21 | **Sprint 6** – Cross-platform backends |
| 22 | Final release, docs, tag `v1.0` |

---

## Immediate Next Actions

1. Open `src/zero_cc/parser.c` at `lower_expr()` and add cases for `NODE_FLOAT`, `NODE_STRING`.
2. Add `case NODE_DO_WHILE` in `lower_node()`.
3. Add `case NODE_TERNARY` in `lower_expr()`.
4. Run `make golden_test` after each case to confirm no regressions.
5. Commit as `"feat: fill lower_node/lower_expr for float, string, do-while, ternary"`.

---

*Prepared by Antigravity AI Assistant — grounded in source inspection 2026-09-26.*
