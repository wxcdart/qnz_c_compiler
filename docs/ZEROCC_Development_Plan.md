# ZeroCC Development Plan

## Project Overview

ZeroCC is a multi-ISA C99/C11 compiler targeting x86-64, AArch64, and RISC-V with both an in-memory `W^X` JIT (`--run`) and AOT assembly/ELF backends (`-S`, `-c`, `-o`).

- **Knowledge Graph Navigator**: [`docs/okf/index.md`](./okf/index.md)
- **Full Changelog**: [`docs/CHANGELOG.md`](./CHANGELOG.md)

---

## Completed Sprints

### Sprint 34: Small Function Inlining (`-O2`/`-O3`) & LICM Multi-Def Safety ⚠️ (landed with open risks)
- **Status**: COMMITTED (`0272a2a`). Inlining risks were audited in Sprint 35 Phase 1 (2026-10-20): FP signatures, body locals, labels/goto and `&param` are now rejected by conservative candidate filters (`jit/compile.h`); see the Phase 1 section and `test/inline_audit/` (17 differential tests in the `opt_sweep.py` corpus).
- **Achievement**: AST-level call-site inlining of small `static`/`inline` leaf functions (≤35 AST nodes, non-variadic, non-recursive, no struct return >8B, no calls in body) with TU-level AST retention/cloning. Fixed a pre-existing LICM bug that hoisted instructions whose destination temp had multiple definitions in the loop.
- **Key Deliverables**: `parser/ast_arena.h` (`zero_cc_ast_clone`/`_node_count`/`_has_call`), `jit/compile.h` (candidate detection), `ir/ast_lowering_expr.c` + `ast_lowering_stmt.c` (param substitution, return redirection), `opt/opt_loop.c` (multi-def guard).
- **Measured**: 100M-iteration helper-call loop 380 ms (`-O0`, no inlining) → 143 ms (`-O2` JIT). Single benchmark; not a general claim.
- **Phase 1 (done)**: conservative candidate filters close the audited inlining gaps (FP signatures, body locals, labels/goto, `&param`).
- **Phase 2 (done)**: CFG layer (`opt/cfg.{h,c}`) — the long-term root-cause fix for per-pass control-flow re-derivation; see the Phase 2 entry below. **Next: Phase 3** — migrate the known-fragile passes (mem2reg loop promotion, LICM loop detection, vectorizer body check) onto the CFG.
- **Follow-up (post-Phase 2, optional)**: re-enable inlining of bodies *with* locals by re-registering body locals (name+type snapshot taken at registration time, before the callee's `c->locals` is reset) into the caller's frame at the call site, with a caller-name-collision check; also FP param/return support once the inlining context carries parameter types.

### Sprint 33: Cross-Block mem2reg & LICM Invariant Load Hoisting ⚠️ (introduced regressions — fixed in Sprint 35 Phase 0)
- **Status**: COMMITTED (`15588aa`) but **regressed correctness**. Verified by building `d0f3f54` (pre-Sprint 33, all pass) vs `15588aa` (fail): `test/jit/break_continue.c` (wrong result at `-O1`, the default level), `test/jit/goto_loop.c` and `test/aot/loops_conditionals.c` (wrong rc at `-O2`), and all five `test/licm/*.c` (segfault, rc 139, at `-O2`/`-O3`; they are correct at `-O0`/`-O1`). These were not caught because the reported `make test` 140/140 does not run those cases through zerocc at the affected levels.
- **Intended achievement**: `opt_promote_loop_carried()` promotes induction variables/accumulators to virtual registers; straight-line mem2reg preserved across fallthrough branches/labels; LICM hoists read-only stack loads and extensions. `mem2reg`/`copy_prop` promoted to `-O1` (the default), `licm` to `-O2`. 100M-iteration loop 251 ms → 160 ms (measured on one benchmark).
- **Suspects**: `opt_promote_loop_carried` (loop promotion with `break`/`continue`/`goto` exits and multiple backedges) for the `-O1` failure; LICM stack-load/extension hoisting for the `-O2` segfaults.

### Interim hardening commits (between Sprint 31 and Sprint 32) ✅
- **`8781592` — `-O` level gating**: `OptPass.min_level`, `comp->opt_level`; passes tagged O1/O2/O3; `-O0` is now plain codegen. **Default level is `-O1`** (`src/main.c`).
- **`5766bee` — Vectorizer hardening**: `analyze_loop_for_vectorization_full` now rejects loops with mid-body conditional branches (`has_mid_body_cond`, any `OP_JE/JNE/JL/JLE/JG/JCOND` that is not the backedge at `loop->exit_idx`), and the increment rewrite only matches the loop counter (`loop_var_temp`). This **resolves both C11 residuals** that `fragility-analysis.md`, `CHANGELOG.md` and the OKF `optimization-passes.md` still list as open.

### Sprint 32: M5 Dynamic Scope Hash Table, Dynamic Locals Array & Segmented AST Arena ✅
- **Status**: COMPLETE
- **Achievement**: Replaced the fixed Two-Phase Hash Table (`ZeroFlattenTpht`) with a dynamically resizing separate-chaining bucket hash table in `Scope`. Converted `c->locals` in `ZeroCCCompiler` to a dynamically doubling array with an $O(1)$ hash table for variable lookup, eliminating the $O(N^2)$ compilation bottleneck. Converted `ZeroCCASTArena` to 1,024-node fixed chunks to eliminate dangling pointers on expansion. Added stack probing for frames >4KB. Verified up to 25,000 local declarations in both JIT and AOT.
- **Key Deliverables**:
  - `semantic/scope.{h,c}`: Dynamic separate-chaining hash table with 75% load factor auto-resizing.
  - `types/symbols.h`, `types/symbol_mgmt.h`, `main.c`, `jit/compile.h`: Dynamic `c->locals` array and $O(1)$ bucket table.
  - `parser/ast_arena.h`, `parser/ast_stmt.h`, `parser/parser.h`: Chunked 1,024-node segmented AST arena.
  - `isa/x86_64.c`, `isa/x86_64_asm.c`, `ir/ir.h`, `ir/ir_emit_ops.h`, `opt/opt_mem2reg.c`: Frame offsets >32KB and stack probe loop for large frames.
- **Result**: Tested 1,000 to 25,000 local variables (JIT + AOT rc=0); `make test` **140/140**, sret **6/6**, OKF lint PASS.

### Sprint 31: M3 Dynamic Struct Members Table ✅
- **Status**: COMPLETE
- **Achievement**: Converted `ZeroCCType.members` from a fixed 64-element inline array to a dynamically resizing heap table, removing the silent struct member cap.

### Sprint 30: `-S` Float Emit Completeness (+ C12 comisd flags, C13 REX ordering) ✅
- **Status**: COMPLETE
- **Achievement**: The `-S` text path now has float parity with the MC path: 3-operand FP arithmetic aliasing, `ucomisd`/`fcmp` compares via a new `emit_fp_cmp_asm` vtable slot, `movsd` for 8-byte rodata doubles, `VLOADSS` widen / `VSTORESS` narrow through the FP scratch (`xmm15`/`s15`/`fcvt.s.d`), and `OP_CMP_ZERO` (`testq`). Two MC-path bugs fell out: **C12** — `ucomisd` only defines ZF/PF/CF, so signed `setl/setle/setg/setge` after FP compares were wrong; new `CC_FLT_LT/LE/GT/GE` (6–9) → `setb/setbe/seta/setae` with a post-regalloc remap (x86). **C13** — `x86_emit_fp_cmp` built REX wrong and emitted it before the `0x66` prefix (CPU drops it); same ordering fixed in `vstoress`/`fp_neg`.
- **Result**: `/tmp/sfloat` battery **84/84** (JIT `-O0`/`-O1`, AOT, `-S`+gcc), zero `# UNKNOWN:` ops; a5/e11 `-S` rc=0; `make test` **140/140**, sret **6/6**, OKF lint PASS.

### Sprint 29: C9 Float Cast Lowering & C11 Vectorizer Call-Guard (C10 Resolved) ✅
- **Status**: COMPLETE
- **Achievement**: (1) `NODE_CAST` now actually converts: int/pointer→float/double emits `CVTSI2SD` (previously the raw int bits were passed/stored as a double bit pattern — `use2((float)k)` gave 3.0e-323), and float→float/double no longer truncates through `cvttsd2si`. (2) The auto-vectorizer no longer vectorizes loops whose body contains a call/return (C11): it previously widened the induction step to `k += 4` and converted scalar float ops to VADDPS while the scalar call-body ran 1/4 as often — dropping loop iterations. This also resolved **C10** ("float local as call argument inside a loop" — its repros were C11 in disguise).
- **Key Deliverables**:
  - `ir/ast_lowering_expr.c`: float/double target branch in `NODE_CAST` (`CVTSI2SD` for int/pointer sources; no-op for float sources; sets `last_expr_is_float`).
  - `opt/opt_vectorize.c`: `has_call` guard (OP_CALL/OP_CALL_INDIRECT/OP_RET) in the vectorization criteria; documented residuals (mid-body branches, increment-rewrite breadth).
- **Result**: C9 battery (e25/e25p/e36/e37) and C11 loop battery (e21–e48) rc=0 JIT `-O0`/`-O1` + AOT with gcc cross-checks; vectorizer still fires on real array loops (e48 VADDPS); `make test` **140/140**, sret **6/6**, OKF lint PASS, C8/C5/d batteries rc=0, c1-wrap OK, 500-globals OK.

### Sprint 28: C8 FP Call-Result Liveness — Materialized Returns, XMM Spans-Call Spill, Pool Aliasing ✅
- **Status**: COMPLETE
- **Achievement**: Float/double function-call results are no longer clobbered by FP arithmetic later in the same expression (`if(use2(2.0f) != 1.0f+1.25f)` and harder multi-call forms), in both JIT and AOT. Three stacked root causes fixed: (1) FP call returns were untracked physical-register references — now materialized into a fresh temp via `OP_FMOV` after the call; (2) XMM intervals ignored `spans_call` — they now always spill (no callee-saved FP registers exist); (3) the spill-selection steal branch could hand over `-1` from an already-spilled interval, creating a dangling temp — selection now skips spilled intervals, FP intervals spanning a call may not steal, and a `-1` steal falls back to spilling. Supporting: class-aware FP spill reload/store through new `fp_scratch_reg1/2` (physical 15/14, outside the XMM pool) in both emitters, and XMM↔VEC pool cross-marking (a pre-existing aliasing bug where a scalar float local's 16-byte VLOAD could clobber an overlapping XMM temp).
- **Key Deliverables**:
  - `ir/ast_lowering_expr.c`: `OP_FMOV` materialization of float/double call results.
  - `ir/regalloc.h`: XMM `spans_call` → forced spill; `is_spilled` skip + no-steal guards in both spill-selection loops; `-1`-steal fallback; XMM↔VEC pool cross-marking (shared-hardware ISAs only); `REGALLOC_FP_SCRATCH1/2` constants.
  - `ir/ast_lowering_mc.c` + `ir/asm_emitter.c`: class-aware spill reload/store (GPR scratch vs `emit_load_fp/store_fp` into the FP scratch XMMs).
  - `isa/isa_backend.h` + x86_64/aarch64/riscv backends: `fp_scratch_reg1/2` (xmm15/xmm14, d15/d14, f15/f14).
- **Discovered (documented, pre-existing on the pre-C5 baseline)**: **C9** — inline int→float cast in call-argument position drops the conversion (`use2((float)k)` passes raw int bits); **C10** — a float local as a call argument inside a loop loses its value (literal/int arguments fine). Repros `/tmp/c6v/e25.c`, `e30.c`, `e31.c`.
- **Result**: C8 battery (e10–e18, e22, f.c) rc=0 JIT `-O0`/`-O1` + AOT; `make test` **140/140**, sret **6/6**, OKF lint PASS, C5/d batteries rc=0, c1-wrap OK, 500-globals OK. The `-S` path contains the C8 mechanism correctly; e11 via `-S`+gcc still fails only on the separate pre-existing `-S` float-emit gaps (2-operand `addsd`, integer `cmpq` for float compares, `movss` on 8-byte rodata doubles).

### Sprint 27: C7 Member Compound Assignment & Width-Correct Member Stores ✅
- **Status**: COMPLETE
- **Achievement**: Compound assignment to single-segment struct members (`s.w += 5`, `s.f += 0.5f`, `s.d *= 2.0`, `p->m op= v`) now loads the current member value, applies the op, and stores the result (previously only the RHS was stored — pre-existing since before C5). Member stores in the shared tail became width-correct (no more adjacent-member clobber from 64-bit stores). **C8 discovered and documented**: float function-call results are clobbered by FP arithmetic in the same expression (both modes, pre-existing) — suspected FP liveness issue around `OP_CALL`.
- **Key Deliverables**: `member_apply_compound()` helper in `ir/ast_lowering_expr.c` (width-correct loads, integer + float op sets) wired into all four single-segment member-assignment paths; width-correct tail store.
- **Result**: d1–d8/c2/c3/w1/w2 probes pass JIT+AOT; `make test` **140/140**, sret **6/6**, C5 battery rc=0, c1-wrap OK, 3000-globals AOT OK.

### Sprint 26: M2 Dynamic Global Table & ELF Writer Tables, Scope TPHT Growth ✅
- **Status**: COMPLETE
- **Achievement**: Removed the three stacked caps behind "globals ≤ 256 silently dropped": the global variable table, the scope two-phase hash table (64+61 → 256+247 blocks), and the ELF writer's symbol/strtab/relocation tables are all dynamically growing. 3000 globals verified in JIT + AOT (previously: silent drop at 257, AOT runtime segfault past ~1100 from dangling symbol indices). The TPHT growth also raised M5 (locals per scope) from ~758 to ~3000.
- **Key Deliverables**:
  - `types/global_vars.h`: heap-backed `global_vars` (doubles on demand); `add_global_var` callers in `jit/compile_unit.h` now report allocation failure as a clean fatal diagnostic.
  - `semantic/scope.c` + `common/zero_tpht.h`: `SCOPE_TPHT_BLOCK_COUNT` 64 → 256; overflow block count scales with the primary (odd, ~95%).
  - `jit/elf_writer.{h,c}`: dynamic `symbols`/`reloc_*`/`strtab` (256 initial, doubling); `ElfWriterSymbol.name` → stable `name_offset` into the strtab (realloc-safe); heap-sized `sym_remap`; `elf_writer_cleanup` frees the new tables.
- **Result**: `make test` **140/140**, sret **6/6**, C5 battery rc=0 JIT+AOT, 1000 locals OK (was 758), 1500-case switch OK, c1-wrap OK.

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
  - Optimization levels (`-O0`..`-O3`, **default `-O1`**; `src/main.c`). Registered passes (`g_opt_passes` in `opt_pass.c`, priority order, each tagged with `min_level`):
    - **`-O1`**: `constant_fold`, `flag_dce`, `dce`, `copy_prop`, `mem2reg` (local + cross-block loop-carried promotion)
    - **`-O2`**: `cse`, `gvn`, `licm` (Two-Pass Rebuild, call-spanning guard, multi-def guard, stack-load hoisting); plus AST-level small-function inlining (`ast_lowering_expr.c`, checks `c->opt_level >= OPT_LEVEL_O2`)
    - **`-O3`**: `strength_reduction`, `branch_simplify`, `auto_vectorize` (call and mid-body-conditional guards; counter-only increment rewrite)
    - `-O0` is plain codegen (no passes, no inlining)
  - IR shape: flat, non-SSA linear `Instr` array with implicit control flow (labels/jumps); no CFG, basic blocks or dominators (see Sprint 35 plan)
- **Multi-ISA Codegen & ABI**:
  - **x86-64**, **AArch64**, and **RISC-V (RV64GC)** backends
  - Full struct by-value parameter passing and struct returns (`<=8B`, `9-16B`, and `>16B` `sret` hidden pointer across direct and indirect function pointer calls)
  - Direct ELF64 relocatable object writer (`elf_writer.c`) and `W^X` in-memory JIT (`execution.h`)

### 📊 Test & Verification Coverage
- **Main Regression Suite (`make test`)**: 140/140 passing in ~0.4s — **but note what it validates**: the default (`--mode jit`) runs binaries in `build/test/*` that `make all` compiles with the **host `gcc`** (these tests exercise the compiler's internals via `zero_cc.h`, e.g. lexer/parser/IR APIs, not zerocc's generated code). It therefore does **not** detect miscompiles by zerocc's optimizer or codegen. `--mode aot` compiles with zerocc, but only a minority of the 145 sources are standalone programs zerocc can compile (42/145 pass), so it is not a usable baseline either. Per `AGENTS.md`, optimizer/codegen changes must be verified directly with `./build/bin/zerocc` (`--run` and `-o`) at the affected `-O` levels.
- **Direct-zerocc verifiers (actually exercise generated code)**: `tools/mem2reg_verifier.py --verify-all` (12 cases, JIT `-O0`/`-O1`, AOT `-O1` vs GCC), `tools/sret_verifier.py` (6/6), `tools/c11/test_c11.py`, `tools/vla/test_vla.py`, `tools/fragility_probe.py`, `tools/licm_verifier.py`, `tools/licm_test_suite.py`, and `tools/opt_sweep.py` / `make test-opt` (Sprint 35 Phase 0: sweeps the whole `test/{jit,aot,licm}` corpus across `-O0..-O3` in JIT + AOT with zerocc itself — the definitive optimizer gate).
- **C11 Verification Suite (`python3 tools/c11/test_c11.py`)**: **11/11 passing (100%)**
- **VLA & Multi-Dim Array Suite (`python3 tools/vla/test_vla.py`)**: **100% passing**
- **System V & Multi-ISA `sret` Verifier (`python3 tools/sret_verifier.py --verify-all`)**: **6/6 passing (100%)** (+ `--target-all` QEMU AArch64/RISC-V)
- **Fragility Battery (`python3 tools/fragility_probe.py`)**: parameterized C/H/M probes with JIT+AOT classification, `--bisect` boundary search; `c1-wrap` 29-assertion signedness/wraparound battery; verified boundaries — nested calls 190 OK, switch cases 1500 OK, locals 1000 OK / ~3000 clean error, expr depth 150 OK / 151 clean error, globals 3000 OK (JIT+AOT)
- **OKF v0.2 Knowledge Graph (`python3 tools/okf.py lint`)**: **11 files valid** (10 concept pages + `log.md`), links & graph reachability verified

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
AST-to-IR Lowering (src/zero_cc/ir/ast_lowering*.c -> 51-opcode Instr)
    ↓
Optimization Pipeline (src/zero_cc/opt/opt_*.c; -O0 none, default -O1: ConstantFold/FlagDCE/DCE/CopyProp/Mem2Reg, -O2: +CSE/GVN/LICM/inlining, -O3: +StrengthReduction/BranchSimplify/Vectorize)
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
- Full SSA-form φ-node construction for cross-block `mem2reg` and GVN — **deferred**; decision and prerequisites (CFG first) in §10
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
- ~~`.data` section emission for globals~~ — **done (`b0975fc`)**; objects with globals now link from `-S` output
- ~~`CVTSI2SD` (int→double)~~ — **done (`b0975fc`)**, plus `CVTTSD2SI`
- Remaining (Sprint 27/28 findings): float constants loaded with wrong width in the asm path (`movss` on 8-byte rodata doubles), float `!=` emitted as integer `cmpq` (needs `ucomisd` + `setcc`), and 2-operand `addsd`/`subsd` FP arithmetic (needs the 3-operand form with a `movsd` of src1, or the commutative shortcut) — tracked in [`docs/zero-rt-plan.md`](./zero-rt-plan.md) §9

### 7a. Conformance residuals from Sprints 28/29 (all resolved; watch-items only)
- ~~**C9**~~ — inline int→float cast dropped in call-argument position — **fixed (Sprint 29)** via the `NODE_CAST` float-target branch
- ~~**C10**~~ — float local as call argument inside a loop — **fixed (Sprint 29)** (was C11 in disguise)
- ~~**C11**~~ — auto-vectorizer vectorized loops containing calls (loop counter widened to `k += 4`) — **fixed (Sprint 29)** via the `has_call` guard; the two documented residuals (mid-body conditional branches, increment-rewrite breadth) were **fixed in `5766bee`** (`has_mid_body_cond` + counter-only increment rewrite). `docs/fragility-analysis.md` §C11 and the OKF page still list them as open and need updating (Sprint 35 Phase 4).

### 8. Conformance Headroom (M-tier, [`docs/fragility-analysis.md`](./fragility-analysis.md))
- ~~M2: globals >256 silently dropped~~ — **done (Sprint 26)**: global table, scope TPHT (256+247 blocks), and ELF symbol/reloc/strtab tables all dynamically growing; 3000 globals verified JIT + AOT
- ~~M3: struct members >64 currently silently dropped~~ — **done (Sprint 31)**: dynamically resizing heap table for struct members
- ~~M5: raise the ~758-locals-per-scope cap~~ — **done (Sprint 32)**: dynamic scope separate-chaining hash table, dynamic `c->locals` array with O(1) hash lookup, segmented chunked AST arena; verified to 25,000+ locals
- H1 (optional): raise the expression-depth guard above 150 if the 64 MB stack headroom justifies it

### 9. Optimization Passes & GCC Performance Parity
- **Cross-Block `mem2reg`** — implemented (Sprint 33) and **correctness restored (Sprint 35 Phase 0)**: conservative slot audit + full post-loop load forwarding; passes `make test-opt` at `-O0..-O3` (JIT + AOT).
- **Function Inlining** — implemented (Sprint 34): AST-level, leaf-only; audit tracked in Sprint 35 Phase 1.
- ~~**Vectorizer residuals** (mid-body conditionals, increment-rewrite breadth)~~ — **done** (`5766bee`).

### 10. Sprint 35 (PLANNED): Correctness Recovery, Inline Audit, CFG Layer, Pass Migration

**Motivation.** (1) Sprint 33 regressed correctness (see Open Regressions) and `make test` could not detect it, so the first job is to fix it and build a harness that can. (2) The IR is a flat, non-SSA, linear `Instr` array with implicit control flow. Passes each re-derive control-flow facts (`label_to_idx` in LICM, jump-target pre-scan in mem2reg, span pattern-matching in the vectorizer), which is the common root of the LICM multi-def bug and the Sprint 33 loop-promotion regressions. Sprint 34 inlining produces branchy multi-definition code and raises the exposure. Decision (2026-10-19): **add a CFG now; defer SSA** until CFG-based passes demonstrably hit a wall.

**Guiding constraints**
- The CFG is a *view* over the existing `Instr` array (blocks = index ranges); no new IR, no copying, no change to regalloc input.
- Built lazily, cached, invalidated only by passes that change control flow. Not built at `-O0` (JIT fast path unchanged).
- Dominators computed on demand only (Cooper–Harvey–Kennedy).
- No performance or "done" claim without a direct-zerocc measurement/verification at the affected `-O` levels. `make test` alone is not evidence. The "~78× faster than GCC" figure in the docs has no recorded methodology and must be re-measured or reworded (Phase 4).

**Phase 0 — Fix Sprint 33 regressions & add an `-O` sweep harness (blocks everything else) — ✅ DONE (2026-10-20)**
1. ~~Add `tools/opt_sweep.py` …~~ — **done**: `tools/opt_sweep.py` sweeps every standalone test in `test/{jit,aot,licm}` at `-O0/-O1/-O2/-O3` in JIT (`--run`) and AOT (`-o`), comparing `(rc, stdout)` to the `-O0` baseline (plus an informational `gcc -O2` diff), with per-test timeouts and `NONDET`/skip flagging (e.g. `profile_demo`). Wired into `make test-opt` and the `AGENTS.md` Verification Rules.
2. ~~Root-cause and fix each regression …~~ — **done**: the five root causes are fixed (x86 extend/setcc ModRM encoding ×3, LICM shared-header invariant loss, LICM in-loop source-def check, DCE fixpoint liveness, mem2reg conservative slot audit + full post-loop forwarding). See the "Former Open Regressions — FIXED" table above for the per-test mapping. The fix went beyond the originally planned "conservative guard": each pass was corrected rather than disabled, so `-O2`/`-O3` still perform loop promotion + LICM hoisting.
3. ~~Triage `profile_demo` …~~ — **done**: it is timing/nondeterminism-sensitive (wall-clock output); the sweep flags it `NONDET` and does not hard-fail.
- **Exit (met)**: `make test-opt` **88/88** across all four levels, JIT and AOT; `mem2reg_verifier.py --verify-all` 12/12; `sret_verifier.py` 6/6; `make test` 141/141.

**Phase 1 — Inlining audit — ✅ DONE (2026-10-20)**
1. ~~Adversarial differential tests vs `gcc -O2`, JIT + AOT at `-O2`/`-O3` …~~ — **done**: 17-test battery in `test/inline_audit/` (param mutation, `&param`, pointer mutation, narrow/wide args, float/double args & returns incl. FP-expression return, static locals, labels/goto, multiple returns, same callee twice in one expression, nested depth 2, struct params/returns ≤8B, mixed inlined+real calls), run JIT+AOT at `-O0..-O3` against a `gcc -O2` reference.
2. ~~Conservative candidate filters in `jit/compile.h` …~~ — **done**. The audit found **four inlining-specific miscompile classes** (all at `-O2/-O3`, all absent from the pre-existing corpus): (a) **float/double params or return** — the param-substitution path forces `last_expr_is_float=0` and hands the caller's arg temp (an XMM value) to the body as an integer (FP return redirection went through a GPR `MOV_REG`); (b) **local declarations in the body** — locals are registered at *parse time* into the callee's per-function `c->locals`, which is reset per function, so at the call site body locals lower to `TEMP_NONE` (loop counters vanished, `big(4)` returned garbage); (c) **labels/goto in the body** — user labels resolve through a *global name-based* map (`get_or_create_user_ir_label`), so a same-named caller label aliases the inlined body's target; (d) **`&param`** — value params bind to virtual registers with no stack slot; `&param` lowered to an invalid LEA and segfaulted. Filters added: reject FP signatures, `zero_cc_ast_has_local_decl`, `zero_cc_ast_has_label_goto`, and `zero_cc_ast_addr_of_param` (new AST walkers in `parser/ast_arena.h` + prototypes in `parser.h`). Verified still inlined after the filters: expression-only leaf helpers (`sq`, `min`-style), struct params/returns ≤8B, multiple returns, real calls from the inlined body, nested depth 2, same callee twice in one expression. **Known pre-existing gap found by the audit (not inlining)**: `static` locals lower as ordinary stack locals (re-initialised every call) — `10_static_local.c` fails at *every* `-O` level including `-O0` (tracked as a frontend gap, kept in the corpus as a consistency guard; its REF-DIFF vs gcc is informational).
3. ~~Add the passing cases to the `opt_sweep.py` corpus.~~ — **done**: `test/inline_audit/` is now part of the sweep corpus (105 tests).
- **Exit (met)**: all 17 adversarial tests match gcc at every level *except* the documented pre-existing static-local gap (which fails identically at `-O0`, i.e. without inlining).

**Phase 2 — CFG layer** (`opt/cfg.{h,c}`) — ✅ DONE (2026-10-20)
1. ~~Build basic blocks (leaders: labels, instruction after jump/ret), successor/predecessor lists, reachability; helpers for loop headers/back-edges.~~ — **done**: `opt/cfg.{h,c}`. Blocks tile `[start,end)` over the flat IR (leaders: instruction 0, every `OP_LABEL`, instruction after any terminal `JMP/JE/JNE/JL/JLE/JG/JCOND/RET/EPILOGUE`). Successors from the last instruction of each block (explicit target + fallthrough for branches, none for `RET`/`EPILOGUE`); preds derived; reachability by BFS from block 0; back-edge detection `blocks[v].start <= blocks[u].start` (matching the natural-loop convention the legacy passes use) with `is_loop_header` marking. **Note**: zerocc does not emit an `OP_EPILOGUE` instruction — the epilogue is the final `CALLEE_SAVE_LOADS` block; the backend appends `leave`/`ret` outside the IR, so the last block legitimately has no successors.
2. ~~Lazy cache on `ZeroCCCompiler` with explicit invalidate API.~~ — **done**: `ZeroCCCFG cfg_cache` + `uint64_t cfg_version` in `ZeroCCCompiler`; `cfg_build()` rebuilds only when `cfg_version` mismatches; invalidation hooks in `ir_reset()` (per function), after every optimizer pass (`opt_pass.c`), and after the two in-place IR mutations in lowering (callee-save append, x86 float-CC remap). Not built at `-O0` unless requested.
3. ~~`-dump-cfg` flag …~~ — **done**: `-dump-cfg` in `main.c` (both AOT and JIT paths), emitting exactly the planned format: `fn main` / `bb1: [4..10] succ=bb8,bb2 pred=bb0,bb7 loop_header=1` / `loops:` section with `bb7->bb1` lines and an `N blocks, M reachable, K loops` summary.
4. ~~`tools/cfg_verifier.py` …~~ — **done** (stdlib-only). One `zerocc -dump-ir -dump-cfg` run per test; the Python side independently rebuilds blocks/loops from the *IR* dump (separate code, shared opcode rules only) and cross-checks: block tiling, jump-target-is-block-start, pred/succ mutual consistency, reachability (BFS + "no unreachable block with a reachable pred"), and the C-vs-Python loop-set equality as `(header_pos, from_pos)` pairs. `--verify-all` (sweep corpus + `test/cfg`), `--file`, `--dot` text export, `--opt`. **Deviation from plan**: genuinely unreachable blocks (unreachable tails after `return`s, orphaned constant blocks the lowering emits) are reported as WARNINGS, not failures — they are a property of the existing codegen (present at `-O0`), not of the CFG layer; a block with a *reachable* predecessor that is itself unreachable is still a hard failure.
5. ~~Unit tests …~~ — **done**: `test/cfg/` — `straight.c`, `if_else.c`, `nested_loops.c` (2 nested back-edges), `goto_mix.c`, `switch_case.c`, `ternary_in_loop.c`.
- **Exit (met)**: `cfg_verifier.py --verify-all` over the full standalone corpus: **0 failed** (63 SKIPped tests are exactly the non-standalone API/header tests the sweep also cannot AOT-compile — identical set, pre-existing frontend gaps, not CFG issues); all 48 verifiable functions pass every check incl. the independent loop cross-check; negative tests confirm the verifier catches tiling breaks, pred/succ breaks, and loop-set mismatches. `make test-opt` 105/105, `make test` 141/141, `mem2reg_verifier.py` 12/12, `sret_verifier.py` 6/6.

**Phase 3 — Pass migration (known-fragile passes first)**
1. Mem2reg loop promotion (`opt_promote_loop_carried`): replace the jump-target pre-scan and its loop heuristics with CFG predecessors/back-edges; require a single-entry natural loop (the Sprint 33 failure class).
2. LICM: replace `label_to_idx` loop detection with CFG back-edges; keep the multi-def invariant and the call-spanning guard.
3. Vectorizer: replace the span-based mid-body-conditional check (`5766bee`) with a CFG "single basic-block body" check.
- **Exit**: `opt_sweep.py` zero diffs at `-O0..-O3` (JIT + AOT), `mem2reg_verifier.py --verify-all`, `sret_verifier.py --verify-all`, `licm_verifier.py`, no output regressions.

**Phase 4 — Measurement & docs**
1. `--profile-opts` before/after on a large synthetic function: per-pass and total compile time at `-O0/-O1/-O2`.
2. Re-measure and correct the JIT-vs-GCC compile-time claim (state workload, GCC invocation, and what is timed).
3. Update OKF (`ir-and-regalloc.md`, `optimization-passes.md`, incl. removing the stale vectorizer-residual text), `fragility-analysis.md` §C11, CHANGELOG (add the missing entries for `8781592`, `5766bee`, and the Sprint 33 regression); `okf.py lint && index`.

**Deferred (revisit after Phase 4)**
- Promotable-slot SSA (phis for non-address-taken stack slots only) replacing `opt_promote_loop_carried`; requires dominators and out-of-SSA with coalescing.
- Full SSA over all temps (rewrite-scale; only if multi-def reasoning in GVN/CSE blocks further passes).
- Branchless `select`/cmov lowering, loop unrolling, IR-level inlining — all become tractable on the CFG.
- IR cleanups: call args into the instruction stream (remove `instr_index`-keyed side table), move physical regs out of the temp namespace, add type tag.
