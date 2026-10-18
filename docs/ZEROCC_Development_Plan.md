# ZeroCC Development Plan

## Project Overview

ZeroCC is a multi-ISA C99/C11 compiler targeting x86-64, AArch64, and RISC-V with both an in-memory `W^X` JIT (`--run`) and AOT assembly/ELF backends (`-S`, `-c`, `-o`).

- **Knowledge Graph Navigator**: [`docs/okf/index.md`](./okf/index.md)
- **Full Changelog**: [`docs/CHANGELOG.md`](./CHANGELOG.md)

---

## Completed Sprints

### Sprint 25: C5 Pointer-Member Chains, C6 AOT-Asm Float, H1/H2, M1 & zero_rt Plan ✅
- **Status**: COMPLETE
- **Achievement**: Multi-segment member chains through pointer members (`o.p->v`, `(*pp)->w`, `geto()->p->f`) with width-safe deref stores and float compound assignment through chains; float locals in the AOT `-S` path on all three ISAs; fatal diagnostics (H2); parser recursion guard + 64 MB stack (H1); identifier-length diagnostic (M1); and the zero_rt cross-platform runtime plan.
- **Key Deliverables**:
  - **C5** (`ir/ast_lowering_expr.c`, `parser/definitions.h`, `jit/type_info.h`): pointer members now record their pointee base type; `lower_member_chain_addr` lowers `chain_depth >= 2` chains; pointer-deref stores are type-sized (1/2/4/8) so `*pv = 77` no longer clobbers adjacent struct fields; float compound assignment through chains (VLOADSS/SD + FADD/FSUB/FMUL/FDIV + VSTORESS/SD). 21/21 C5 checks pass JIT + AOT.
  - **C6** (`isa/{x86_64,aarch64,riscv}_asm.c`, `isa/isa_backend.h`): `emit_load_fp_asm` / `emit_store_fp_asm` vtable entries for `OP_FLOAD`/`OP_FSTORE`; float call arguments in the asm path resolve source registers from `fp_regs`. Verified by `-S` roundtrip through GNU `as` + host gcc.
  - **H1/H2** (`main.c`, `parser/ast_expr.h`): `RLIMIT_STACK` → 64 MB + `expr_depth` guard (clean error at depth 151+, previously a stack-overflow segfault near 200); `had_error` now aborts before codegen/execution instead of emitting broken output.
  - **M1** (`lexer/*`): identifiers >127 chars now a clean fatal diagnostic (previously silent truncation).
  - **Docs**: `docs/zero-rt-plan.md` (T3 plan — musl-independent runtime for Linux/Windows/macOS), Sprint 25 CHANGELOG, fragility status (all C-tier + H-tier RESOLVED); two adjacent pre-existing `-S` gaps documented (`.data` never emitted; `CVTSI2SD` UNKNOWN).
- **Result**: `make test` **140/140**, sret **6/6**, C5 battery rc=0 (JIT+AOT), no regressions vs pre-C5 baseline.

### Sprint 24: C2b 16-bit Temp Ids, C3 Switch Case Cap Removed, AOT-Asm Extend-Width Fix ✅
- **Status**: COMPLETE
- **Achievement**: `Instr` temp fields widened `uint8_t` → `uint16_t` (packed struct 19→24 B; `TEMP_NONE` = 0xFFFF at 140+ sites; 115 truncating casts; LRA 256-cap removed; optimizer tables widened/guarded; dual 0xFF base-register markers in FLOAD/FSTORE/LEA moved to `TEMP_NONE`; legacy 8-bit `OP_CALL` imm arg-packing removed in favor of the always-present `IRCallArgs` side table). The 128-case switch cap was removed (O(n) dispatch; ~32k ceiling from the 16-bit label space). `-S` SEXT/ZEXT width fixed with an explicit opcode→width mapping (`2000000000 + 2000000000` previously roundtripped as +10240).
- **Result**: 117/190 nested calls correct in both modes; 129/500/1000/1500-case switches pass JIT+AOT; C4 reclassified (757/758 locals = scope TPHT capacity, clean diagnostic, no hang).

### Sprint 23: C2a Temp-Id Exhaustion Guard ✅
- **Status**: COMPLETE
- **Achievement**: C2's silent miscompilation became a clean compile error — diagnostic guard at 239 temps in `zero_cc_lower_ast` (covers all 221 allocation sites) with an honest message (split into *functions*; `-O1+` does not help). `tools/generate_deps.py` comment/string stripping fixed with a state machine.

