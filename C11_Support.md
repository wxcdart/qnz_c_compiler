# zero_cc — C11 Support Plan

This document tracks the plan, milestones, and status for bringing **C11**
language support to `zero_cc`. It is the companion to the test suite in
`test/` and the `Makefile` used to build/run those tests.

The long‑term goal is: once zero_cc understands a useful subset of C11, we
will write a thin self‑hosting driver (`zcc.c`) that uses zero_cc's own
JIT/ELF back‑ends to compile C source — including, eventually, itself.

**Do not attempt self‑hosting before this plan is complete.** Real‑world C
code (and the zero_cc sources themselves) rely on C11 features such as
`_Static_assert`, `_Alignas`, and `_Atomic`.

---

## 0. Ground Rules

* Every milestone must:
  1. Add/extend a test in `test/` that exercises the new feature.
  2. Keep `make test` fully green (all existing tests + the new one).
  3. Be committed/snapshotted independently so we can bisect regressions.
* Prefer the smallest change that makes the test pass; avoid speculative
  generality.
* Where the standard allows an implementation‑defined shortcut for a
  freestanding/JIT compiler (e.g., `_Thread_local` → `static`), document the
  limitation in this file and in `README.md`.

---

## 1. Feature Matrix

| # | Feature | Token(s) already added | Status | Test file |
|---|---------|-------------------------|--------|-----------|
| 1 | `_Static_assert` | `TK_STATIC_ASSERT` | ✅ done (statement scope; file/struct scope pending) | `test/static_assert.c` |
| 2 | `_Alignas` / `_Alignof` | `TK_ALIGNAS`, `TK_ALIGNOF` | ✅ done | `test/align.c` |
| 3 | `_Atomic` | `TK_ATOMIC` | ✅ done | `test/atomic.c`, `test/atomic_jit.c` |
| 4 | `_Noreturn` | `TK_NORETURN` | ⬜ not started | `test/noreturn.c` |
| 5 | `_Thread_local` | `TK_THREAD_LOCAL` | ⬜ not started | `test/tls.c` |
| 6 | `_Generic` (generic selection) | `TK_GENERIC` (pre‑existing) | ⬜ not started | `test/generic.c` |
| 7 | `__has_include` | n/a (preprocessor operator) | ⬜ not started | `test/has_include.c` |
| 8 | Convenience headers (`<stdalign.h>`, `<stdnoreturn.h>`, `<assert.h>` w/ `static_assert`) | n/a | ⬜ not started | covered by tests above once headers exist |
| — | `true` / `false` / `nullptr` as keywords | `TK_TRUE`, `TK_FALSE`, `TK_NULLPTR` | ✅ done (tokenizer + builtin macro cleanup) | implicit in all tests |
| — | `__STDC_VERSION__` | n/a | ✅ set to `202311L`; will add a way to target `201112L` for strict C11 mode if needed | n/a |

Legend: ✅ done · 🟡 in progress / partial · ⬜ not started

---

## 2. Milestones

### Milestone 1 — `_Static_assert`

**Goal:** `_Static_assert(constant-expression, "message");` (and the C23
one‑argument form `_Static_assert(constant-expression);`) is fully checked
at compile time.

**Work items**
- [x] Tokenizer recognizes `_Static_assert` → `TK_STATIC_ASSERT`.
- [x] Parser stub consumes `_Static_assert ( expr [, "msg"] ) ;` at
      statement scope (`statements.h`).
- [x] Parser also accepts `_Static_assert` at **file scope** (top level, in
      `zero_cc_compile_c99` in `jit_linker.h`) and inside `struct` bodies.
- [x] Implement a **constant‑expression evaluator** (`zero_cc_eval_const_expr`)
      that can evaluate a subset of expressions at compile time:
      integer literals, `sizeof`, `_Alignof`, arithmetic/bitwise/logical
      operators, and named `enum` constants.
- [x] If the evaluated expression is `0`, emit a compiler error including
      the message (or a default message for the C23 one‑arg form) and set
      an error flag on `ZeroCCCompiler` (add `bool had_error;` +
      `char error_msg[256];`).
- [x] `zero_cc_compile_c99` / callers check `c->had_error` and abort/return
      non‑zero.

