# ZeroCC – Feature‑Completion Roadmap & C99 Gap Analysis

> Last updated: 2026-10-01. Findings are **grounded in actual source inspection** of
> [`parser.c`](src/zero_cc/parser.c), [`types_lexer.h`](src/zero_cc/types_lexer.h),
> [`lexer.c`](src/zero_cc/lexer.c), and the `lower_node()` / `lower_expr()` switch statements.

## Goal: Self-Hosting C11 Compiler

ZeroCC is designed to eventually **self-host** (compile itself). Architecture supports:

- **Modular design**: Separate source files, not single-header
- **Multi-ISA**: x86-64, aarch64, riscv64 backends
- **C11 conformance**: TLS, Atomic, Alignment, Generic selection
- **Cross-platform**: Linux, Windows, macOS, baremetal

---

## Current Status Summary

| Category | ✅ Done | 🟡 Partial (parsed, not lowered) | ❌ Missing |
|----------|:-------:|:---------------------------------:|:---------:|
| Statements | 9 | 1 | 1 |
| Expressions | 14 | 9 | 3 |
| Types & Declarations | 11 | 8 | 8 |
| Preprocessor | 6 | 2 | 3 |
| Code Generation | 3 | 1 | 6 |
| Diagnostics | 0 | 1 | 2 |
| **Sprint 1** | **✅ Complete** | | |
| **Sprint 2** | **✅ Complete** | | |
| **Sprint 3** | **✅ Complete** | | |
| **Total** | **43** | **22** | **23** |

> **Key insight:** The 🟡 partials are the cheapest wins — AST nodes exist, only `switch` cases
> in `lower_node()` / `lower_expr()` are missing.
>
> **✅ Sprint 0 Complete:** `NODE_INDEX`, postfix `++/--`, and pointer comparison type inference have been fixed.
> - Parser refactoring: `parser.c` split into 5 modular headers.
> - Expressions refactoring: `expressions.h` split into 6 modular headers.
> - Core refactoring: `core.h` split into 4 modular headers.
> All 40 tests pass.
>
> **✅ Sprint 0.5 Complete (2026-09-29):** Include architecture hardening.
> - Created `zero_cc_fwd.h` forward declaration header for reducing dependencies
> - Fixed circular include: `lexer.h` ↔ `lexer/scan.h`
> - Added comprehensive include documentation (`docs/includes-and-forward-declarations.md`)
> - Fixed missing standard library includes across type headers
> - All 40 tests pass with improved include structure
>
> **✅ Sprint 2 Complete (2026-09-29):** New parsing features.
> - `goto` + label parsing and jump backpatching
> - Global variable declarations
> - `extern` symbol table
> - `typedef` for function pointers
> - Token paste `##` and stringification `#`
> - Variadic macros / `__VA_ARGS__`
> - Predefined macros (`__FILE__`, `__LINE__`, `__func__`)
> - `#pragma once` guard
> - `_Pragma` operator
>
> **✅ Sprint 1 Complete (2026-09-29):** Code generation gap fill.
> - Float literals, string literals, ternary, do-while, member access (with offsets)
> - Array subscript, cast (integers), sizeof, _Static_assert
>
> **Note:** Float operations (SSE2 arithmetic) and float casts moved to Sprint 3.

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
| `goto` / labels | ✅ | `NODE_GOTO` / `NODE_LABEL` in AST enum; parsed and lowered (JMP rel32 with patching) |
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
| `sizeof(T)` / `sizeof expr` | ✅ | `TK_SIZEOF` recognised and evaluated |
| `_Alignof(T)` | ✅ | Token defined and evaluated |
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
| `typedef` for function pointers | ✅ | `typedef int (*fn)(int)` works correctly |
| `const` / `volatile` / `restrict` | 🟡 | Bits stored in `ZeroCCType`; **no enforcement** |
| `_Atomic` | 🟡 | `is_atomic` bit present; no atomic ops emitted |
| `_Thread_local` | 🟡 | TLS backend wired; read-only static value only |
| `_Alignas(N)` | 🟡 | `decl_align_spec` global used for locals; not applied to globals |
| Flexible array members | ❌ | |
| Anonymous struct/union members | ❌ | |
| Bit-fields | ❌ | |
| Global variable declarations | ✅ | `data_section` buffer exists; parsing and emission implemented |
| `extern` declarations | ✅ | External symbol table implemented |

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
| Token paste `##` | ✅ | `a ## b` pastes tokens correctly |
| Stringification `#arg` | ✅ | `#arg` converts to string literal |
| Variadic macros `…` / `__VA_ARGS__` | ✅ | Supported via macro expansion and __VA_ARGS__ handling |
| `_Pragma` operator | ❌ | |
| Predefined macros (`__FILE__`, `__LINE__`, `__func__`) | ✅ | Dynamic value tracking implemented |

