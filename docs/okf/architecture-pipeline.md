---
type: Architecture
title: ZeroCC Compilation Pipeline & Two-Pass Architecture
description: End-to-end compilation pipeline from C source through AST arena, semantic analysis, two-pass IR, optimization passes, and JIT/AOT code generation.
resource: architecture/pipeline
tags: [architecture, pipeline, ast, ir, jit, aot]
timestamp: 2026-11-03T18:57:28Z
generated: antigravity-agent
verified: engineering-lead
status: active
---

# ZeroCC Compilation Pipeline & Two-Pass Architecture

ZeroCC compiles C99/C11 source files through a unified pipeline that supports both **in-memory JIT execution** (`--run`) and **AOT compilation** (`-S` assembly, `-c` ELF object, `-o` linked executable).

## Pipeline Stages

1. **Lexer & Preprocessor (`src/zero_cc/lexer/`, `src/zero_cc/core/`)**
   - Tokenizes preprocessed/raw C source, handling `#include`, `#define` macros, `#if` arithmetic evaluation (`cpp_expr.h`), virtual embedded headers (`virtual_headers.h`), and `#pragma pack` / diagnostic pragmas (`src/zero_cc/core/pragma_state.c`).
2. **Parser & AST Arena (`src/zero_cc/parser/`)**
   - Parses declarations (`definitions.h`, `decl_struct.h`, `decl_enum.h`, `decl_local.h`), type expressions (`type_parser.h`), statements (`ast_stmt.h`), constant expressions (`const_eval.h`), and expressions (`ast_expr.h`) into a contiguous `ZeroCCASTArena` indexed by `ASTRef` (`uint32_t`).
   - Collects function prototypes and local/global symbol tables into `ZeroCCCompiler` (`src/zero_cc/jit/compile.h`, `src/zero_cc/jit/compile_unit.h`, `src/zero_cc/jit/const_ptr_eval.h`). Global variables (`c->global_vars`) and their name-to-index open-addressing hash table (`c->g_name_hash`) are compiler-instance owned (Sprint 77).
