# ZeroCC – Feature‑Completion Roadmap & C99 Gap Analysis

> Last updated: 2026-10-03. Findings are **grounded in actual source inspection** of
> [`parser.c`](../src/zero_cc/parser.c), [`types_lexer.h`](../src/zero_cc/types_lexer.h),
> [`lexer.c`](../src/zero_cc/lexer.c), [`ast_lowering.h`](../src/zero_cc/parser/ast_lowering.h)
> (modularized into `ast_lowering_stmt.h`, `ast_lowering_expr.h`, `ast_lowering_mc.h`),
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
| Expressions | 24 | 0 | 1 |
| Types & Declarations | 17 | 2 | 2 |
| Preprocessor | 11 | 0 | 0 |
| Code Generation / Runtime | 11 | 2 | 0 |
| Diagnostics | 0 | 1 | 2 |
| **Total Constructs** | **73** | **5** | **5** |

### Sprint Progress Summary

| Sprint | Description | Status | Test Suite |
|---|---|:---:|:---:|
| **Sprint 0 / 0.5** | Include architecture hardening, forward declarations, modularization | ✅ Complete | 40/40 pass |
| **Sprint 1** | Code generation gap fill (floats, strings, ternary, member, subscript, casts) | ✅ Complete | 55/55 pass |
| **Sprint 2 / 2.5** | New parsing (goto, globals, extern, macros, pragmas) | ✅ Complete | 70/70 pass |
| **Sprint 3** | Multi-ISA Backend & VREG System, System TLS (`fs`/`gs`) | ✅ Complete | 85/85 pass |
| **Sprint 4** | Codegen depth (SSE2 FP arithmetic & calling conv, struct return by value, `va_arg`) | ✅ Complete | 105/105 pass |
| **Frontier 1 & 2** | Sealed backend abstraction leaks, full SSE2 FP ABI, generalised x86-64 emission | ✅ Complete | **111/111 pass** |
| **Sprint 5** | Type system completeness (`union` layout ✅, flexible arrays ✅, bit-fields ✅, VLAs, atomics) | ✅ Complete | **112/112 pass** |
| **Sprint 5.5** | Codebase hardening, test harness modernization, ASan/UBSan audit, JIT W^X, diagnostic & header sanitization | ✅ Complete | 109/109 pass |
| **Sprint 5.6** | Register allocation & calling convention hardening (callee-save preservation, OP_CALL 7+ args, nested call clobbering) | ✅ Complete | **112/112 pass** |
| **Sprint 5.7** | Register allocator completion (Phases 1-3: callee-saves, call affinity, real stack spill slot lowering) & `ast_lowering.h` modularization | ✅ Complete | **112/112 pass** |
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
| Compound assignment `+= -= *= /= %= &= \|= ^= <<= >>=` | ✅ | All integer and floating-point compound assignments lowered; fixed float operand order for `-=` and `/=` (Sprint 5.8) |
| Ternary `? :` | ✅ | `NODE_TERNARY` lowered with conditional jumps |
| Comma operator `,` | ✅ | Full `NODE_COMMA` parsing, chaining, and sequential AST lowering (`test/jit/comma_jit.c`) |
| Cast `(T)expr` | ✅ | `NODE_CAST` lowered (int truncation/sign-extend, `cvtsi2sd`, `cvttsd2si`) |
| `sizeof(T)` / `sizeof expr` | ✅ | Compile-time constant evaluation |
| `_Alignof(T)` | ✅ | Compile-time constant evaluation |
| Member access `.` / `->` | ✅ | `NODE_MEMBER` lowered with struct/union member byte-offset arithmetic |
| Array subscript `a[i]` | ✅ | `NODE_INDEX` lowered with scaled base+index addressing |
| Function call | ✅ | Full System V AMD64 ABI calling convention (int regs `%rdi`–`%r9`, FP regs `%xmm0`–`%xmm7`, stack args, variadic `%al`) |
| Compound literals `(T){…}` | ✅ | Full `NODE_COMPOUND_LIT` support for scalars, arrays, structs, address-of, and decay (`test/jit/compound_literal_jit.c`); zero-initialization of omitted gaps per C99 §6.7.8 |
| Designated initializers `.field=val` / `[idx]=val` | ✅ | Full support in both compound literals and variable declarations (`test/jit/designated_init_test.c`); zero-initialization of omitted gaps per C99 §6.7.8; array bounds checking for `[idx]`; flexible array member init rejection |
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
| **VLA** `T a[n]` (variable size) | ✅ | Dynamic stack allocation (`sub rsp, reg`), pointer decay, index/assign (`test/vla_test.c`); initializer rejection per C99 §6.7.8 p3 |
| `struct` (named, with members) | ✅ | Member offset alignment and padding |
| `union` | ✅ | Overlapping member layout, max alignment & size calculation |
| `enum` (simple, explicit values) | ✅ | Enum constants registered and resolved in symbol table |
| Named `enum` as a distinct type-tag | ✅ | Full `TYPE_ENUM` tag, registry, variables, parameters, returns, members, sizeof (`test/jit/enum_tag_jit.c`) |
| `typedef` (scalar, struct) | ✅ | Typedef table resolution |
| `typedef` for function pointers | ✅ | `typedef int (*fn)(int)` parsed and callable |
| `const` / `volatile` / `restrict` | ✅ | Syntactic parsing, qualifier preservation, and semantic enforcement on variables, pointers, members, and structs (Sprint 5 Task 39, `test/const_volatile_test.c`) |
| `_Atomic` | ✅ | C11 `_Atomic` type, atomic ops (`lock cmpxchg`, `lock xadd`, `xchg`), and GCC/C11 `__sync`/`__atomic` builtins (Sprint 5 Task 38, `test/atomic_test.c`) |
| `_Thread_local` | ✅ | System TLS (`fs`/`gs` segment) lowered and verified (`test/tls.c`, `test/tls_jit.c`) |
| `_Alignas(N)` | ✅ | Alignment applied to variable declarations |
| Flexible array members | ✅ | Trailing zero-length arrays supported in structs (Sprint 5 Task 35); initialization rejected per C99 §6.7.8 p21 |
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
| Register allocation | ✅ | Linear-scan register allocation (`regalloc.h`, `live_range.h`) with callee-save preservation (`rbx, r12-r15`), call-spanning affinity, and real stack spill/reload lowering (`[rbp - offset]`) |
| ARM64 back-end | 🟡 | Target abstraction ready in `isa_backend.h`; full instruction selection planned for Sprint 7 |
| RISC-V back-end | 🟡 | Target abstraction ready in `isa_backend.h`; full instruction selection planned for Sprint 7 |
| `libc` symbol resolution | ✅ | Dynamic symbol resolution via `dlsym(RTLD_DEFAULT)` at JIT execution |

