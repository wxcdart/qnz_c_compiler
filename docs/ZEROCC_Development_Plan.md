# ZeroCC – Feature‑Completion Roadmap & C99 Gap Analysis

> Last updated: 2026-10-02. Findings are **grounded in actual source inspection** of
> [`parser.c`](../src/zero_cc/parser.c), [`types_lexer.h`](../src/zero_cc/types_lexer.h),
> [`lexer.c`](../src/zero_cc/lexer.c), [`ast_lowering.h`](../src/zero_cc/parser/ast_lowering.h),
> and the multi-ISA backend implementations in [`src/zero_cc/isa/`](../src/zero_cc/isa/).

## Goal: Self-Hosting C11 Compiler

ZeroCC is designed to eventually **self-host** (compile itself). Architecture supports:

- **Modular design**: Separate source files, not single-header
- **Multi-ISA**: x86-64, aarch64, riscv64 backends
- **C11 conformance**: TLS, Atomic, Alignment, Generic selection
- **Cross-platform**: Linux, Windows, macOS, baremetal

---

## Current Status Summary

| Category | ✅ Done | 🟡 Partial (parsed, not fully lowered/enforced) | ❌ Missing |
|----------|:-------:|:-----------------------------------------------:|:---------:|
| Statements | 10 | 0 | 0 |
| Expressions | 22 | 2 | 1 |
| Types & Declarations | 16 | 3 | 2 |
| Preprocessor | 11 | 0 | 0 |
| Code Generation / Runtime | 10 | 3 | 0 |
| Diagnostics | 0 | 1 | 2 |
| **Total Constructs** | **69** | **9** | **5** |

### Sprint Progress Summary

| Sprint | Description | Status | Test Suite |
|---|---|:---:|:---:|
| **Sprint 0 / 0.5** | Include architecture hardening, forward declarations, modularization | ✅ Complete | 40/40 pass |
| **Sprint 1** | Code generation gap fill (floats, strings, ternary, member, subscript, casts) | ✅ Complete | 55/55 pass |
| **Sprint 2 / 2.5** | New parsing (goto, globals, extern, macros, pragmas) | ✅ Complete | 70/70 pass |
| **Sprint 3** | Multi-ISA Backend & VREG System, System TLS (`fs`/`gs`) | ✅ Complete | 85/85 pass |
| **Sprint 4** | Codegen depth (SSE2 FP arithmetic & calling conv, struct return by value, `va_arg`) | ✅ Complete | 105/105 pass |
| **Frontier 1 & 2** | Sealed backend abstraction leaks, full SSE2 FP ABI, generalised x86-64 emission | ✅ Complete | **111/111 pass** |
| **Sprint 5** | Type system completeness (`union` layout ✅, flexible arrays ✅, bit-fields ✅, VLAs, atomics) | 🟡 In Progress | **112/112 pass** |
| **Sprint 5.5** | Codebase hardening, test harness modernization, ASan/UBSan audit, JIT W^X | 📅 Scheduled | — |
| **Sprint 6** | Industrial-grade testing (GCC torture, Csmith fuzzing, rich diagnostics) | 📅 Scheduled | — |
| **Sprint 7** | Cross-platform backends (AArch64, RISC-V, Windows PE/COFF) | 📅 Scheduled | — |
| **Sprint 8** | Two-pass IR & LuaJIT-inspired optimization pipeline (-O0 to -O3) | 📅 Scheduled | — |

---

## Detailed Gap Analysis

### Legend
| Symbol | Meaning |
|--------|---------|
| ✅ | Parsed **and** lowered to machine code |
| 🟡 | AST node / construct exists, but lowering or enforcement is partial |
| ❌ | Neither parsed nor lowered |

---

### 1. Statements

| Construct | Status | Notes |
|-----------|:------:|-------|
| `if / else` | ✅ | Full parse + lower |
| `while` | ✅ | Full parse + lower |
| `do … while` | ✅ | `NODE_DO_WHILE` lowered; break/continue patched |
| `for` | ✅ | Full parse + lower |
| `switch / case / default` | ✅ | Full parse + lower with jump table / branch chain |
| `break` | ✅ | Full jump patching |
| `continue` | ✅ | Full loop target jump patching |
| `return <expr>` | ✅ | Full (int, float in `%xmm0`, and struct returns) |
| `goto` / labels | ✅ | `NODE_GOTO` / `NODE_LABEL` parsed and lowered (JMP rel32 with backpatching) |
| `_Static_assert` | ✅ | Evaluated at compile-time; aborts compilation on assertion failure |