3. **Semantic Analysis (`src/zero_cc/semantic/`)**
   - Validates types, scopes, and promotions (`visitor.c`, `expr.c`, `stmt.c`, `promote.c`) and annotates AST nodes with `TypeInfo *`.
   - The **live pass** is `sem_analyze_ref` over each function body (invoked from `zero_cc_compile_function` in `src/zero_cc/jit/compile.h` after the previously-compiled functions are registered in the global scope); new diagnostics set `ZeroCCCompiler.had_error` and stop compilation. The standalone `semantic_analyze()` facade (with its `DiagBuf`) is exercised separately by `test/semantic/semantic_facade_test.c`.
   - Enforced C99 constraint violations (each with a dedicated negative test in `test/semantic/`):
     - **6.7.5.3p6** call argument count vs. a *prototype* (variadic: at least the named parameters). `TypeInfo.function.is_prototype` distinguishes parameter-type-list declarations (incl. explicit `(void)`) from empty-paren `()` / implicit declarations, which stay unconstrained; `ZeroCCFunction.has_prototype` carries the flag from parse time.
     - **6.8.4.3p1** `goto` target must be a label of the enclosing function — labels are pre-collected over the whole body (`sem_collect_labels`) so forward gotos validate.
     - **6.5.2.1p3** struct/union member must exist in the field table (previously a missing member silently defaulted to `int` at offset 0).
     - **6.8p10** a compound block's closing `}` — truncated source now reports `expected '}' to end block` instead of compiling.
   - **"Did you mean" suggestions** (clang/gcc-style) on undeclared identifiers (candidates = every visible symbol in the scope chain via `scope_chain_names`) and unknown struct/union members (candidates = the field table). `diag_levenshtein` (early-exit at 3) + `diag_best_match` in `semantic/diagnostics.c`: a candidate qualifies at distance ≤ 2 **and** ≤ `max(1, name_len/2)` (tight leash on short names), and must be the **unique** best — tied or distant names are not suggested. Regression: `test/semantic/did_you_mean_test.c` (incl. tie-rejection and no-bogus-suggestion cases).
   - AST walkers must follow child edges **tag-aware**: `aux.step`/`aux.alt` only exist on `NODE_FOR` (step/body) and `NODE_IF`/`NODE_TERNARY` (else); on every other tag they alias `s.str`/`int_val` in the payload union (a goto's target *name length* ends up in `aux.step`). See `zero_cc_ast_addr_of_param_impl` and `sem_collect_labels_impl` for the idiom.
   - **Scope Registration Efficiency** — `zero_cc_compile_function` registers all previously compiled functions and all global variables in the global scope before analyzing each new function body. Previously this performed `zero_cc_type_to_typeinfo` + `type_function` conversion *before* checking if the symbol already existed in scope, producing O(n²) type-conversion work on translation units with hundreds of static functions. `scope_lookup_current()` is now called **before** type conversion in both the function and global-variable registration loops (`src/zero_cc/jit/compile.h`), so duplicate symbols are skipped with O(1) hash lookup instead. c-torture `execute/memclr.c` (2048 static functions) went from >5s to ~2s JIT at -O1+; all other modes improved proportionally. Verified by `make test-torture` (0 regressions) and `make test-opt` (120/120 OK).
   - **GNU Nested Functions / Trampolines (Phase 1 + Phase 2 value-capture + Phase 3a by-reference capture)** — GNU C nested function definitions inside a block (C99 6.9p15 extension) are **accepted** in JIT mode on x86-64, replacing the previous clean rejection. The parser registers the nested function's signature in `c->functions`, saves the body token range, and skips past the body; after the enclosing function is lowered, each nested function's body is re-parsed, lowered, and emitted as a separate function in the same text section (`src/zero_cc/jit/compile.h`, `src/zero_cc/parser/decl_local.h`). **Phase 2 adds closure capture**: every enclosing local in scope at the nested definition is passed as a hidden **leading parameter** (nested signature = captured_0..N, user_params). **Phase 3a makes scalar captures (size ≤ 8) by-reference**: the hidden param is `T*` (the call site passes `zc_sym_addr` of the enclosing slot), the nested function adds a same-named **mirror local** (original type) after the params, and **mirror-in** (entry, after the param save, before VLA-dim evaluation) / **mirror-out** (epilogue, inside the `VREG_0`/`VREG_2`-protected block, which is reserved whenever capture mirrors exist) keep the mirror coherent with the enclosing storage — so nested writes to captured variables are visible to the enclosing function (GNU semantics). mem2reg is skipped for both the nested function and any enclosing function with by-ref captures (captured locals and mirrors must keep stable frame slots), and param promotion is disabled for params captured by reference. The nested **semantic scope** registers capture names with the pointee (original) type, not the pointer param type, or `i++` lowers as pointer arithmetic. Phase 2/3a invariants, each found by a miscompile: (1) the nested local table is reset **before** re-parsing the body (declaration nodes bake `c->local_count` into `node->aux.step` at parse time); (2) `c->current_stack_frame` is reset **before** `zero_cc_add_local` for params; (3) the re-parsed body gets `sem_collect_labels` + `sem_analyze_ref` in a fresh param scope (member offsets in `aux.alt`, TypeInfo); (4) `zero_cc_lower_ast` takes the function identity from `pending_lowering_fn_idx`; (5) nested registration copies `fc->is_variadic`. **Symbol collision**: nested functions are block-scoped and may share source names across enclosing functions (c-torture `20061220-1.c` defines three `nested` fns); each is emitted under a unique mangled symbol (`ZeroCCFunction.nested_sym_name` = `__zf<idx>_<name>`), call sites resolve via the **enclosing chain** (`zero_cc_resolve_callee` in `ast_lowering_call.c`, innermost level wins) and emit the mangled name, and every name-resolution site (AOT `.o` writer in `main.c`, JIT call patcher in `execution.h`, x86-64/RISC-V JIT call emitters) maps mangled names back via `zero_cc_find_function_by_symbol` (`symbol_mgmt.h`). **Multi-level nesting**: the deferred nested-compile loop must NOT restore `function_count` below the count after a nested re-parse, or functions registered *inside* another nested body (e.g. `baz` → `nested` → `nested2`) are discarded and calls to them lower as direct calls to a non-existent function (segfault); and a nested function capturing another nested function's by-ref capture **pointer param** renames that param to `__zc_cap<N>_<i>` at registration (state published in `enc_byref_captures`/`enc_capture_count`) so the mirror local keeps sole ownership of the source name (else `x += 4` on the duplicate name lowers as pointer arithmetic). **Remaining gaps**: runtime VLA parameter dimensions in nested functions (`int foo(char a[2][++N])` — dimension expr saved at registration + evaluated at entry + runtime `sizeof(a[0])`; top-level ZeroCC still rejects runtime-dimension VLA params) keep `pr22061-3/4` at `SKIP-TARGET:nested_capture_writeback`; indirect calls through function pointers (`f = inner; f(x)`) need the real 2-word trampoline/context design (19 `SKIP-TARGET:trampolines` tests).
   - See [Frontend, Type System & Semantic Analysis](./frontend-and-types.md).
4. **AST-to-IR Lowering (`src/zero_cc/ir/ast_lowering*.c`)**
   - Lowers each function's AST into a linear buffer of 51-opcode `Instr` instructions (`IRArena`) plus a call-argument side table (`IRCallArgsArena`).
   - Standalone `.o` translation units: `ast_lowering.c` (function frame & parameter saves), `ast_lowering_stmt.c` (statements & returns), `ast_lowering_expr.c` (expressions, literals, member/index access), `ast_lowering_call.c` (function calls & ABI conventions), `ast_lowering_builtins.c` (GCC/Clang builtins, varargs, constant evaluation, overflow builtins, atomic builtins), `ast_lowering_assign.c` (compound assignment, bitfield assignment, vector compound ops), `ast_lowering_binop.c` (binary operators, unary operators, casts, complex arithmetic helpers, bitfield reductions), `ast_lowering_mc.c` (JIT machine code emission), and `asm_emitter.c` (AOT GAS assembly emission), coordinated via `ast_lowering_internal.h` and `asm_emitter.h`.
   - See [Two-Pass IR & Register Allocation](./ir-and-regalloc.md).
5. **IR Optimization Passes (`src/zero_cc/opt/opt_*.c`)**
   - Runs modular optimization passes over `IRArena` via standalone `opt_pass.c` before register allocation. All passes and CFG builders (`cfg.c`, `cfg.h`) operate explicitly on `ZeroCCCompiler *c` without relying on thread-local globals (`g_compiler`).
   - See [Optimization Passes & Safety Invariants](./optimization-passes.md).
6. **Live Interval Analysis & Linear Scan Register Allocation (`src/zero_cc/ir/live_range.h`, `src/zero_cc/ir/regalloc.h`)**
   - Computes live ranges (extending across loop backedges) and assigns virtual temporaries to ISA physical registers or spill slots.
7. **Backend Emission (`src/zero_cc/ir/ast_lowering_mc.c`, `src/zero_cc/ir/asm_emitter.c`, `src/zero_cc/isa/`)**
   - **JIT Path (`ast_lowering_mc.c` + `execution.h`)**: Emits native machine code bytes into a `CodeBuffer`, patches relocations/calls, maps pages `PROT_READ | PROT_EXEC` (W^X), and executes `main`.
   - **AOT Path (`asm_emitter.c` / `elf_writer.c` / `src/main.c`)**: Emits `.s` assembly via backend `*_asm.c` hooks or writes `.o` ELF relocatable objects and links executables.
   - See [Multi-ISA Backends](./isa-backends.md).

## Key Source Files Map

| Subsystem | Primary Files |
| :--- | :--- |
| CLI & Driver | `src/main.c`, `src/zero_cc/driver/driver_opts.h`, `src/zero_cc/driver/driver_asm.h`, `src/zero_cc/jit/compile.h`, `src/zero_cc/jit/execution.h` |
| Preprocessor | `src/zero_cc/core/preprocessor.h`, `src/zero_cc/core/preprocessor.c`, `src/zero_cc/core/virtual_headers.h`, `src/zero_cc/core/cpp_expr.h` |
| Type System & Symbols | `src/zero_cc/types/type_system.h`, `src/zero_cc/types/symbol_mgmt.h`, `src/zero_cc/jit/type_info.h` |
| Parser (Decl/Stmt/Expr) | `src/zero_cc/parser/definitions.h`, `src/zero_cc/parser/decl_struct.h`, `src/zero_cc/parser/decl_struct.c`, `src/zero_cc/parser/decl_enum.h`, `src/zero_cc/parser/const_eval.h`, `src/zero_cc/parser/type_parser.h`, `src/zero_cc/parser/ast_stmt.h`, `src/zero_cc/parser/decl_local.h`, `src/zero_cc/parser/ast_expr.h` |
| Compilation Unit & Const Ptr | `src/zero_cc/jit/compile_unit.h`, `src/zero_cc/jit/compile_unit.c`, `src/zero_cc/jit/const_ptr_eval.h` |
| Semantic Checker | `src/zero_cc/semantic/visitor.c`, `src/zero_cc/semantic/expr.c`, `src/zero_cc/semantic/promote.c` |
| IR, Lowering & RegAlloc | `src/zero_cc/ir/ir.h`, `src/zero_cc/ir/ast_lowering.c`, `src/zero_cc/ir/ast_lowering_stmt.c`, `src/zero_cc/ir/ast_lowering_expr.c`, `src/zero_cc/ir/ast_lowering_call.c`, `src/zero_cc/ir/ast_lowering_builtins.c`, `src/zero_cc/ir/ast_lowering_assign.c`, `src/zero_cc/ir/ast_lowering_binop.c`, `src/zero_cc/ir/regalloc.h` |
| Optimizations | `src/zero_cc/opt/opt_pass.c`, `src/zero_cc/opt/opt_mem2reg.c`, `src/zero_cc/opt/opt_loop.c`, `src/zero_cc/opt/opt_vectorize.c` |
| Codegen & Backends | `src/zero_cc/ir/ast_lowering_mc.c`, `src/zero_cc/ir/asm_emitter.c`, `src/zero_cc/isa/x86_64.c`, `src/zero_cc/isa/x86_64_asm.c` |

## Related Concepts
- Return to [Root Index](./index.md)
- See [Frontend, Type System & Semantic Analysis](./frontend-and-types.md)
- See [Two-Pass IR & Register Allocation](./ir-and-regalloc.md)
- See [System V AMD64 ABI & Struct Calling Conventions](./abi-sysv-x86_64.md)
