# ZeroCC Development Plan

## Project Overview

ZeroCC is a multi-ISA C99/C11/C23 compiler targeting x86-64, AArch64, and RISC-V with both an in-memory `W^X` JIT (`--run`) and AOT assembly/ELF backends (`-S`, `-c`, `-o`).

- **Knowledge Graph Navigator**: [`docs/okf/index.md`](./okf/index.md)
- **Full Changelog**: [`docs/CHANGELOG.md`](./CHANGELOG.md)

---

## Current Status

### ✅ Completed Features
- **Frontend & Preprocessor**: Lexer, `#include` / `#define` / `#ifdef`, `#pragma pack(push/pop)`, diagnostic pragmas, main-file-dir quoted-include resolution (C99 6.10.2p2) + real `-I`/`-isystem` include search, arena-allocated AST (`ZeroCCASTArena`), `TypeInfo` semantic checker, Makefile dependency generation (`-MMD`, `-MF`, `-MT`, Sprint 50), `-E` preprocess-only mode (Sprint 51), `-U` command-line undefine, `-include` forced include, and GCC flag compatibility (Sprint 52).
- **Compiler Diagnostics (Sprint 42)**: gcc-format caret diagnostics; C99 constraint checking (call arity vs. prototype 6.7.5.3, goto-label 6.8.4.3, struct member 6.5.2.1, block terminator 6.8p10); "did you mean" suggestions (Levenshtein ≤ 2, unique-best) for undeclared identifiers and unknown members; macro-expansion `note:` chains; multi-error recovery with a 20-error budget (one pass reports all faults); differential gate: `tools/diag_battery.py` (16-case corpus vs gcc, ratcheting baseline, 8/8 category coverage).
- **C99 Support**:
  - 1D and multi-dimensional fixed arrays (`a[M][N][K]`) and matrix VLAs (`a[n][M]`, multiple VLAs per function) with scope-exit stack restoration
  - Unified C99 6.7.8/6.7.9 aggregate-initializer engine: designated initializers (`.field`, `[idx]`, chains, ranges `[2 ... 5] = { ... }`), elided braces, string-fill semantics, char-array string init (C11 6.7.9p13) & compound literals
  - `inline`, `restrict` alias-group tracking, flexible array members, variadic functions (`<stdarg.h>`)
  - Scalar `float`/`double` pipeline: double-width FP value model, width-correct `VLOADSS/VSTORESS/VLOADSD/VSTORESD`, float arithmetic/negation/compound assignment (globals, struct members, and pointer-member chains), float function returns, float initializers
  - File-scope global variables: full aggregate/string/pointer/char-array initializers + JIT/AOT data relocations
  - `switch` (arbitrary case count, O(n) dispatch, GNU case ranges) & `goto`/user labels; C11-conformant cast expressions
  - 32-bit `int` wraparound & end-to-end signedness (`SEXT*/ZEXT*` width extension, signed `>>`), pointer-member chains (`o.p->v`, `(*pp)->w`) with width-safe deref stores, scaled pointer arithmetic
  - 16-bit temp ids (up to 65534 per function); C99/C11 127+ parameter limit (`ZERO_CC_MAX_PARAMS 256`, dynamic arena param slices); C89 implicit-`int` accepted with a diagnostic (robustness)
- **C23 Support**:
  - `typeof` and `typeof_unqual` (plus `__typeof__`, `__typeof`, `__typeof_unqual__`, `__typeof_unqual`) across locals, parameters, return types, struct members, typedefs, casts (`(typeof(T))val`), and `sizeof(typeof(...))`
  - Early parameter scope registration enabling dependent parameter type deduction (`typeof(int) add(typeof(int) a, typeof(a) b)`)
  - Empty `{}` initializers (C23 6.7.10) for automatic locals, static locals, compound literals, and globals
  - `[[attribute-list]]` skipping (e.g. `[[nodiscard]]`, `[[maybe_unused]]`)
  - Predefined object macro `__STDC_NO_COMPLEX__ 1` formally signaling complex number type non-support (C11/C23)
  - Predefined compiler identification macros `__zerocc__ 1` and `__ZEROCC__ 1`