---

### 2. Expressions

| Construct | Status | Notes |
|-----------|:------:|-------|
| Integer literals | ✅ | 8, 16, 32, 64-bit signed and unsigned |
| Float literals | ✅ | `NODE_FLOAT` lowered to SSE2 `movsd` via rodata RIP-relative addressing |
| String literals | ✅ | `NODE_STRING` lowered via rodata + RIP-relative `lea rax, [rip+off]` |
| Bool literals `true`/`false` | ✅ | Full |
| `nullptr` | ✅ | Full |
| Identifiers / variables | ✅ | Full local stack offset and global data address resolution |
| Arithmetic `+ - * / %` | ✅ | Integer ALU and SSE2 floating-point arithmetic (`addsd`, `subsd`, `mulsd`, `divsd`) |
| Bitwise `& \| ^ ~` | ✅ | Full 32-bit and 64-bit bitwise ALU |
| Shift `<< >>` | ✅ | Arbitrary register shifts with `%cl` masking |
| Comparison `== != < <= > >=` | ✅ | Integer (`cmp` + `setcc`) and float (`ucomisd` + `setcc`) |
| Logical `&& \|\|` | ✅ | Short-circuit evaluation with branch patching |
| Unary `! ~ - & *` | ✅ | Full logical not, bitwise not, negate, address-of, and pointer dereference |
| `++ --` (pre/post) | ✅ | Register-independent increment/decrement with correct pre/post value semantics |
| Assignment `=` | ✅ | Memory and register assignment |
| Compound assignment `+= -= *= /= %= &= \|= ^= <<= >>=` | ✅ | All integer and floating-point compound assignments lowered |
| Ternary `? :` | ✅ | `NODE_TERNARY` lowered with conditional jumps |
| Comma operator `,` | 🟡 | Handled in function argument lists; expression-level `NODE_COMMA` partial |
| Cast `(T)expr` | ✅ | `NODE_CAST` lowered (int truncation/sign-extend, `cvtsi2sd`, `cvttsd2si`) |
| `sizeof(T)` / `sizeof expr` | ✅ | Compile-time constant evaluation |
| `_Alignof(T)` | ✅ | Compile-time constant evaluation |
| Member access `.` / `->` | ✅ | `NODE_MEMBER` lowered with struct/union member byte-offset arithmetic |
| Array subscript `a[i]` | ✅ | `NODE_INDEX` lowered with scaled base+index addressing |
| Function call | ✅ | Full System V AMD64 ABI calling convention (int regs `%rdi`–`%r9`, FP regs `%xmm0`–`%xmm7`, stack args, variadic `%al`) |
| Compound literals `(T){…}` | 🟡 | `NODE_COMPOUND_LIT` parsed; single-word lowering |
| Designated initializers `.field=val` | ❌ | No parse support |
| `_Generic(…)` | ✅ | Full C11 generic selection with type matching, pointer decay, and default branch |

---

### 3. Types & Declarations