---

### 6. Diagnostics

| Construct | Status | Notes |
|-----------|:------:|-------|
| Error messages with source location | ✅ | Standard file:line:col caret formatting implemented in diagnostics.c |
| Macro expansion notes | ✅ | Implemented via `DiagMacroContext` chain in diagnostics |
| Warning vs. error distinction | ✅ | `DiagSeverity` enum (DIAG_ERROR, DIAG_WARNING, DIAG_NOTE) implemented |

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

| Priority | Task | Effort | Status |
|----------|------|--------|--------|
| 18 | `#pragma pack(n)` - structure packing control | 2 days | ✅ Complete (preprocessor forwards `#pragma` / `_Pragma` to the tokenizer) |
| 19 | `#pragma message("text")` - compiler messages | 1 day | ✅ Complete (printed to stderr by the preprocessor, tested in `jit/pragma_diag`) |
| 20 | `#pragma GCC diagnostic` - warning control | 2 days | ✅ Complete (push/pop/ignored/warning/error; translation-unit-wide, not per source range) |
| 21 | `#pragma GCC push_options` / `pop_options` | 1 day | 🟡 Accepted and ignored (no per-region options tracked yet) |
| 22 | Generic pragma passthrough (emit to output) | 1 day | ✅ Complete (unknown pragmas forwarded; unknown `GCC` pragmas ignored) |

**Notes:**
- The preprocessor consumes `message` and `GCC ...` pragmas (including via `_Pragma`) and forwards `pack` and other pragmas to the tokenizer.
- `#pragma GCC diagnostic` state is the final state at end of preprocessing and applies to the whole translation unit; a line-range implementation is future work.

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
| 34 | Bit-fields (`: width` bit-packing & mask/shift operations) | 1 week | ✅ Complete |
| 35 | Flexible array members (trailing zero-length struct arrays) | 2 days | ✅ Complete |
| 36 | Anonymous struct/union members | 3 days | ✅ Complete |
| 37 | VLA (dynamic stack allocation) | 1 week | ✅ Complete |
| 38 | `_Atomic` operations (compare-exchange, fetch-add) | 1 week | ✅ Complete |
| 39 | `const` / `volatile` semantic enforcement | 3 days | ✅ Complete |

---

### Sprint 5.5 – Codebase Hardening, Cleanup & Sanitization (Weeks 22–23)

> **Strategic Priority**: Clean up technical debt, sanitize memory, and structure the test harness before subjecting ZeroCC to heavy industrial test suites (GCC torture & Csmith).