**Test:** `test/static_assert.c` — all assertions pass:
- `_Static_assert(sizeof(int) == 4)` — basic constant expression
- `_Static_assert(_Alignof(int) == 4)` — `_Alignof` in constant context
- `_Static_assert` inside `typedef struct { ... }` — struct scope
- `_Static_assert` at file scope — global scope
- Optional message string: `_Static_assert(expr, "message")`
- C23 one-argument form: `_Static_assert(expr)`
- Error reporting: sets `c->had_error` and `c->error_msg` on failure

**Test:** `test/static_assert.c` — all assertions pass.

**Negative test:** `test/static_assert_fail.c` — verifies failing `_Static_assert` is caught:
- Compiles code with `_Static_assert(0, "This should fail");` via zero_cc API
- Verifies `c->had_error` is set
- Verifies `c->error_msg` contains meaningful error
- [x] Negative test implemented and passes

---

### Milestone 2 — `_Alignas` / `_Alignof`

**Goal:** alignment can be queried (`_Alignof`) and overridden (`_Alignas`)
for variables and struct members.

**Work items**
- [x] `ZeroCCType.align` is correctly computed for all base types
      (`char`=1, `short`=2, `int`/`float`=4, `long`/`double`/pointers=8).
- [x] Parser: recognize `_Alignas(type)` and `_Alignas(constant-expr)` in
      the declaration‑specifier sequence; store the resulting alignment on
      the type/symbol being declared.
- [x] Parser: `_Alignof(type)` / `_Alignof(expr)` as a unary operator in
      `expressions.h`, producing an integer constant (mirrors `sizeof`).
- [x] Struct layout (`typedef struct { ... }` handling in `statements.h`)
      rounds each member's offset up to that member's alignment, and rounds
      the final struct size up to the struct's own alignment (max of
      members' alignments, or the `_Alignas` override).
- [x] Local variable allocation (`zero_cc_add_local` /
      `zero_cc_emit_prologue`) respects `_Alignas` overrides on the stack.
- [x] **Type-only expression parser** for `_Alignof(expr)` / `sizeof(expr)`:
      full recursive-descent parser returning `ZeroCCType` (mirrors value
      parser) supporting member access (`.`, `->`), array indexing (`[]`),
      pointer dereference (`*`), address-of (`&`), casts, parentheses,
      and literals.
- [x] Constant evaluator (`eval_alignof`, `eval_sizeof`) uses type-only parser.
- [x] Code generator (`zero_cc_parse_unary`) uses type-only parser.

**Test:** `test/align.c` — all assertions pass:
- `_Alignof(int)` = 4, `_Alignof(long long)` = 8
- `_Alignof(S)` = 16 (struct with `_Alignas(16)` member)
- `offsetof(S, x)` = 0, `sizeof(S)` = 16
- `_Alignof(int[10])` = `_Alignof(int)`
- `int _Alignas(32) y;` → `_Alignof(y)` = 32
- `_Alignof(s.x)` = 16 (member access on variable)
- `_Alignof(p->x)` = 16 (pointer member access)
- `_Alignof(S.x)` = 16 (member access on type name)
- `sizeof(s.x)` = 4, `sizeof(S.x)` = 4
- `_Static_assert(_Alignof(s.x) == 16)` works in constant context

---

### Milestone 3 — `_Atomic`

**Goal:** `_Atomic T` parses, has correct `sizeof`, and simple atomic
builtins (`__atomic_load_n`, `__atomic_add_fetch`, …) compile and run.

**Work items**
- [x] Add `is_atomic` flag (or `TYPE_ATOMIC` wrapper) to `ZeroCCType`.
- [x] Parser: accept `_Atomic int x;` and `_Atomic(int) x;` forms in
      decl‑specifiers.
- [x] `sizeof`/`_Alignof` of an atomic type equal that of the underlying
      type (no padding added).
- [x] Recognize the built‑in functions used by `<stdatomic.h>`-style code:
      `__atomic_load_n`, `__atomic_store_n`, `__atomic_add_fetch`,
      `__atomic_fetch_add`, `__atomic_exchange_n`,
      `__atomic_compare_exchange_n` — implement as normal function calls
      that resolve via `dlsym`/JIT to the host's `libatomic`/`libgcc`
      implementations (same mechanism already used for other libc calls).
- [x] Decide & document policy for ELF‑emitted (non‑JIT) atomics: for
      Milestone 3 we only guarantee atomics work under `zero_cc_execute_jit`;
      ELF object/exe emission may leave a TODO note if the relocation isn't
      implemented yet.