| Construct | Status | Notes |
|-----------|:------:|-------|
| `int`, `char`, `void`, `float`, `double`, `_Bool` | ✅ | Full basic types |
| `short`, `long`, `long long` | ✅ | Mapped to fixed-width types |
| `unsigned` variants | ✅ | Full unsigned integer types |
| Pointers `*` | ✅ | Multi-level pointers with correct scaling |
| Arrays `T a[N]` (fixed size) | ✅ | Fixed-size array stack allocation and decay to pointers |
| **VLA** `T a[n]` (variable size) | ✅ | Dynamic stack allocation (`sub rsp, reg`), pointer decay, index/assign (`test/vla_test.c`) |
| `struct` (named, with members) | ✅ | Member offset alignment and padding |
| `union` | ✅ | Overlapping member layout, max alignment & size calculation |
| `enum` (simple, explicit values) | ✅ | Enum constants registered and resolved in symbol table |
| Named `enum` as a distinct type-tag | 🟡 | Enum values resolved; distinct type checking partial |
| `typedef` (scalar, struct) | ✅ | Typedef table resolution |
| `typedef` for function pointers | ✅ | `typedef int (*fn)(int)` parsed and callable |
| `const` / `volatile` / `restrict` | ✅ | Syntactic parsing, qualifier preservation, and semantic enforcement on variables, pointers, members, and structs (Sprint 5 Task 39, `test/const_volatile_test.c`) |
| `_Atomic` | ✅ | C11 `_Atomic` type, atomic ops (`lock cmpxchg`, `lock xadd`, `xchg`), and GCC/C11 `__sync`/`__atomic` builtins (Sprint 5 Task 38, `test/atomic_test.c`) |
| `_Thread_local` | ✅ | System TLS (`fs`/`gs` segment) lowered and verified (`test/tls.c`, `test/tls_jit.c`) |
| `_Alignas(N)` | ✅ | Alignment applied to variable declarations |
| Flexible array members | ✅ | Trailing zero-length arrays supported in structs (Sprint 5 Task 35) |
| Anonymous struct/union members | ✅ | C99/C11 recursive flattening, direct member access & nested bit-fields (Sprint 5 Task 36, `test/anon_struct_test.c`) |
| Bit-fields | ✅ | C99 bit-fields (`: width`) with storage-unit packing, masking, sign-extension, and compound assignment (Sprint 5 Task 34, `test/bitfield_test.c`) |
| Global variable declarations | ✅ | Emitted to `.data` and `.bss` with RIP-relative relocations |
| `extern` declarations | ✅ | External symbol table resolution |

---

### 4. Preprocessor

| Construct | Status | Notes |
|-----------|:------:|-------|
| `#include <…>` / `"…"` | ✅ | System and local include resolution with search paths |
| `#define FOO value` (object-like) | ✅ | Object macro substitution |
| `#define FOO(x) …` (function-like) | ✅ | Function-like macro expansion |
| `#ifdef / #ifndef / #if / #else / #endif` | ✅ | Conditional compilation |
| `#undef` | ✅ | Macro undefinition |
| `#pragma once` | ✅ | Header guard optimization |
| Token paste `##` | ✅ | Macro token concatenation |
| Stringification `#arg` | ✅ | Macro argument stringification |
| Variadic macros `…` / `__VA_ARGS__` | ✅ | Supported via macro expansion and `__VA_ARGS__` handling |
| `_Pragma` operator | ✅ | C99 `_Pragma("string")` preprocessor operator |
| Predefined macros (`__FILE__`, `__LINE__`, `__func__`) | ✅ | Dynamic value tracking implemented |

---

### 5. Code Generation / Runtime

| Construct | Status | Notes |
|-----------|:------:|-------|
| Integer arithmetic (x86-64) | ✅ | Full 32-bit and 64-bit integer ALU |
| Floating-point / SSE2 | ✅ | Complete SSE2 arithmetic (`addsd`, `subsd`, `mulsd`, `divsd`), comparison (`ucomisd`), and FP ABI |
| `do … while` machine code | ✅ | Lowered with break/continue patching |
| Ternary branch machine code | ✅ | Lowered with conditional jumps |
| Member access / subscript machine code | ✅ | Full member byte-offset calculation and scaled array indexing |
| `goto` / label jumps | ✅ | Machine-code emission via JMP rel32 with backpatching |
| Struct return by value | ✅ | AMD64 ABI hidden pointer (`%rdi`) for >16B, packed into `%rax`/`%rdx` for ≤16B |
| `va_start` / `va_arg` / `va_end` | ✅ | AMD64 ABI variadic save area, `%al` register setup, and `va_arg` expansion |
| Global variable storage (`.data` / `.bss`) | ✅ | Data section emission and RIP-relative data relocation |
| Register allocation | 🟡 | Linear-scan infrastructure in place (`regalloc.h`, `live_range.h`); stack spill default |
| ARM64 back-end | 🟡 | Target abstraction ready in `isa_backend.h`; full instruction selection planned for Sprint 7 |
| RISC-V back-end | 🟡 | Target abstraction ready in `isa_backend.h`; full instruction selection planned for Sprint 7 |
| `libc` symbol resolution | ✅ | Dynamic symbol resolution via `dlsym(RTLD_DEFAULT)` at JIT execution |

---

### 6. Diagnostics