- **C11 Support**:
  - `_Alignas` & `_Alignof` on types, struct fields, and local variables
  - Anonymous `struct` and `union` field flattening
  - `_Generic` type-based selection (6.5.1.1 compatibility) & `_Static_assert`
  - `_Atomic` qualifiers, `__sync_*` / `__atomic_*` builtins (incl. bitwise RMW `__atomic_fetch_or/and/xor`), C11 `<stdatomic.h>`, and hardware atomic IR opcodes (`OP_ATOMIC_XADD`, `OP_ATOMIC_CMPXCHG`, `OP_ATOMIC_XCHG`, `OP_MFENCE`)
  - `<threads.h>` (`thrd_t`, `mtx_t`) mapped to pthreads; `_Thread_local` (`__thread`) static TLS end-to-end on x86-64 (initial-exec, `.tdata`/`.tbss`, JIT + AOT + `-S`, OKF: `tls-static.md`)
- **GNU/Clang Extensions (Sprint 43)**: `__builtin_offsetof`, case ranges, designated-initializer ranges, statement expressions `({ ... })`, verified differentially against GCC (`tools/extension_verifier.py` 14/14).
- **IR & Optimization Pipeline (`opt_pass.c`)**:
  - 133-opcode `Instr` IR with `IRCallArgs` side table (opcode count grew 51 → 133 across Sprints 18-47: atomics, `OP_SYSCALL`, `OP_TLS_LEA`, `OP_PARAM_LOAD`, SIMD, etc.)
  - Optimization levels (`-O0`..`-O3`, **default `-O1`**; `src/main.c`). Registered passes (`g_opt_passes` in `opt_pass.c`, priority order, each tagged with `min_level`):
    - **`-O1`**: `constant_fold`, `flag_dce`, `dce`, `copy_prop`, `mem2reg` (local + cross-block loop-carried promotion)
    - **`-O2`**: `cse`, `gvn`, `licm` (Two-Pass Rebuild, call-spanning guard, multi-def guard, stack-load hoisting); plus AST-level small-function inlining (`ast_lowering_expr.c`, checks `c->opt_level >= OPT_LEVEL_O2`)
    - **`-O3`**: `branchless` (conditional select), `strength_reduction`, `branch_simplify`, `auto_vectorize` (call and mid-body-conditional guards, verified-constant-loop-bound prerequisite)
    - `-O0` is plain codegen (no passes, no inlining)
  - IR shape: flat, non-SSA linear `Instr` array with implicit control flow (labels/jumps); **CFG layer** (`opt/cfg.{h,c}`) provides basic blocks, pred/succ, reachability, and natural loops — a lazy versioned cache over the Instr array, used by LICM/mem2reg/vectorizer (Sprint 35 Phase 2/3); `-dump-cfg` + `tools/cfg_verifier.py`
- **Multi-ISA Codegen & ABI**:
  - **x86-64**, **AArch64**, **RISC-V 64 (RV64GC)**, and **RISC-V 32 (RV32GC)** backends
  - Full struct by-value parameter passing and struct returns (`<=8B`, `9-16B`, and `>16B` `sret` hidden pointer across direct and indirect function pointer calls)
  - Codegen performance: selective callee-saved spilling + fallthrough jump elimination, x86-64 red zone, inline epilogues, frameless leaf functions with parameter promotion, `-fomit-frame-pointer` (Sprint 44) — all verified by `tools/prologue_epilogue_verifier.py` + `tools/extension_verifier.py`
  - Direct ELF64 and ELF32 relocatable object writer (`elf_writer.c`), freestanding `W^X` in-memory JIT (`execution.h`), and direct raw binary AOT compilation default (`-o`, `-c`) across all 4 target ISAs with `ZC_TEXT_AOT=1` escape hatch.
  - **Toolchain**: T0 Makefile depfiles (`-MMD`/`-MF`/`-MT`), T1 multi-file driver (multi-TU JIT merge, per-input `-c`/`-S`, `-E` mode, compatibility flags; `make test-driver` **37/37**) and T2 own static linker (ELF64 + `ar` readers, ET_EXEC output, `--zerocc-link` across **x86-64, AArch64, and RISC-V64**; `make test-linker` **21/21**) — OKF: `multi-tu.md`, `own-linker.md`.
- **zero_rt (Phase 0 & Phase 1, Linux x86-64, AArch64, RISC-V64)**: ZeroCC-owned freestanding runtime compiled by zerocc itself across all three 64-bit architectures (`build/lib/zero_rt.a`, `build/lib/zero_rt_aarch64.a`, `build/lib/zero_rt_riscv64.a`) with 100% test battery parity (brk heap, strings, atexit/exit with `zeroc_main_wrapper`, unbuffered stdio with vendored Ryū float formatting); auto-linked by `--zerocc-link`. OKF: `zero-rt.md`.

