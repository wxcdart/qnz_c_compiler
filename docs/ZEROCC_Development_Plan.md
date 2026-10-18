# ZeroCC Development Plan

## Project Overview

ZeroCC is a multi-ISA C99/C11 compiler targeting x86-64, AArch64, and RISC-V with both an in-memory `W^X` JIT (`--run`) and AOT assembly/ELF backends (`-S`, `-c`, `-o`).

- **Knowledge Graph Navigator**: [`docs/okf/index.md`](./okf/index.md)
- **Full Changelog**: [`docs/CHANGELOG.md`](./CHANGELOG.md)

---

## Completed Sprints

### Sprint 18: C11 Core Features, `<threads.h>`, Multi-Dimensional Arrays/VLAs & Tooling Cleanup ✅
- **Status**: COMPLETE
- **Achievement**: Implemented and verified C11 core language features (`_Alignas`, `_Alignof`, anonymous structs/unions, `_Generic`, `_Static_assert`), C11 concurrency (`<threads.h>` & atomics), multi-dimensional fixed arrays & matrix VLAs (`a[n][M]`), global variable value loads/assignments/ELF object symbols, and LICM call-spanning safety invariants.
- **Key Deliverables**:
  - **C11 Verification Suite (`tools/c11/test_c11.py`)**: 11/11 tests passing across both AOT and JIT modes.
  - **C11 `<threads.h>` (`include/threads.h`) & `-l` Linker Flag (`src/main.c`)**: Mapped `thrd_t`, `thrd_create`, `thrd_join`, `mtx_t`, `mtx_init`, `mtx_lock`, `mtx_unlock`, and `mtx_destroy` to POSIX threads (`-lpthread`).
  - **Multi-Dimensional Arrays & Matrix VLAs (`tools/vla/test_vla.py`)**: Supported up to 16 array dimensions (`a[M][N][K]`), pointer decay in `NODE_INDEX`, struct array member indexing (`p[i][j].x`), and matrix VLAs (`a[n][M]` with runtime outer dimension and compile-time inner stride).
  - **Global Variables & ELF Symbols**: Unified `global_vars` across translation units (`src/zero_cc/types/global_vars.h`, instantiated in `src/zero_cc/parser/ir.c`), emitted `ELF_STT_OBJECT` `.data` symbols, and supported `&global`, scalar loads, assignments, and `global++`.
  - **LICM Call-Spanning Safety Invariant (`opt_loop.c`)**: Prevented LICM from hoisting constants out of loops containing `OP_CALL` / `OP_CALL_INDIRECT`, protecting caller-saved registers when callee-saved registers are exhausted.
  - **Repository Organization**: Curated `tools/` (archiving 136 legacy scripts into `tools/archive/`) and `docs/` (archiving 95 legacy notes into `docs/archive/`).
- **Result**: All **140/140** main tests + **11/11** C11 tests + **6/6** `sret` checks passing (100% pass rate).

### Sprint 17: System V AMD64 & Multi-ISA Hidden Pointer (`sret`) Struct Returns ✅
- **Status**: COMPLETE
- **Achievement**: Implemented hidden return pointer (`sret`) for structs >16 bytes across x86-64 (`%rdi`), AArch64 (`x8` indirect result register), and RISC-V (`a0` hidden first argument) across JIT and AOT code paths with full bidirectional GCC interoperability.
- **Key Deliverables**:
  - Callee saves hidden pointer (`%rdi` / `x8` / `a0`), shifts user integer args where required, writes struct chunks to `[hidden_ptr + i*8]`, and returns `hidden_ptr` in `%rax` / `x0` / `a0`.
  - Caller allocates 16-byte aligned buffer, passes address in hidden register, shifts arguments, and copies chunks or references returned pointer.
  - Handled edge cases: direct member access on call return (`make().c`), struct assignment to existing local (`r = make()`), tail returns (`return other()`), and indirect calls via function pointers (`is_func_ptr`).
  - Automated verification: `python3 tools/sret_verifier.py --verify-all` (6/6 checks) and `--target-all` (QEMU AArch64 & RISC-V).
- **Result**: All 136/136 tests passing (100% pass rate).

### Sprint 16: SIMD Auto-Vectorization & AOT Assembly Emission Fixes ✅
- **Status**: COMPLETE
- **Achievement**: Enabled multi-ISA SIMD auto-vectorization (`opt_vectorize.c`) for `float[N]`, `double[N]`, and `int[N]` loops across x86-64 SSE2/SSE4.1, AArch64 NEON, and RISC-V RVV 1.0; fixed AOT assembly parameter save placement.
- **Result**: All 128/128 tests passing.