| Construct | Status | Notes |
|-----------|:------:|-------|
| Error messages with source location | 🟡 | Line/column tracked; caret formatting planned in Sprint 5.5 Task 44 |
| Macro expansion notes | ❌ | Planned in Sprint 6 |
| Warning vs. error distinction | ❌ | All diagnostics currently treated as hard errors; planned in Sprint 6 Task 45 |

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
| 5 | Member access `.` / `->` codegen (`NODE_MEMBER`) | 2 days | ✅ |
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

### Sprint 4 – Code generation depth (Weeks 14–18) ✅ COMPLETE

| Priority | Task | Effort | Status |
|----------|------|--------|--------|
| 28 | Full SSE2 floating-point arithmetic (`addsd`, `subsd`, `mulsd`, `divsd`) | 1 week | ✅ |
| 29 | Struct return by value (hidden-pointer calling convention) | 1 week | ✅ |
| 30 | `va_arg` expansion | 1 week | ✅ |
| 31 | Linear-scan register allocator | 2 weeks | 🟡 Partial |
| 32 | Global variable storage emission (`.data` / `.bss`) | 3 days | ✅ |

---

### Sprint 5 – Type system completeness (Weeks 19–21)

| Priority | Task | Effort | Status |
|----------|------|--------|--------|
| 33 | `union` proper layout (overlapping members) | 3 days | ✅ Complete |
| 34 | Bit-fields (`: width` bit-packing & mask/shift operations) | 1 week | |
| 35 | Flexible array members (trailing zero-length struct arrays) | 2 days | ✅ Complete |
| 36 | Anonymous struct/union members | 3 days | |
| 37 | VLA (dynamic stack allocation) | 1 week | |
| 38 | `_Atomic` operations (compare-exchange, fetch-add) | 1 week | |
| 39 | `const` / `volatile` semantic enforcement | 3 days | |

---

### Sprint 5.5 – Codebase Hardening, Cleanup & Sanitization (Weeks 22–23)

> **Strategic Priority**: Clean up technical debt, sanitize memory, and structure the test harness before subjecting ZeroCC to heavy industrial test suites (GCC torture & Csmith).

| Priority | Task | Effort | Status |
|----------|------|--------|--------|
| 40 | **Architecture Cleanup & Dead Code Elimination**: Remove legacy stubs, unify `compile.h`/`ast_lowering.h` pathways, and eliminate remaining hacky fallbacks | 4 days | |
| 41 | **Test Suite Restructuring & Harness Modernization**: Organize 111 flat tests into structured categories (`test/unit/`, `test/jit/`, `test/abi/`, `test/semantic/`); implement unified runner with timing, pass/fail counts, and TAP/JUnit output | 3 days | |
| 42 | **Memory Leak & Sanitizer Audit**: Clean run of entire test suite under AddressSanitizer (`-fsanitize=address,undefined`) and Valgrind; fix AST arena and string buffer leaks | 4 days | |
| 43 | **JIT W^X Memory Hardening**: Transition from `PROT_READ|PROT_WRITE|PROT_EXEC` to strict W^X (`mprotect` write-then-execute) for security and real-world OS compliance | 2 days | |
| 44 | **Diagnostic & Header Sanitization**: Clean up header dependencies, eliminate redundant includes, and standardize error reporting with file:line:col caret formatting | 3 days | |

---

### Sprint 6 – Industrial-Grade Testing, Fuzzing & Conformance (Weeks 24–26)

| Priority | Task | Effort | Status |
|----------|------|--------|--------|
| 45 | Rich diagnostics: `file:line:col: error: …` format and warning vs error distinction | 1 week | |
| 46 | GCC C-Torture integration (run `gcc.c-torture/execute` ~1,500 tests against ZeroCC JIT) | 1 week | |
| 47 | Differential Fuzzing: Csmith & yarpgen harness comparing ZeroCC execution results vs GCC/Clang | 1 week | |
| 48 | Regression test suite (bootstrap from TCC corpus) | 1 week | |
| 49 | GitHub Actions CI (`make clean && make test` + ASan/UBSan matrix) | 2 days | |

---

### Sprint 7 – Cross-platform & Multi-Architecture Expansion (Weeks 27–29)

| Priority | Task | Effort | Status |
|----------|------|--------|--------|
| 50 | ARM64 backend (complete TODOs in `types_lexer.h` and `isa/aarch64.c`) | 3 weeks | |
| 51 | RISC-V 64 backend completion | 2 weeks | |
| 52 | PE/COFF backend (Windows) | 3 weeks | |
| 53 | `STATIC=1` make flag for static binaries | 3 days | |