| Priority | Task | Effort | Status |
|----------|------|--------|--------|
| 40 | **Architecture Cleanup & Dead Code Elimination**: Remove legacy stubs, unify `compile.h`/`ast_lowering.h` pathways, and eliminate remaining hacky fallbacks | 4 days | ✅ |
| 41 | **Test Suite Restructuring & Harness Modernization**: Organize 108 flat tests into structured categories (`test/unit/`, `test/jit/`, `test/abi/`, `test/semantic/`); implement unified runner with timing, pass/fail counts, and TAP/JUnit output | 3 days | ✅ |
| 42 | **Memory Leak & Sanitizer Audit**: Clean run of entire test suite under AddressSanitizer (`-fsanitize=address,undefined`) and Valgrind; fix AST arena and string buffer leaks | 4 days | ✅ |
| 43 | **JIT W^X Memory Hardening**: Transition from `PROT_READ|PROT_WRITE|PROT_EXEC` to strict W^X (`mprotect` write-then-execute) for security and real-world OS compliance | 2 days | ✅ |
| 44 | **Diagnostic & Header Sanitization**: Clean up header dependencies, unify `include/` vs `src/` layout (eliminate duplicate `src/zero_cc/zero_cc.h`), eliminate redundant includes, and standardize error reporting with file:line:col caret formatting | 3 days | ✅ |

---

### Sprint 5.6 – Register Allocation & Calling Convention Hardening (Week 23.5)

> **Strategic Priority**: Eliminate caller/callee-saved register clobbering, scale function call argument capacity to the full AMD64 ABI, and protect 9–16 byte struct returns.

| Priority | Task | Effort | Status |
|----------|------|--------|--------|
| 44.1 | **Callee-Saved Register Mask in Prologue/Epilogue (Phase 1)**: Track `used_callee_saved_gpr_mask`; reserve top-of-frame slots (`CALLEE_SAVE_FRAME_RESERVE`); emit stores in prologue and reloads in epilogue for `rbx, r12-r15` | 3 days | ✅ Complete |
| 44.2 | **Multi-Argument Calling Convention**: Support up to 16 integer arguments via `IRCallArgs` side table; emit args 1–6 in `rdi, rsi, rdx, rcx, r8, r9` and args 7+ on stack with 16-byte alignment; load stack args in callee prologue | 2 days | ✅ Complete |
| 44.3 | **Nested Call Clobbering Resolution**: Prevent intervening nested call results in RAX/caller-saved registers from overwriting outer argument registers via proactive stack spilling during evaluation and pre-call reload | 2 days | ✅ Complete |
| 44.4 | **Struct Return Register Protection (9–16 bytes)**: Preserve direct `VREG_0` return across RAX and RDX for 2-eightbyte struct and union values | 1 day | ✅ Complete |

---

### Sprint 5.7 – Register Allocator Completion & Lowering Modularization (Week 23.8)

> **Strategic Priority**: Promote Register Allocation to feature-complete (Phase 2 & Phase 3) and modularize the monolithic 2,300+ line lowering engine.

| Priority | Task | Effort | Status |
|----------|------|--------|--------|
| 44.5 | **Call-Boundary Live Range Splitting & Affinity (Phase 2)**: Track `OP_CALL` positions in `LiveRangeAnalyzer`; flag `spans_call = true` on intervals crossing calls; prioritize callee-saved registers (`rbx, r12-r15`) | 2 days | ✅ Complete |
| 44.6 | **Real Stack Spill Slot Code Generation (Phase 3)**: Implement `regalloc_get_spill_slot_for_temp()`; allocate dynamic frame spill slots `[rbp - (spill_base_offset + (slot + 1) * 8)]`; reload operands via scratch registers `r10`/`r11`; store spilled destination through `r10` | 3 days | ✅ Complete |
| 44.7 | **Extreme Pressure & Spilling Test Suite**: Add Tests 8, 9, 10 to `test/jit/regalloc_test.c` (call-spanning variables, 18 simultaneously live values, spilled two-address ALU expressions) | 1 day | ✅ Complete |
| 44.8 | **`ast_lowering.h` Architectural Modularization**: Decompose 2,320-line monolith into dedicated headers: `ast_lowering_stmt.h` (360 lines), `ast_lowering_expr.h` (1,385 lines), `ast_lowering_mc.h` (452 lines), and coordinator `ast_lowering.h` (178 lines) | 2 days | ✅ Complete |

---

### Sprint 6 – Industrial-Grade Testing, Fuzzing & Conformance (Weeks 24–26)