**Current status:** 
- `_Atomic` parsing, `sizeof`/`_Alignof`, and self-hosted builtin lowering for `__atomic_load_n`, `__atomic_store_n`, `__atomic_add_fetch` and memory-order constants are complete. The compiler is self-hostable for Milestone 3.
- Pointer l-value code generation for `*p = expr` is implemented.
- ELF object/executable emission emits atomic types with the same layout as the underlying type; true atomic instruction selection / relocations for ELF remains a future task.

**Test:** `test/atomic.c` (already written) — extend with:
- [x] A JIT‑run test (`test/atomic_jit.c`) that verifies compilation of
      `_Atomic` types, `sizeof(_Atomic T)`, and `_Static_assert` for atomic
      sizes.

---

### Milestone 4 — `_Noreturn`

**Goal:** `_Noreturn` parses on function declarations/definitions and is
recorded, enabling future optimizations/diagnostics.

**Work items**
- [x] Add `bool noreturn;` to `ZeroCCFunction`.
- [x] Parser: accept `_Noreturn` before/after other function specifiers
      (`static`, `inline`) in `zero_cc_compile_function` (`jit_linker.h`).
- [ ] (Optional, stretch) warn if a `_Noreturn` function's body can fall off
      the end without calling a function itself marked `_Noreturn` or
      `exit`/`abort`.

**Test:** `test/noreturn.c` (already written) — passes once parsing accepts
the keyword without falling through to "General Expression Statement" and
misparsing.

---

### Milestone 5 — `_Thread_local`

**Goal:** `_Thread_local` parses; for this freestanding/JIT compiler we
implement it as **Policy A**: alias to `static` storage (single instance,
documented limitation — no real per‑thread storage).

**Work items**
- [x] Parser: accept `_Thread_local` as a storage-class specifier in variable declarations, skipped before type parsing.
- [x] Code‑gen: variables declared with `_Thread_local` are emitted as normal locals/globals — same semantics as `static` for this JIT implementation.
- [x] Document the limitation in `README.md` under "Known Limitations".
- [ ] (Future/Policy B, not in this milestone) real TLS via
      `pthread_key_create`/`__thread` — tracked as a follow‑up item, not
      required for C11 baseline.

**Test:** `test/tls.c` — passes with the `static` alias semantics.

---

### Milestone 6 — `_Generic` (Generic Selection)

**Goal:** `_Generic(controlling-expr, type: expr, ..., default: expr)`
selects the correct association based on the type of the controlling
expression, at compile time, without evaluating the controlling expression.

**Work items**
- [x] Add `TK_GENERIC` handling to `expressions.h` (primary expression
      parsing).
- [x] Parse the controlling expression *for its type only* via
      `zero_cc_type_of_equality`, discarding generated code.
- [x] Parse the association list: `type-name : assignment-expression` pairs
      and at most one `default : assignment-expression`.
- [x] Simplified type matching for base types, pointers, and float.
- [x] Select the first matching association (or `default` if none match)
      and emit only that expression.

**Test:** `test/generic.c` — passes with GCC compilation. Zero_cc JIT
supports `_Generic` for basic int/float/pointer selections.

---

### Milestone 7 — `__has_include`

**Goal:** `#if __has_include(<header.h>)` / `#if __has_include("header.h")`
works in the preprocessor.

**Work items**
- [ ] Extend `zero_cc_cpp_eval_expr` (in `core.h`) to special‑case the
      `__has_include ( ... )` token sequence before generic expression
      evaluation (similar to how `defined(...)` is special‑cased).
- [ ] Resolve the header name using the existing include‑search logic
      (`zero_cc_cpp_resolve_include`) plus the virtual‑header table
      (`zero_cc_cpp_get_virtual_header`) — a header "exists" if either a
      real file is found on the include path **or** a virtual header with
      that name is registered.
- [ ] Return `1`/`0` accordingly into the `#if` expression evaluator.

**Test:** `test/has_include.c` (already written).

---

### Milestone 8 — Convenience Headers

**Goal:** the friendly, non‑underscore spellings from the standard headers
are available.

**Work items**
- [ ] Add virtual header `<stdalign.h>`:
      `#define alignas _Alignas` / `#define alignof _Alignof` (C11); note
      that in C23 these become keywords directly and the macros are for
      backward compatibility only — harmless to keep defined.