---

### Sprint 8 – Two-Pass IR & LuaJIT-Inspired Optimization Pipeline (Weeks 30–33)

> **Strategic Priority**: Transition from direct AST-to-machine-code lowering to a full Two-Pass IR architecture with linear-scan register allocation and multi-tier optimization passes (-O0 through -O3), drawing high-throughput design patterns from LuaJIT's linear IR and folding engine. Reference: [`docs/2_pass_plan.md`](2_pass_plan.md) and [`docs/TWO_PASS_ARCHITECTURE.md`](TWO_PASS_ARCHITECTURE.md).

| Priority | Task | Effort | Status |
|----------|------|--------|--------|
| 54 | **Complete Pass 1 (AST → IR)**: Expand `ir_emit_ops.h` and migrate remaining AST statement/expression nodes to IR; eliminate dual-emission pathway | 1 week | 🟡 Partial |
| 55 | **Register Allocator Hardening**: Fix live-interval computation in `live_range.h`; robust linear-scan assignment with callee/caller register spilling in `regalloc.h` | 1 week | 🟡 Partial |
| 56 | **Pass 2 Machine Code Emission**: Complete `emit_ir_to_machine_code()` using target `isa_backend.h` interfaces (x86-64, AArch64, RISC-V) | 1 week | |
| 57 | **LuaJIT-Style Middle-End Optimizations (-O1 / -O2)**: Fast linear-pass constant folding, algebraic simplification, dead-code elimination (DCE), and common subexpression elimination (CSE) | 1 week | |
| 58 | **Loop & Control-Flow Optimizations (-O3)**: Loop-invariant code motion (LICM), strength reduction (multiplication to shifts/adds), and branch simplification | 1 week | |
| 59 | **Optimization Benchmarking & Differential Verification**: Benchmark compilation speed and execution throughput against GCC/Clang across optimization levels (-O0 to -O3) | 3 days | |

---

## Timeline Overview

| Week | Work |
|------|------|
| 0.5 | **Sprint 0.5** – Include architecture hardening (forward decls, circular include fixes) ✅ |
| 1–3 | **Sprint 1** – lower_node/lower_expr gap fill (float, string, ternary, member, subscript) ✅ |
| 4–7 | **Sprint 2** – New parsing (goto, globals, extern, macros) ✅ |
| 8–12 | **Sprint 3** – Multi-ISA Backend & VREG System ✅ |
| 13–18 | **Sprint 4** – Codegen depth (FP, structs, va_arg, register alloc) ✅ |
| 19–21 | **Sprint 5** – Type system completeness (Bit-fields, VLAs, Atomic) |
| 22–23 | **Sprint 5.5** – Codebase Hardening, Cleanup & Sanitization (ASan, W^X, Test Harness) |
| 24–26 | **Sprint 6** – Industrial Testing & Conformance (GCC Torture, Csmith Fuzzing) |
| 27–29 | **Sprint 7** – Cross-platform backends (AArch64, RISC-V, Windows) |
| 30–33 | **Sprint 8** – Two-Pass IR & LuaJIT-Inspired Optimization Pipeline (-O0 to -O3) |
| 34 | Final release, docs, tag `v1.0` |

---

## Immediate Next Actions

### Current Focus: Sprint 5 – Type System Completeness

Sprints 0.5 through 5 are complete with all 116 test suites passing cleanly. The C99 type system is now feature-complete:

1. **Task 34: Bit-fields** (`: width` bit-packing & mask/shift operations in struct/union) — ✅ Complete (`test/bitfield_test.c`)
2. **Task 36: Anonymous struct / union members** — ✅ Complete (`test/anon_struct_test.c`)
3. **Task 37: Variable Length Arrays (VLAs)** — ✅ Complete (`test/vla_test.c`)
4. **Task 38: `_Atomic` operations** — ✅ Complete (`test/atomic_test.c`)
5. **Task 39: `const` / `volatile` semantic enforcement** — ✅ Complete (`test/const_volatile_test.c`). Sprint 5 is now 100% complete!

---

*Prepared by Antigravity AI Assistant — grounded in source inspection 2026-10-02.*
*Updated with Sprint 5.5 (Codebase Hardening) and Sprint 8 (Two-Pass IR & Optimization Pipeline).*