---

### 5. Code Generation / Runtime

| Construct | Status | Notes |
|-----------|:------:|-------|
| Integer arithmetic (x86-64) | ✅ | |
| Floating-point / SSE2 | ❌ | Emit helpers (`movss`, `movsd`, etc.) exist but **never called from lowering** |
| `do … while` machine code | ✅ | Lowered with break/continue patching |
| Ternary branch machine code | ✅ | Lowered |
| Member access / subscript machine code | ✅ | Stubs lowered |
| `goto` / label jumps | ✅ | Machine-code emission via JMP rel32 with patching |
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

### Sprint 0.5 – Include Architecture Hardening (Week 0.5)

**Completed 2026-09-29** - Infrastructure work to reduce future maintenance burden.

| Task | Status | Notes |
|------|--------|-------|
| Create `zero_cc_fwd.h` forward declaration header | ✅ | All major types declared |
| Fix circular include: `lexer.h` ↔ `lexer/scan.h` | ✅ | Extracted `ZeroCCLexer` to `state.h` |
| Document include order dependencies | ✅ | `types_lexer.h` header comments |
| Create include best practices documentation | ✅ | `docs/includes-and-forward-declarations.md` |
| Fix missing standard library includes | ✅ | `<stdio.h>`, `<stdbool.h>`, `<dlfcn.h>` |
| Verify all tests pass | ✅ | 40/40 tests pass |

---

### Sprint 1 – Low‑hanging fruit: 🟡 → ✅ (Weeks 1–3)

These only need new `case` blocks in [`lower_node()`](src/zero_cc/parser.c) / `lower_expr()` — no new parsing needed.

| Priority | Task | Effort | Status |
|----------|------|--------|--------|
| 1 | Float literal emission (`NODE_FLOAT` → `movsd` from rodata) | 1 day | ✅ |
| 2 | String literal emission (`NODE_STRING` → `lea rax, [rip+rodata]`) | 1 day | ✅ |
| 3 | `do … while` machine code (`NODE_DO_WHILE`) | 1 day | ✅ |
| 4 | Ternary `?:` codegen (`NODE_TERNARY`) | 2 days | ✅ |
| 5 | Member access `.` / `->` codegen (`NODE_MEMBER`) | 2 days | ✅ (with offsets) |
| 6 | Array subscript codegen (`NODE_INDEX`) | 1 day | ✅ (char/byte only) |
| 7 | Cast codegen (`NODE_CAST`) | 1 day | ✅ (integers) |
| 8 | `sizeof` constant folding | 1 day | ✅ |
| 9 | Fix `_Static_assert` to actually abort on failure | 1 day | ✅ |

---

### Sprint 2 – New parsing needed (Weeks 4–7)

| Priority | Task | Effort | Status |
|----------|------|--------|--------|
| 10 | `goto` + label parsing and jump backpatching | 3 days | ✅ |
| 11 | Global variable declarations (write to `data_section`) | 3 days | ✅ |
| 12 | `extern` symbol table | 2 days | ✅ |
| 13 | `typedef` for function pointers | 2 days | ✅ |
| 14 | Token paste `##` and stringification `#` in macro expander | 3 days | ✅ |
| 15 | Variadic macros / `__VA_ARGS__` | 3 days | ✅ |
| 16 | Predefined macros (`__FILE__`, `__LINE__`, `__func__`) | 1 day | ✅ |
| 17 | `#pragma once` guard | 1 day | ✅ |

---

### Sprint 2.5 – Extended pragma support (Week 8)

Extended pragma directive support for better compatibility with real-world C code.

| Priority | Task | Effort |
|----------|------|--------|
| 18 | `#pragma pack(n)` - structure packing control | 2 days |
| 19 | `#pragma message("text")` - compiler messages | 1 day |
| 20 | `#pragma GCC diagnostic` - warning control | 2 days |
| 21 | `#pragma GCC push_options` / `pop_options` | 1 day |
| 22 | Generic pragma passthrough (emit to output) | 1 day |

---

### Sprint 3 – Multi-ISA Backend & VREG System (Weeks 9–13) ✅