- [ ] Add virtual header `<stdnoreturn.h>`: `#define noreturn _Noreturn`.
- [ ] Extend virtual header `<assert.h>` (create if it doesn't exist) with
      `#define static_assert _Static_assert`.
- [ ] Add virtual header `<stdatomic.h>` (minimal): typedefs like
      `atomic_int` as `_Atomic int`, plus macros for the memory‑order
      constants (`memory_order_relaxed`, `memory_order_seq_cst`, …) mapped
      to the `__ATOMIC_*` builtin constants already used in `test/atomic.c`.

**Test:** update `test/align.c`, `test/noreturn.c`, `test/atomic.c`,
`test/static_assert.c` to optionally `#include` the friendly headers and
use the non‑underscore spellings, verifying both spellings compile
identically.

---

## 3. Suggested Schedule

| Week | Milestone |
|------|-----------|
| 1 | Milestone 1 – `_Static_assert` (incl. constant‑expr evaluator) ✅ |
| 2 | Milestone 2 – `_Alignas` / `_Alignof` ✅ |
| 3 | Milestone 3 – `_Atomic` |
| 4 | Milestone 4 – `_Noreturn` |
| 5 | Milestone 5 – `_Thread_local` |
| 6 | Milestone 6 – `_Generic` |
| 7 | Milestone 7 – `__has_include` |
| 8 | Milestone 8 – convenience headers + docs polish |
| 9–10 | Buffer: fix regressions, expand negative/error tests, update `README.md`, prototype the self‑hosting driver (`zcc.c`) |

Milestones may be combined or reordered if a dependency is discovered (for
example, Milestone 6's `_Static_assert`-based tests depend on Milestone 1's
constant evaluator).

---

## 4. Definition of Done (for this plan)

C11 support is considered "baseline complete" when:

1. All feature rows in the matrix (§1) are ✅.
2. `make test` passes with zero failures, including negative tests that
   confirm errors are actually raised (e.g., a failing `_Static_assert` is
   caught, not ignored).
3. `README.md`'s "C11/C23 Support" section is updated to reflect actual
   (not aspirational) status, with any implementation‑defined shortcuts
   (e.g., `_Thread_local` → `static`) explicitly called out under a "Known
   Limitations" heading.
4. A first draft of the self‑hosting driver (`zcc.c`) exists and can
   successfully compile at least one non‑trivial `.c` file using only
   zero_cc's public API.

---

## 5. GNU11 Extensions Track

Zero_cc already ships several GNU C extensions used by existing tests. These are tracked separately from the C11 baseline to avoid scope creep:

| # | GNU Feature | Status | Notes |
|---|-------------|--------|-------|
| G1 | Statement expressions `({ ... })` | ✅ done | Supported via parser and codegen |
| G2 | `__attribute__((...))` / `__attribute__` | ✅ done | Parsed as no-op, builtin macro defined |
| G3 | `__extension__` | ✅ done | Preprocessor no-op macro |
| G4 | `__typeof__` / `typeof` | ✅ done | Tokenizer + type-only parser |
| G5 | `__builtin_va_start` / `__builtin_va_arg` / `__builtin_va_end` | ✅ done | Self-hosted macros |
| G6 | `__attribute__` variadic forms | ✅ done | Macro defined as empty |
| G7 | `__restrict` / `__restrict__` | ✅ done | Object macros |
| G8 | Compound literals | ⬜ not started | |
| G9 | `__asm__` / inline asm | ⬜ not started | |
| G10 | `__builtin_memcpy` / other builtins | ⬜ not started | |

GNU extensions are maintained in `core.h` builtin macros and the parser; they are not part of the C11 baseline but are required for self-hosting the compiler.

## 6. Out of Scope (tracked separately, not required for C11 baseline)

- Full C23 feature set (`typeof`/`typeof_unqual` semantics, `#embed`,
  `__VA_OPT__`, UTF‑8 identifiers, `_BitInt(N)`).
- Real per‑thread storage for `_Thread_local` (Policy B).
- Atomic code generation for the ELF object/executable emitters (only the
  JIT path is guaranteed in Milestone 3).
- Cross‑compilation (zero_cc remains host‑architecture‑only: x86_64/ARM64).