### 📊 Test & Verification Coverage (verified 2026-11-04 at HEAD)
- **Main Regression Suite (`make test`)**: **160+/160+** passing — host-gcc compiled test binaries exercising the compiler's internal APIs (lexer/parser/IR). Per `AGENTS.md`, optimizer/codegen changes must be verified directly with `./build/bin/zerocc` (`--run` and `-o`) at the affected `-O` levels.
- **Direct-zerocc verifiers**: `tools/mem2reg_verifier.py` (12/12), `tools/sret_verifier.py` (6/6), `tools/c11/test_c11.py` (11/11), `tools/vla/test_vla.py` (11/11), `tools/licm_verifier.py`, `tools/licm_test_suite.py` (5/5), `tools/cfg_verifier.py` (CFG cross-check), `tools/extension_verifier.py` (14/14 GNU extensions), `tools/literal_verifier.py`, `tools/prologue_epilogue_verifier.py` (all 4 ISAs), `tools/opt_sweep.py` / `make test-opt` (definitive optimizer gate: `-O0..-O3`, JIT + AOT, zerocc itself).
- **Cross-ISA Raw-Emitter QEMU Gate (`make test-isa-qemu`)**: **44/44 passing (100%)** across all 4 target ISAs differential-tested against static-musl under QEMU.
- **Multi-TU Driver Gate (`make test-driver`)**: **37/37**.
- **Own-Linker Gate (`make test-linker`)**: **21/21** (x86-64, AArch64, RISC-V64 under QEMU).
- **GCC `c-torture` Conformance Gate (`make test-torture` / `python3 tools/torture_run.py`)**: differential-tests ZeroCC against `gcc -O1` across the 3,940-file `gcc.c-torture` corpus (`execute/` at `-O0..-O3` × `{JIT, AOT}`, `compile/` at `-O0/-O2`), ratcheting `test/torture/baseline.json` (**`PASS: 3,266`**, **`REJECT: 0`**, **`MISCOMPILE: 0`**, **`TIMEOUT: 0`**, `SKIP-TARGET: 11` trampoline-related). Achieved via Sprints 57-96: zero MISCOMPILE, zero REJECT, zero TIMEOUT.
- **OKF v0.2 Knowledge Graph (`python3 tools/okf.py lint`)**: **18 files valid** (17 concept pages + `log.md`), links & graph reachability verified (18/18 reachable).
- **C Style Checker (`python3 tools/style_check.py`)**: **0 errors** baseline.
- **Diagnostics Battery (`python3 tools/diag_battery.py`)**: differential diagnostics vs gcc on `test/diag/*.c` corpus; ratcheting `test/diag/baseline.json`.

### ✅ Former Open Regressions — FIXED (Sprint 35 Phase 0, 2026-10-20; verified by `make test-opt` 88/88)
Introduced by Sprint 33 (`15588aa`); zerocc `--run`, same results in AOT where checked. All resolved; the table is retained for the record:

| Test | Expected (rc) | Pre-Sprint 33 | HEAD (pre-fix) | Root cause & fix |
| :--- | :--- | :--- | :--- | :--- |
| `test/jit/break_continue.c` | 0 | pass at `-O0..-O2` | **failed at `-O1`/`-O2`/`-O3`** (wrong sum: 2 at `-O1`) | x86 `setcc`/extend ModRM source-operand encoding (REX-extended registers) + LICM shared-header invariant loss |
| `test/jit/goto_loop.c` | 0 | pass | **failed at `-O2`/`-O3`** (rc 1) | x86 `setcc %al` hardcoded dest clobbered live RAX; `x86_emit_extend_asm` ignored `src` |
| `test/aot/loops_conditionals.c` | 0 | pass | **failed at `-O2`/`-O3`** (rc 1 / SIGSEGV AOT) | same x86 extend/setcc encoding bugs |
| `test/licm/*.c` (5 files) | 45/13/45/60/42 | pass | **segfault/hang (rc 139) at `-O2`/`-O3`** | LICM hoisted a loop-dependent `ADD` (missing in-loop source-def check) + DCE single-sweep liveness deleted the backedge write-back → infinite loop |

`test/jit/profile_demo.c` was the reported timeout: it is **timing/nondeterminism-sensitive** (wall-clock output); the sweep flags it `NONDET` rather than hard-failing it.