### Sprint 14: LICM Two-Pass Rebuild & Loop Lifetime Safety ✅
- **Status**: COMPLETE
- **Achievement**: Loop-Invariant Code Motion (LICM) implemented using a Two-Pass Rebuild architecture into a fresh `IRArena`; extended loop live ranges across backedges and reserved `%rcx` on x86-64 for dynamic shifts.

### Sprint 13: Mem2Reg Stabilization & Register Allocator Spill Safety ✅
- **Status**: COMPLETE
- **Achievement**: Local `mem2reg` pass enabled with sub-8-byte overlapping slot invalidation; reserved `%rsp`, `%rbp`, `%r10`, and `%r11` in `regalloc_init()`.

### Sprint 11 & 10.5.7: Multi-ISA Backend-Driven Assembly & Call ABI Compliance ✅
- **Status**: COMPLETE
- **Achievement**: Implemented `ZeroCCISABackend` vtable and assembly emitters (`x86_64_asm.c`, `aarch64_asm.c`, `riscv_asm.c`) with ABI-compliant integer/FP register argument passing.

---

## Current Status

### ✅ Completed Features
- **Frontend & Preprocessor**: Lexer, `#include` / `#define` / `#ifdef`, `#pragma pack(push/pop)`, diagnostic pragmas, arena-allocated AST (`ZeroCCASTArena`), and `TypeInfo` semantic checker.
- **C99 Support**:
  - 1D and multi-dimensional fixed arrays (`a[M][N][K]`) and matrix VLAs (`a[n][M]`) with scope-exit stack restoration
  - Designated initializers (`.field = val`, `[idx] = val`) & compound literals
  - `inline`, `restrict` alias-group tracking, flexible array members, variadic functions (`<stdarg.h>`)
- **C11 Support**:
  - `_Alignas` & `_Alignof` on types, struct fields, and local variables
  - Anonymous `struct` and `union` field flattening
  - `_Generic` type-based selection & `_Static_assert`
  - `_Atomic` qualifiers, `__sync_*` / `__atomic_*` builtins, and hardware atomic IR opcodes (`OP_ATOMIC_XADD`, `OP_ATOMIC_CMPXCHG`, `OP_ATOMIC_XCHG`, `OP_MFENCE`)
  - `<threads.h>` (`thrd_t`, `mtx_t`) mapped to pthreads & `_Thread_local` (`__thread`) storage
- **IR & Optimization Pipeline (`opt_pass.c`)**:
  - 51-opcode `Instr` IR with `IRCallArgsArena` side table
  - Active passes: Constant Folding, Copy Propagation, CSE, GVN, Local Mem2Reg, LICM (Two-Pass Rebuild with call-spanning guard), SIMD Auto-Vectorization, and Dead Code Elimination
- **Multi-ISA Codegen & ABI**:
  - **x86-64**, **AArch64**, and **RISC-V (RV64GC)** backends
  - Full struct by-value parameter passing and struct returns (`<=8B`, `9-16B`, and `>16B` `sret` hidden pointer across direct and indirect function pointer calls)
  - Direct ELF64 relocatable object writer (`elf_writer.c`) and `W^X` in-memory JIT (`execution.h`)

### 📊 Test & Verification Coverage
- **Main Regression Suite (`make test`)**: **140/140 passing (100%)** in ~0.6s
- **C11 Verification Suite (`python3 tools/c11/test_c11.py`)**: **11/11 passing (100%)**
- **VLA & Multi-Dim Array Suite (`python3 tools/vla/test_vla.py`)**: **100% passing**
- **System V & Multi-ISA `sret` Verifier (`python3 tools/sret_verifier.py --verify-all`)**: **6/6 passing (100%)**
- **OKF v0.2 Knowledge Graph (`python3 tools/okf.py lint`)**: **9/9 pages valid, 44 verified graph edges**

---

## Architecture Overview