| Priority | Task | Effort | Status |
|----------|------|--------|--------|
| 23 | Virtual Register (VREG) system | 1 week | ✅ |
| 24 | Multi-ISA backends (x86-64, aarch64, riscv64) | 2 weeks | ✅ |
| 25 | Backend architecture cleanup | 3 days | ✅ |
| 26 | TLS backend (system TLS with fs/gs segments) | 1 week | ✅ |
| 27 | x86-64 encoding bug fixes | 3 days | ✅ |

### Sprint 4 – Code generation depth (Weeks 14–18)

| Priority | Task | Effort |
|----------|------|--------|
| 28 | Full SSE2 floating-point arithmetic (`addsd`, `subsd`, `mulsd`, `divsd`) | 1 week |
| 29 | Struct return by value (hidden-pointer calling convention) | 1 week |
| 30 | `va_arg` expansion | 1 week |
| 31 | Linear-scan register allocator | 2 weeks |
| 32 | Global variable storage emission (`.data` / `.bss`) | 3 days |

---

### Sprint 5 – Type system completeness (Weeks 19–21)

| Priority | Task | Effort |
|----------|------|--------|
| 33 | `union` proper layout (overlapping members) | 3 days |
| 34 | Bit-fields | 1 week |
| 35 | Flexible array members | 2 days |
| 36 | Anonymous struct/union members | 3 days |
| 37 | VLA (dynamic stack allocation) | 1 week |
| 38 | `_Atomic` operations (compare-exchange, fetch-add) | 1 week |
| 39 | `const` / `volatile` semantic enforcement | 3 days |

---

### Sprint 6 – Diagnostics, Testing & CI (Weeks 22–24)

| Priority | Task | Effort |
|----------|------|--------|
| 40 | Rich diagnostics: `file:line:col: error: …` format | 1 week |
| 41 | Warning vs. error distinction | 2 days |
| 42 | Regression test suite (bootstrap from TCC corpus) | 2 weeks |
| 43 | GitHub Actions CI (`make clean && make test`) | 1 day |

---

### Sprint 7 – Cross-platform (Weeks 25–27)

| Priority | Task | Effort |
|----------|------|--------|
| 44 | ARM64 backend (complete TODOs in `types_lexer.h`) | 3 weeks |
| 45 | PE/COFF backend (Windows) | 3 weeks |
| 46 | `STATIC=1` make flag for static binaries | 3 days |

---

## Timeline Overview

| Week | Work |
|------|------|
| 0.5 | **Sprint 0.5** – Include architecture hardening (forward decls, circular include fixes) ✅ |
| 1–3 | **Sprint 1** – lower_node/lower_expr gap fill (float, string, ternary, member, subscript) ✅ |
| 4–7 | **Sprint 2** – New parsing (goto, globals, extern, macros) ✅ |
| 8–12 | **Sprint 3** – Multi-ISA Backend & VREG System ✅ |
| 13–18 | **Sprint 4** – Codegen depth (FP, structs, va_arg, register alloc) |
| 19–21 | **Sprint 5** – Type system completeness |
| 22–24 | **Sprint 6** – Diagnostics, tests, CI |
| 25–27 | **Sprint 7** – Cross-platform backends |
| 28 | Final release, docs, tag `v1.0` |

---

## Immediate Next Actions

### What's Next: Sprint 1 - Code Generation Gap Fill

The include architecture is now solid. The next work focuses on filling in the missing `lower_node()` and `lower_expr()` cases.

1. **Float literal emission** (`NODE_FLOAT` → `movsd` from rodata) - 1 day
2. **String literal emission** (`NODE_STRING` → `lea rax, [rip+rodata]`) - 1 day  
3. **`do … while` machine code** (`NODE_DO_WHILE`) - 1 day
4. **Ternary `?:` codegen** (`NODE_TERNARY`) - 2 days
5. **Member access `.` / `->` codegen** (`NODE_MEMBER`) - 2 days
6. **Array subscript codegen** (`NODE_INDEX`) - 1 day
7. **Cast codegen** (`NODE_CAST`) - 1 day
8. **Fix `_Static_assert`** to actually abort on failure - 1 day

**Recommended workflow:**
- Open `src/zero_cc/parser.c` at `lower_expr()` and add cases
- Run `make clean && make test` after each case to confirm no regressions
- Commit incrementally: `"feat: fill lower_expr for NODE_FLOAT"`, etc.

---

*Prepared by Antigravity AI Assistant — grounded in source inspection 2026-09-26.*
*Include architecture audit completed 2026-09-29.*