**Root causes fixed in Sprint 35 Phase 0** (bisected to Sprint 33 `15588aa`; pre-Sprint-33 `d0f3f54` was clean on the same sweep except a latent `-O3` `break_continue` case also covered by the emitter fixes):
1. `src/zero_cc/isa/x86_64.c` `x86_emit_extend` — `MOVSXD`/`MOV` put the destination in both the reg and r/m ModRM fields; now source in r/m, dest in reg.
2. `src/zero_cc/isa/x86_64_asm.c` — `x86_emit_cond_set_asm` hardcoded `setcc %al`/`movzbq %al,…` (clobbered live RAX); now uses the destination's low byte. `x86_emit_extend_asm` ignored `src` (assumed in-place); now emits the real source register.
3. `src/zero_cc/opt/opt_loop.c` — (a) multiple natural loops can share one header; the two-pass rebuild now emits the **union** of all shared-header loops' invariants (dedup `emitted_at_header`) instead of only the first loop's, so an invariant NOP'd by one loop is never lost; (b) `instr_is_invariant` now rejects any candidate whose source operand is defined anywhere in the loop body (including after the candidate — it feeds back through the backedge), unless the def is itself invariant; in-place self-refs stay hoistable.
4. `src/zero_cc/opt/opt_dce.c` — liveness is now computed to a **fixpoint** (monotonic live set, ≤10 backward sweeps; bail-out marks everything live). A single backward sweep mis-declares loop-carried values dead at their backedge definition.
5. `src/zero_cc/opt/opt_mem2reg.c` — `opt_promote_loop_carried` now fully audits the slot (one in-loop load+store, one preheader init, all other loads strictly post-exit) and forwards **all** post-loop loads, not just up to the next label.

**New gate**: `make test-opt` (`tools/opt_sweep.py`) — compiles & runs every standalone test at `-O0..-O3` in **both** JIT and AOT with zerocc itself, comparing `(rc, stdout)` to the `-O0` baseline. **88/88 pass** after the fixes. `make test` (141/141), `mem2reg_verifier.py` (12/12) and `sret_verifier.py` (6/6) also pass.

### 🛡️ Fragility Audit Status ([`docs/fragility-analysis.md`](./fragility-analysis.md))
- **CRITICAL tier**: C1–C11 **ALL RESOLVED** (C8 Sprint 28; C9 cast lowering + C11 vectorizer call-guard Sprint 29; C10 resolved as C11 in disguise). No open C-tier items.
- **HIGH tier — ALL RESOLVED**: H1 (64 MB stack + depth-150 guard), H2 (fatal diagnostics)
- **MEDIUM tier**: M1 resolved (identifier >127 → clean error); **M2 resolved (Sprint 26: dynamic global table + dynamic ELF symbol/reloc/strtab tables + scope TPHT 256 blocks)**; **M3 resolved (Sprint 31: dynamic struct members heap table)**; M4 is debug-dump only; **M5 resolved (Sprint 32: dynamic scope bucket hash table + dynamic locals array + segmented chunked AST arena, verified to 25k+ locals)**
- **AOT `-S` completeness**: `.data` emission and `CVTSI2SD`/`CVTTSD2SI` **resolved in `b0975fc`**; float constant width, FP compares, 3-operand FP arithmetic, `VLOADSS`/`VSTORESS` width, and `OP_CMP_ZERO` **resolved in Sprint 30** — `-S` float parity with the MC path verified (84/84 battery). Remaining `-S` gaps, if any, show up as `# UNKNOWN:` comments in the output.

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
AST-to-IR Lowering (src/zero_cc/ir/ast_lowering*.c -> 133-opcode Instr)
    ↓
Optimization Pipeline (src/zero_cc/opt/opt_*.c + opt/cfg.{h,c} CFG view; -O0 none, default -O1: ConstantFold/FlagDCE/DCE/CopyProp/Mem2Reg, -O2: +CSE/GVN/LICM/inlining, -O3: +Branchless/StrengthReduction/BranchSimplify/Vectorize)
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
- [`docs/okf/multi-tu.md`](./okf/multi-tu.md) — multi-TU driver & JIT image merge
- [`docs/okf/own-linker.md`](./okf/own-linker.md) — T2 own linker
- [`docs/okf/zero-rt.md`](./okf/zero-rt.md) — Zero_RT freestanding runtime (Phase 0)
- [`docs/okf/tls-static.md`](./okf/tls-static.md) — static `_Thread_local`
- [`docs/okf/global-data-relocs.md`](./okf/global-data-relocs.md) — data section & pointer relocations
- [`docs/okf/perf-measurements.md`](./okf/perf-measurements.md) — per-pass costs & the retracted 78× claim
- [`docs/okf/scalar-float-x86.md`](./okf/scalar-float-x86.md) — scalar float pipeline & WSL2 RX-page limitation
- [`docs/okf/build-and-verification.md`](./okf/build-and-verification.md) — build targets & verifiers
- [`docs/okf/c-torture-gate.md`](./okf/c-torture-gate.md) — GCC `c-torture` conformance gate & ratcheting baseline

