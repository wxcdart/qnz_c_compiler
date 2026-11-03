---
type: Architecture
title: ZeroCC Compilation Pipeline & Two-Pass Architecture
description: End-to-end compilation pipeline from C source through AST arena, semantic analysis, two-pass IR, optimization passes, and JIT/AOT code generation.
resource: architecture/pipeline
tags: [architecture, pipeline, ast, ir, jit, aot]
timestamp: 2026-11-03T13:16:33Z
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