```text
C99/C11 Source Code
    ↓
Lexer & Preprocessor (src/zero_cc/lexer/, src/zero_cc/core/preprocessor.h)
    ↓
Arena AST Parser (src/zero_cc/parser/definitions.h, ast_stmt.h, ast_expr.h)
    ↓
Semantic Analyzer (src/zero_cc/semantic/visitor.c, TypeInfo)
    ↓
AST-to-IR Lowering (src/zero_cc/ir/ast_lowering*.c -> 51-opcode Instr)
    ↓
Optimization Pipeline (src/zero_cc/opt/opt_*.c: ConstantFold, CopyProp, CSE, GVN, Mem2Reg, LICM, Vectorize, DCE)
    ↓
Live Range Analysis & Linear Scan RegAlloc (src/zero_cc/ir/live_range.h, regalloc.h)
    ↓
    ├─→ [JIT Path] src/zero_cc/ir/ast_lowering_mc.c → W^X CodeBuffer (execution.h)
    └─→ [AOT Path] src/zero_cc/ir/asm_emitter.c / elf_writer.c → .s / .o / Executable
```

For detailed architectural invariants, consult the OKF Knowledge Graph in [`docs/okf/index.md`](./okf/index.md):
- [`docs/okf/architecture-pipeline.md`](./okf/architecture-pipeline.md)
- [`docs/okf/frontend-and-types.md`](./okf/frontend-and-types.md)
- [`docs/okf/ir-and-regalloc.md`](./okf/ir-and-regalloc.md)
- [`docs/okf/optimization-passes.md`](./okf/optimization-passes.md)
- [`docs/okf/abi-sysv-x86_64.md`](./okf/abi-sysv-x86_64.md)
- [`docs/okf/isa-backends.md`](./okf/isa-backends.md)
- [`docs/okf/build-and-verification.md`](./okf/build-and-verification.md)

---

## Next Steps (Future Roadmap)

### 1. Codebase Modularity & Build Structure Refactoring ✅
- **Eliminate Unity `#include "*.c"` in `parser.c` ✅**: Converted `ast_lowering.c`, `ast_lowering_stmt.c`, `ast_lowering_expr.c`, `ast_lowering_mc.c`, `asm_emitter.c`, and `opt_pass.c` into standalone `.o` translation units in `Makefile`.
- **Split `src/zero_cc/parser/` ✅**: Separated into three dedicated subsystem directories:
  - `src/zero_cc/parser/` (AST parsing, constant evaluation, AST dump)
  - `src/zero_cc/ir/` (IR definitions, AST-to-IR lowering, live ranges, register allocation, and codegen dispatch)
  - `src/zero_cc/opt/` (IR optimization passes & profiling)

### 2. Unified Type System Bridge (`ZeroCCType` ↔ `TypeInfo`) ✅
- **Completed**: Unified base-type resolution (`zero_cc_resolve_base_type`), bidirectional type conversion (`zero_cc_type_to_typeinfo`, `typeinfo_to_zero_cc_type`), single-source array/pointer declarator construction in `ast_stmt.h`, and canonical AST node type accessors (`ast_node_get_typeinfo`, `ast_node_set_typeinfo`) in `src/zero_cc/jit/type_info.h`.

### 3. Register Allocator Spill/Reload in AOT Assembly (`asm_emitter.c`) ✅
- **Completed**: Mirrored `ast_lowering_mc.c`'s scratch-register (`%r10`/`%r11`, `x9`/`x10`, `t0`/`t1`) spill reload/store handling in `asm_emitter.c` and `isa/*_asm.c`, and enforced strict callee-saved-only allocation (or stack spill) for `spans_call` intervals in `regalloc.h`.

### 4. Extended Language & Backend Features
- Full `switch`/`case` jump-table / branch-chain lowering in `lower_stmt_switch`
- 128-bit integer (`__int128`) and `long double` support
- Full SSA-form φ-node construction for cross-block `mem2reg` and GVN

### 5. zero_rt — Self-Contained Cross-Platform Runtime (T3)
- **Plan**: [`docs/zero-rt-plan.md`](./zero-rt-plan.md) — minimal ZeroCC-owned runtime (heap, minimal stdio, string builtins, exit) with no musl dependency, targeting Linux/Windows/macOS via a portable core + per-OS backends (same shape as `ZeroCCISABackend`).
- **Phases**: Phase 0 Linux (first customer of the T2 ELF linker) → Phase 1 Windows (PE/COFF writer + Win64 ABI vtable) → Phase 2 macOS (Mach-O writer; compiler ABI already compatible).
- musl remains vendored pristine as an optional full-stdio library on Linux; float formatting (`%f`/`%e`/`%g`) via vendored Ryū (MIT).