---

## Next Steps (Future Roadmap)

### ⚡ Current Priorities (as of 2026-11-04, verified at HEAD)

1. **Memory-Error Sanitizer (`-fsanitize=address`) — next optimization/verification sprint.**
   - Prerequisite: mature `zero_rt` with ZeroCC-owned allocators (malloc/free).
   - Scope: 16 TB virtual shadow memory mapping (`0x00007fff8000`–`0x10007fff8000`), allocator poisoning, global and stack frame redzones, 3–5 instruction shadow-byte checks on every load/store. Instrumented in lowering (`ast_lowering_expr.c`/`ast_lowering_stmt.c`).
   - Lighter-weight alternatives to consider first: `-fsanitize-trap=undefined` (trap-based UBSan with no runtime), `-ftrapv` (signed overflow via `jo`/overflow branches), `-fstack-protector` (canary word before saved FP/RA).

2. **RISC-V 32-bit (`rv32`) own linker & `zero_rt` runtime** — ELF32 static executable output (`EM_RISCV`, `ELFCLASS32`) and RV32 freestanding runtime (`zero_rt_riscv32.a`). Word-sized `sw`/`lw` for 4-byte slots (`BE(c)->isa == ISA_RISCV32`).

3. **Loop unrolling** — next CFG-enabled optimization after the sanitizer work. Dominators and natural loops are available on-demand via the CFG layer (`opt/cfg.{h,c}`).

4. **zero_rt Linker Phases 2–3: Windows PE/COFF + Win64 ABI (`T3-Win`) and macOS Mach-O (`T4-Mac`)** — PE/COFF object/executable writer, Win64 ABI vtable (shadow space, arg regs rcx/rdx/r8/r9, no red zone), kernel32 OS boundary wrappers; Mach-O object/executable writer, Mach syscall numbers.

5. **DWARF debug information (`-g`)** — debug line tables (`.debug_line`) and basic symbol info (`.debug_str`, `.debug_info`).

6. **Full SSA form** — rewrite the flat, non-SSA linear `Instr` array into SSA with dominators and out-of-SSA (coalescing). Only if CFG-based passes (LICM, mem2reg, vectorizer) demonstrably hit a wall. Requires: dominator tree (Cooper–Harvey–Kennedy), SSA construction, phi-node lowering.

7. **IR cleanup** — move call arguments into the instruction stream (remove the `instr_index`-keyed `IRCallArgs` side table), move physical registers out of the temp namespace (decouple `TEMP_NONE` sentinel from the ID space), add type tags to `Instr`.

---

### Completed Codebase Modularity & Tooling (Reference)

These items from earlier roadmap sections are all complete:

- **Eliminate Unity `#include "*.c"` in `parser.c` ✅**: Converted `ast_lowering.c`, `ast_lowering_stmt.c`, `ast_lowering_expr.c`, `ast_lowering_mc.c`, `asm_emitter.c`, and `opt_pass.c` into standalone `.o` translation units in `Makefile`.
- **Split `src/zero_cc/parser/` ✅**: Separated into three dedicated subsystem directories:
  - `src/zero_cc/parser/` (AST parsing, constant evaluation, AST dump)
  - `src/zero_cc/ir/` (IR definitions, AST-to-IR lowering, live ranges, register allocation, and codegen dispatch)
  - `src/zero_cc/opt/` (IR optimization passes & profiling)
- **Unified Type System Bridge (`ZeroCCType` ↔ `TypeInfo`) ✅**: Unified base-type resolution (`zero_cc_resolve_base_type`), bidirectional type conversion (`zero_cc_type_to_typeinfo`, `typeinfo_to_zero_cc_type`), single-source array/pointer declarator construction in `ast_stmt.h`, and canonical AST node type accessors (`ast_node_get_typeinfo`, `ast_node_set_typeinfo`) in `src/zero_cc/jit/type_info.h`.
- **Register Allocator Spill/Reload in AOT Assembly (`asm_emitter.c`) ✅**: Mirrored `ast_lowering_mc.c`'s scratch-register (`%r10`/`%r11`, `x9`/`x10`, `t0`/`t1`) spill reload/store handling in `asm_emitter.c` and `isa/*_asm.c`; enforced strict callee-saved-only allocation (or stack spill) for `spans_call` intervals in `regalloc.h`.