| Priority | Task | Effort | Status |
|----------|------|--------|--------|
| 45 | Rich diagnostics: `file:line:col: error: …` format and warning vs error distinction | 1 week | ✅ Complete |
| 46 | GCC C-Torture integration (run `gcc.c-torture/execute` ~1,500 tests against ZeroCC JIT) | 1 week | ❌ |
| 47 | Differential Fuzzing: Csmith & yarpgen harness comparing ZeroCC execution results vs GCC/Clang | 1 week | ❌ |
| 48 | Regression test suite (bootstrap from TCC corpus) | 1 week | ❌ |
| 49 | GitHub Actions CI (`make clean && make test` + ASan/UBSan matrix) | 2 days | ❌ |

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
| 54 | **Complete Pass 1 (AST → IR)**: Expand `ir_emit_ops.h` and migrate remaining AST statement/expression nodes to IR; eliminate dual-emission pathway | 1 week | 🟡 Partial (Sprint 8) |
| 55 | **Register Allocator Hardening**: Fix live-interval computation in `live_range.h`; robust linear-scan assignment with callee/caller register spilling in `regalloc.h`, callee-save frame preservation, and stack spill/reload lowering | 1 week | ✅ Complete |
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

### Current Focus: Sprint 6 – Industrial-Grade Testing, Fuzzing & Conformance

Sprints 0.5 through 5.7 are fully complete with all 112 test suites passing cleanly under both standard builds and AddressSanitizer/UBSan:

1. **Sprint 5.6: Register Allocation & Calling Convention Hardening** — ✅ Complete:
   - AMD64 callee-saved register preservation (`rbx`, `r12`–`r15`) with top-of-frame slot allocation.
   - Calling convention expanded to support up to 16 arguments: arguments 1–6 in registers, arguments 7+ on stack with 16-byte alignment.
   - Fixed caller-saved clobbering across nested function call expressions via sequential argument spilling and pre-call reloading.
   - Preserved 9–16 byte struct returns across `rax` and `rdx`.
   - Comprehensive JIT testing in `test/jit/regalloc_test.c`.

2. **Sprint 5.7: Register Allocator Completion & Lowering Modularization** — ✅ Complete:
   - **Phase 2 (Call-Boundary Affinity)** — ✅ Complete:
     - In `live_range.h`, `LiveRangeAnalyzer` tracks all `OP_CALL` positions and flags `spans_call = true` on intervals that cross call boundaries.
     - In `regalloc.h`, `alloc_reg()` prioritizes callee-saved registers (`rbx`, `r12`–`r15`) for intervals spanning calls.
     - Fixed `VREG_0` (RAX) pre-coloring so caller-saved RAX is not erroneously marked free when active.
     - Verified with Test 8 in `test/jit/regalloc_test.c`.
   - **Phase 3 (Real Stack Spill Slot Lowering)** — ✅ Complete:
     - Implemented `regalloc_get_spill_slot_for_temp()` to track spilled intervals.
     - Dynamically reserved spill stack space (`c->current_stack_frame += c->regalloc.next_spill_slot * 8`) with aligned `spill_base_offset`.
     - In `emit_ir_to_machine_code()`, emitted reload instructions into distinct scratch registers (`phys_src1 = r10`, `phys_src2 = r11`) before opcode execution to protect 2-address operations, and stored back `phys_dst = r10` to the stack frame slot at `[rbp - (spill_base_offset + (slot + 1) * 8)]`.
     - Handled spilled arguments in `OP_CALL` for both stack-passed and register-passed arguments.
     - Added Tests 9 & 10 in `test/jit/regalloc_test.c` (18 simultaneously live variables and spilled call-spanning arithmetic).
   - **Architectural Modularization** — ✅ Complete:
     - Decomposed 2,320-line `ast_lowering.h` into clean single-responsibility modules:
       - `ast_lowering_stmt.h` (360 lines)
       - `ast_lowering_expr.h` (1,385 lines)
       - `ast_lowering_mc.h` (452 lines)
       - `ast_lowering.h` (178 lines coordinator)
     - Test Suite status: **112/112 passed** cleanly under `make test` and `make asan`.

3. **Sprint 6: Industrial-Grade Testing, Fuzzing & Conformance** — 📅 Current Priority:
   - **Task 45: Rich diagnostics**: `file:line:col: error: ...` format, caret pointers, and warning vs. error distinction. ✅ **Complete**:
     - Migrated all parser error reporting from `c->had_error = 1; snprintf(...)` to `diag_report_error_at_token()` API.
     - Rich diagnostics with `file:line:col: error: message` format and caret `^` pointers.
     - Warning vs. error distinction implemented via `DiagSeverity` enum (DIAG_ERROR, DIAG_WARNING, DIAG_NOTE).
     - Macro expansion context tracking for nested macro diagnostics.
     - All raw `fprintf(stderr, ...)` error calls migrated to diagnostics API.
     - All 114 tests pass with proper diagnostic output.

---

*Prepared by Antigravity AI Assistant — grounded in source inspection 2026-10-03.*
*Updated with Sprint 5.7 full completion (Phases 1-3 + modular lowering split).*
*Updated 2026-10-03: Task 45 (Rich Diagnostics) complete - migrated all error reporting to diagnostics API.*