### Sprint 22: C1 32-bit Int Wraparound & Signedness End-to-End ✅
- **Status**: COMPLETE
- **Achievement**: 32-bit `int` math truncates with sign/zero extension at every value boundary (new `OP_SEXT*/ZEXT*` opcodes + `emit_extend`/`emit_extend_asm` vtable slots on all three ISAs); `signed`/`unsigned` consumed in every declaration path (locals, both param parsers, return types, struct members, globals, type-name parsers); signed `>>` lowers to `OP_SAR`; global struct member assignment; x86 byte-register (r/m8) ModRM encoding fixed and verified byte-for-byte against GNU `as`; GAS-legal 32-bit-destination `-S` extend emission.
- **Result**: 39/39 gcc-cross-checked signedness/wraparound matrix (JIT = AOT); `int a=2000000000; a+a == -294967296`.

### Sprint 21: Scalar Float Pipeline (x86-64), Cast Grammar, WSL2 128-Bit RX-Read Workaround ✅
- **Status**: COMPLETE
- **Achievement**: Double-width scalar FP value model completed — `VLOADSS/VLOADSD/VSTORESS/VSTORESD` vtable emitters (widen on load, narrow on store via xmm7 scratch), float global/aggregate initializer bytes, mixed float/int binary ops (`CVTSI2SD` promotion), float negation via `(0.0 - x)` (a `xorpd` memory sign-flip GPF-faults on WSL2 kernel 6.18.54 — 128-bit reads from RX pages), C11 cast grammar (cast operand = `cast-expression`), VLOADSS/SD register class corrected (XMM, not VEC), global struct member width narrowing.
- **Result**: 29/29 global/float/cast/switch/goto probes pass JIT + AOT; new OKF page `scalar-float-x86.md`.

### Sprint 20: C99 Correctness Sweep — Globals, Float Members, Switch/Goto, Data Relocations ✅
- **Status**: COMPLETE
- **Achievement**: File-scope global initializers (aggregate/string/pointer/char-array, padding-correct) with `ZeroCCDataReloc` wired end-to-end (JIT absolute patching + AOT multi-section `R_X86_64_64` relocations); `goto`/user labels/`switch` (CMP/JNE dispatch, fallthrough, `default`); float struct member tags; plus a batch of frontend fixes (`->` arrow access, float function returns, char-literal constant eval, x86 shift aliasing, block-scope typedef, preprocessor `#`/`#endif`).
- **Result**: 17/17 global probes + switch/goto/float-member batteries pass JIT + AOT; new OKF page `global-data-relocs.md`.

### Sprint 19: Unified Register Spill/Reload, Prologue Callee-Save & Standalone C99/C11 Verification ✅
- **Status**: COMPLETE
- **Achievement**: Strict callee-saved-only allocation (or stack spill) for `spans_call` intervals; scratch-register spill handling mirrored across JIT and AOT-asm paths; `OP_CALLEE_SAVE_STORES` prologue/epilogue symmetry on all three ISAs; ELF `.symtab` relocation-index remapping; standalone C99/C11 features (multi-level pointer declarators, opaque struct typedefs, struct array members, >8-byte by-value struct args, arbitrary-type `va_arg`, physical-register guards in optimizer passes).
- **Result**: 29/29 standalone AOT+JIT tests passing.

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
  - Scalar `float`/`double` pipeline: double-width FP value model, width-correct `VLOADSS/VSTORESS/VLOADSD/VSTORESD`, float arithmetic/negation/compound assignment (globals, struct members, and pointer-member chains), float function returns, float initializers
  - File-scope global variables: full aggregate/string/pointer/char-array initializers + JIT/AOT data relocations
  - `switch` (arbitrary case count, O(n) dispatch) & `goto`/user labels; C11-conformant cast expressions
  - 32-bit `int` wraparound & end-to-end signedness (`SEXT*/ZEXT*` width extension, signed `>>`), pointer-member chains (`o.p->v`, `(*pp)->w`) with width-safe deref stores
  - 16-bit temp ids (up to 65534 per function); C89 implicit-`int` accepted with a diagnostic (robustness)
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
- **Main Regression Suite (`make test`)**: **140/140 passing (100%)** in ~0.37s
- **C11 Verification Suite (`python3 tools/c11/test_c11.py`)**: **11/11 passing (100%)**
- **VLA & Multi-Dim Array Suite (`python3 tools/vla/test_vla.py`)**: **100% passing**
- **System V & Multi-ISA `sret` Verifier (`python3 tools/sret_verifier.py --verify-all`)**: **6/6 passing (100%)** (+ `--target-all` QEMU AArch64/RISC-V)
- **Fragility Battery (`python3 tools/fragility_probe.py`)**: parameterized C/H/M probes with JIT+AOT classification, `--bisect` boundary search; `c1-wrap` 29-assertion signedness/wraparound battery; verified boundaries — nested calls 190 OK, switch cases 1500 OK, locals 757 OK / 758 clean error, expr depth 150 OK / 151 clean error
- **OKF v0.2 Knowledge Graph (`python3 tools/okf.py lint`)**: **10/10 pages valid**, links & graph reachability verified

### 🛡️ Fragility Audit Status ([`docs/fragility-analysis.md`](./fragility-analysis.md))
- **CRITICAL tier — ALL RESOLVED**: C1 (S22), C2 (S23 guard + S24 16-bit temps), C3 (S24), C4 (reclassified → M5), C5 (S25), C6 (S25)
- **HIGH tier — ALL RESOLVED**: H1 (64 MB stack + depth-150 guard), H2 (fatal diagnostics)
- **MEDIUM tier**: M1 resolved (identifier >127 → clean error); **M2 (globals 256) and M3 (struct members 64) still truncate/drop silently — top conformance candidates**; M4 is debug-dump only; M5 (locals ~758) fails clean
- **AOT `-S` completeness gaps** (pre-existing, `-o` ELF path unaffected): `.data` section never emitted; `CVTSI2SD` (int→double) unimplemented — tracked in [`docs/zero-rt-plan.md`](./zero-rt-plan.md) §9

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
- 128-bit integer (`__int128`) and `long double` support
- Full SSA-form φ-node construction for cross-block `mem2reg` and GVN
- ~~Full `switch`/`case` lowering~~ — done (Sprint 20); the >128-case cap was removed in Sprint 24

### 5. zero_rt — Self-Contained Cross-Platform Runtime (T3)
- **Plan**: [`docs/zero-rt-plan.md`](./zero-rt-plan.md) — minimal ZeroCC-owned runtime (heap, minimal stdio, string builtins, exit) with no musl dependency, targeting Linux/Windows/macOS via a portable core + per-OS backends (same shape as `ZeroCCISABackend`).
- **Phases**: Phase 0 Linux (first customer of the T2 ELF linker) → Phase 1 Windows (PE/COFF writer + Win64 ABI vtable) → Phase 2 macOS (Mach-O writer; compiler ABI already compatible).
- musl remains vendored pristine as an optional full-stdio library on Linux; float formatting (`%f`/`%e`/`%g`) via vendored Ryū (MIT).

### 6. Toolchain Tiers (multi-TU → standalone linking)
- **T0** — build-system hygiene: `-MMD`-style `.d` dependency generation + a real `-I` include search (the flag currently discards its path, placeholder in `main.c`)
- **T1** — multi-file driver: accept multiple `.c` inputs, per-TU compile, combined link
- **T2** — own linker: `ar` archive reader, the static relocation set (`R_X86_64_{64,PC32,32,32S,RELATIVE}`, PLT32→PC32), multi-section `.rela`; zero_rt Phase 0 is its first customer
- **T3** — zero_rt platform phases (see §5)

### 7. AOT `-S` Path Completeness
- `.data` section emission for globals (today a `-S` object with any global fails to link; the `-o` ELF path is unaffected)
- Remaining asm-emitter opcodes (`CVTSI2SD` int→double and any others surfaced by roundtrips) — tracked in [`docs/zero-rt-plan.md`](./zero-rt-plan.md) §9

### 8. Conformance Headroom (M-tier, [`docs/fragility-analysis.md`](./fragility-analysis.md))
- M2: globals >256 currently silently dropped → clean diagnostic (minimum) or dynamic table
- M3: struct members >64 currently silently dropped → clean diagnostic (minimum) or dynamic array
- M5 (optional): raise the ~758-locals-per-scope cap (scope TPHT 64+61 blocks × 8)
- H1 (optional): raise the expression-depth guard above 150 if the 64 MB stack headroom justifies it
