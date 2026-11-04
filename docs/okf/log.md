---
type: Log
title: ZeroCC OKF Knowledge Bundle Log
description: Sequential changelog tracking how and when the ZeroCC OKF knowledge bundle evolves alongside compiler sprints.
resource: okf/log
tags: [zerocc, changelog, log, okf]
timestamp: 2026-11-04T22:19:16Z
generated: antigravity-agent
verified: engineering-lead
status: active
---

# ZeroCC OKF Bundle Log

This file tracks sequential updates to the [ZeroCC OKF Knowledge Graph](./index.md).

## 2026-10-15 — Initial OKF v0.2 Bundle Creation & Sprint 17 (`sret` + Function Pointers)
- **Initialized OKF Bundle (`docs/okf/`)**:
  - Created [index.md](./index.md) as the root navigator for AI coding agents.
  - Created single-concept nodes covering the full compiler architecture:
    - [architecture-pipeline.md](./architecture-pipeline.md)
    - [frontend-and-types.md](./frontend-and-types.md)
    - [ir-and-regalloc.md](./ir-and-regalloc.md)
    - [optimization-passes.md](./optimization-passes.md)
    - [abi-sysv-x86_64.md](./abi-sysv-x86_64.md)
    - [isa-backends.md](./isa-backends.md)
    - [build-and-verification.md](./build-and-verification.md)
- **Recorded Sprint 17 System V AMD64 `sret` Invariants**:
  - Documented hidden return pointer (`%rdi`) handling for structs `>16` bytes across direct calls, member access on call expressions, struct reassignment, tail returns, and indirect function pointer calls (`Task 4.3`).
  - Documented size-aware tail chunk copy (`32`/`16`/`8`-bit load/store) to prevent stack clobbering on non-multiple-of-8 structs (such as 20-byte structs).
  - Documented `tools/okf.py` for validating YAML frontmatter, relative markdown graph links, ranked search, and portable skill setup.

## 2026-10-16 — Updated `isa-backends.md`
- Documented Multi-ISA sret (AArch64 x8 and RISC-V a0) calling conventions and codegen

## 2026-10-16 — Updated `isa-backends.md`
- Multi-ISA SIMD vector emission across x86-64 SSE, AArch64 NEON, and RISC-V RVV

## 2026-10-16 — Updated `optimization-passes.md`
- Auto-vectorization pass safety invariants and loop transformation across ISAs

## 2026-10-16 — Updated `build-and-verification.md`
- Multi-ISA sret cross-verification across x86-64, AArch64, and RISC-V under QEMU

## 2026-10-16 — Updated `optimization-passes.md`
- Expand auto-vectorization pass to support packed 64-bit double precision SIMD loops (OP_VADDPD, OP_VSUBPD, OP_VMULPD, OP_VDIVPD, vec_width=2) across x86-64, AArch64, and RISC-V.

## 2026-10-16 — Updated `optimization-passes.md`
- Add backend peephole optimization: eliminate redundant register self-moves in AOT assembly and JIT machine code, and suppress synthetic OP_NOP assembly comments.

## 2026-10-16 — Updated `frontend-and-types.md`
- Recorded Known Defect: 2-D/3-D array declarations silently miscompiled (parser single-bracket + NODE_INDEX missing pointer decay); plan in docs/MULTIDIM_VLA_PLAN.md

## 2026-10-16 — Updated `optimization-passes.md`
- Implement 32-bit packed integer SIMD auto-vectorization across x86-64 SSE2/SSE4.1, AArch64 NEON, and RISC-V RVV 1.0

## 2026-10-17 — Updated `frontend-and-types.md`
- Multi-dimensional arrays and matrix VLAs (Phase 0 and Phase 1) fully supported

## 2026-10-17 — Updated `frontend-and-types.md`
- Implement C11 core keywords (_Alignas, _Alignof, anonymous structs/unions, _Generic), C11 <threads.h> & atomics, and global variable value load/assignment/ELF object linkage

## 2026-10-17 — Updated `optimization-passes.md`
- Add LICM call-spanning safety invariant to prevent hoisting constants across loops with function calls

## 2026-10-17 — Updated `build-and-verification.md`
- Update build-and-verification guide with 140-test suite, C11/VLA verifiers, and curated tools/ vs tools/archive/ layout

## 2026-10-17 — Updated `index.md`
- List C11 and VLA verifier suites in root OKF index

## 2026-10-17 — Updated `frontend-and-types.md`
- Unify ZeroCCType and TypeInfo construction and AST type queries via canonical type_info.h bridge

## 2026-10-17 — Updated `architecture-pipeline.md`
- Convert ast_lowering.c, ast_lowering_stmt.c, ast_lowering_expr.c, ast_lowering_mc.c, asm_emitter.c, and opt_pass.c from unity-included .c files into standalone .o translation units

## 2026-10-17 — Updated `architecture-pipeline.md`
- Split src/zero_cc/parser/ into parser/ (AST construction), ir/ (IR, lowering, regalloc, codegen), and opt/ (optimization passes)

## 2026-10-17 — Updated `ir-and-regalloc.md`
- Mirror scratch-register spill reload/store in asm_emitter.c, enforce callee-saved-only allocation for spans_call intervals, and emit callee-save stores in emit_prologue_mc

## 2026-10-17 — Updated `ir-and-regalloc.md`
- Unified register allocator spill/reload across JIT/AOT and enabled prologue callee-saved register stores (OP_CALLEE_SAVE_STORES)

## 2026-10-17 — Updated `frontend-and-types.md`
- Verified standalone C99/C11 compilation across 29/29 test files: multi-level pointers, opaque struct typedefs, struct array members, nested brace initializers, multi-declarator extern, and arbitrary va_arg types

## 2026-10-17 — Updated `global-data-relocs.md`
- Add global variables & data reloc concept page: file-scope initializers, ZeroCCDataReloc (JIT patch + AOT .rela.<sec>), ELF writer multi-section reloc support

## 2026-10-17 — Updated `frontend-and-types.md`
- Document float/double struct member tags, NODE_MEMBER s.str/aux union aliasing gotcha, goto/label/switch implementation (CMP/COND_SET/JNE dispatch, break via label_patcher)

## 2026-10-17 — Updated `isa-backends.md`
- Document OP_JE/OP_JNE jump-if-zero/nonzero semantics and JIT global data reloc patching

## 2026-10-17 — Updated `scalar-float-x86.md`
- Added scalar-float-x86 page: double-width FP model, VLOADSS/SD emitters, XMM/VEC classing, WSL2 128-bit RX-read GPF workaround

## 2026-10-17 — Updated `frontend-and-types.md`
- Sprint 22: signedness tags end-to-end, narrow-int width extension at value boundaries, both param parser paths

## 2026-10-17 — Updated `isa-backends.md`
- Sprint 22: OP_SEXT/ZEXT emitters all ISAs, x86 byte-register ModRM trap (verified against GNU as), GAS syntax limits

## 2026-10-18 — Updated `ir-and-regalloc.md`
- C2a: temp-id exhaustion guard (silent miscompilation -> clean diagnostic); temp-id space documented; C2b (16-bit widening) pending

## 2026-10-18 — Updated `ir-and-regalloc.md`
- C2b: temp ids widened to 16-bit (TEMP_NONE=0xFFFF sentinel), LRA 256-cap removed, base-register markers moved to TEMP_NONE, OP_CALL side table is sole arg source

## 2026-10-18 — Updated `frontend-and-types.md`
- Resolve C5 multi-segment member chains through pointer members and width-safe pointer deref stores

## 2026-10-18 — Updated `scalar-float-x86.md`
- Resolve C6 scalar float stack load/store (OP_FLOAD/OP_FSTORE) and FP call arg emission in AOT -S path across x86-64, AArch64, and RISC-V

## 2026-10-18 — Updated `architecture-pipeline.md`
- Resolve H2: ensure compiler diagnostics and syntax errors halt compilation cleanly across JIT and AOT modes

## 2026-10-18 — Updated `frontend-and-types.md`
- Enforce 127-char identifier limit with clean fatal diagnostic (M1 resolved)

## 2026-10-18 — Updated `architecture-pipeline.md`
- Resolve H1: raise stack limit to 64MB and guard expression recursion depth with clean diagnostic

## 2026-10-18 — Updated `isa-backends.md`
- Add OP_CVTSI2SD/OP_CVTTSD2SI asm emission and -S .data section emission across ISAs

## 2026-10-18 — Updated `ir-and-regalloc.md`
- C8 invariants: materialized FP call returns, XMM spans_call forced spill, no-steal-from-spilled guards, class-aware FP spill scratch (fp_scratch_reg1/2 = 15/14), XMM-VEC pool cross-marking

## 2026-10-18 — Updated `optimization-passes.md`
- C11 invariant: auto_vectorize rejects loops whose body contains OP_CALL/OP_CALL_INDIRECT/OP_RET (has_call guard); documented residuals (mid-body branches, increment-rewrite breadth)

## 2026-10-18 — Updated `scalar-float-x86.md`
- Sprint 30: -S float parity (VLOADSS/VSTORESS width, 3-operand FP, ucomisd compares, CMP_ZERO); C12 ucomisd flag mapping; C13 REX-before-prefix invariant

## 2026-10-19 — Updated `frontend-and-types.md`
- Replace Scope TPHT with dynamically resizing separate-chaining hash table, eliminating scope symbol limits

## 2026-10-19 — Updated `ir-and-regalloc.md`
- Document spill philosophy: linear-scan speed vs graph-coloring trade-offs, scratch registers, and mem2reg division of labor

## 2026-10-19 — Updated `optimization-passes.md`
- Document mem2reg loop stack traffic impact and cross-block optimization roadmap

## 2026-10-19 — Updated `optimization-passes.md`
- Document cross-block mem2reg implementation and LICM invariant stack load hoisting

## 2026-10-19 — Updated `optimization-passes.md`
- feat(opt): small function inlining at -O2/-O3 and LICM multi-def safety invariant

## 2026-10-19 — Updated `ir-and-regalloc.md`
- docs(ir): record IR limitations and CFG-first decision (Sprint 35 plan)

## 2026-10-19 — Updated `optimization-passes.md`
- docs(opt): correct stale vectorizer residuals, document -O levels, record Sprint 33 regressions

## 2026-10-19 — Updated `optimization-passes.md`
- Sprint 35 Phase 0: resolved Sprint 33 regressions - LICM shared-header union emission + in-loop source-def invariant, DCE fixpoint liveness, mem2reg conservative slot audit & full post-loop forwarding; make test-opt 87/87

## 2026-10-19 — Updated `build-and-verification.md`
- Documented make test-opt / tools/opt_sweep.py as the mandatory optimizer regression gate

## 2026-10-19 — Updated `isa-backends.md`
- Documented x86 ModRM source/destination placement invariants (setcc dest in r/m; cross-register extend src in r/m) from Sprint 35 Phase 0 fixes

## 2026-10-19 — Updated `ir-and-regalloc.md`
- Corrected opcode count 51 -> 127

## 2026-10-19 — Updated `optimization-passes.md`
- Added loop entry-cleanness guard note (goto_into_loop.c regression test); sweep count 88/88

## 2026-10-20 — Updated `optimization-passes.md`
- Sprint 35 Phase 1: inlining audit - four miscompile classes found (FP signatures, body locals, labels/goto, &param); conservative candidate filters added; 17-test differential battery in corpus

## 2026-10-20 — Updated `ir-and-regalloc.md`
- Sprint 35 Phase 2: CFG layer (opt/cfg.{h,c}) - basic blocks, pred/succ, reachability, backedge loops; versioned lazy cache; -dump-cfg; cfg_verifier.py cross-check

## 2026-10-20 — Updated `build-and-verification.md`
- Sprint 35 Phase 2: add cfg_verifier.py gate and -dump-cfg examples; sweep corpus now 105 tests

## 2026-10-20 — Updated `optimization-passes.md`
- Sprint 35 Phase 3: LICM/mem2reg/vectorizer migrated onto the CFG; vectorizer revived with verified-constant-loop-bound prerequisite

## 2026-10-20 — Updated `ir-and-regalloc.md`
- Sprint 35 Phase 3: pass migration complete; no private loop re-derivations remain in LICM/mem2reg/vectorizer

## 2026-10-20 — Updated `perf-measurements.md`
- Sprint 35 Phase 4: measured per-pass optimizer costs (CFG migration = no regression, 7-10% faster at O2/O3); retracted the 78x JIT-vs-GCC claim with re-measured matrix; recorded 3 fixed 16-bit temp-id OOB bugs (GVN/vectorizer/LICM).

## 2026-10-20 — Updated `ir-and-regalloc.md`
- Sprint 35 Phase 4: retracted 78x JIT claim, linked perf-measurements.md.

## 2026-10-20 — Updated `perf-measurements.md`
- Sprint 36: mem2reg loop promotion restructured to one accumulating scan per loop (O(loops x |IR|)); 1088->3.2ms @O1, 3374->13.8ms @O2 on synth_large.c; -O1 dump-ir byte-identical; GVN is the new dominant pass.

## 2026-10-20 — Updated `frontend-and-types.md`
- Sprint 37: block-scope static locals (data-section allocation + per-call stack mirror; constant-literal init patched at parse time)

## 2026-10-20 — Updated `ir-and-regalloc.md`
- Sprint 37: epilogue hazard note - allocator temps may map onto the return register after return materialization; shield VREG_0 through the frame

## 2026-10-20 — Updated `optimization-passes.md`
- Sprint 38: GVN generation-stamped canonical table (O(1) block reset); corrected pass description

## 2026-10-20 — Updated `perf-measurements.md`
- Sprint 38: GVN resolved (200ms -> 2.3ms); next hotspot LICM

## 2026-10-20 — Updated `optimization-passes.md`
- Sprint 39: branchless conditional select pass (-O3, CFG predecessor checks, -S-safe mask opcodes) + copy_prop generic definition-invalidation fix; known-issues section (FP ternary, 64-bit literals, -S emitter gaps)

## 2026-10-20 — Updated `frontend-and-types.md`
- Sprint 40: array param adjustment, N-D/struct initializer leaf placement, multi-declarator arrays, multiple VLAs, FP conditional expressions, hex literal classification

## 2026-10-20 — Updated `isa-backends.md`
- Sprint 40: -S in-place ALU opcode emitters (ADD_IMM/SUB_IMM/INC/DEC/NOT/LNOT) on all three backends

## 2026-10-20 — Updated `optimization-passes.md`
- Sprint 40: all three Sprint 39 known issues resolved (FP ternary, 64-bit hex literals, -S emitters)

## 2026-10-20 — Updated `global-data-relocs.md`
- Sprint 41: block-scope static locals complete (unsized-array up-front sizing, mirror-direction fix, pointer-to-string initializers via ZeroCCDataReloc), const-eval casts, and stdarg.h va_* -> __builtin_va_* fix

## 2026-10-20 — Updated `build-and-verification.md`
- Documented the stale-build guard (zero_cc_ast_node_size vs sizeof(ZeroCCASTNode) layout check in main.c) and its full-rebuild requirement; added the missing parser.h prototype that silenced the implicit-declaration warning

## 2026-10-20 — Updated `isa-backends.md`
- Sprint 41: documented vtable signature conformance (RISC-V load/store/lea_rip now match their ZeroCCISABackend slots; a mismatch reads garbage for the extra params)

## 2026-10-20 — Updated `build-and-verification.md`
- Sprint 41: noted pre-existing AArch64 sret --target-all failure (sxtw xD,xD source must be a W register); riscv64 + x86-64 pass

## 2026-10-20 — Updated `build-and-verification.md`
- Sprint 41: AArch64 --target-all sxtw bug FIXED (aarch64_asm.c emit_extend_asm now emits x<dst>,w<src> for width-4); noted the separate broken aarch64 binary path (emit_extend encodings + -mabi=lp64d link)

## 2026-10-20 — Updated `build-and-verification.md`
- Sprint 41: aarch64/riscv -o and -c now route through the text emitter + system cross toolchain (raw binary emitter incomplete); cross-target links use -static + -mabi=lp64

## 2026-10-20 — Updated `isa-backends.md`
- Sprint 41: documented cross-target (aarch64/riscv) -o/-c text routing + the incomplete raw binary emitter

## 2026-10-20 — Updated `ir-and-regalloc.md`
- Sprint 41: documented &&/|| straight-line lowering (TK_LOGAND/TK_LOGOR now lower to CMP_ZERO+COND_SET + AND/OR; was default-arm left-operand-only)

## 2026-10-20 — Updated `optimization-passes.md`
- Sprint 41: inliner now rejects value-param writes (zero_cc_ast_assigns_to_param); param writes were dropped during inlining

## 2026-10-20 — Updated `isa-backends.md`
- Sprint 41: OP_CMP_ZERO is ISA-specific in -S (x86 testq / aarch64 cmp r,#0 / riscv mv t0,r); was x86-hardcoded

## 2026-10-20 — Updated `build-and-verification.md`
- Sprint 41: make test-isa-qemu raw-emitter QEMU differential gate + QEMU/cross-gcc dependency

## 2026-10-20 — Updated `isa-backends.md`
- Sprint 41: measured raw-emitter state under QEMU (x86-64 11/11, aarch64 1/11, riscv64/32 0/11) via ZC_RAW_AOT

## 2026-10-21 — Updated `isa-backends.md`
- Document RISC-V raw emitter B/J/S immediate fixes, ELF HI20/LO12 relocation pairs, and comparison live-range tracking

## 2026-10-21 — Updated `build-and-verification.md`
- Document T1 (riscv_enc_check), T2 (reloc_pair_check), and T3 (vtable_check) verification tools

## 2026-10-21 — Updated `isa-backends.md`
- Document raw-emitter verification tools and riscv32 130 active NULL vtable slots finding

## 2026-10-21 — Updated `isa-backends.md`
- Fixed three RISC-V raw-emitter root causes: (1) internal call jal patching at codegen time instead of bare R_RISCV_CALL relocation; (2) RV64 prologue/leave/push/pop/store use sd/ld (f3=3) instead of sw/lw (f3=2); (3) completed RV32 backend vtable wiring all 144 slots using shared RV64 implementations and NULL for vector ops. RV64 text path verified (10_retcode.c returns 79). Remaining RAW emitter issues: startup code hangs (_dl_start→abort), rodata relocations, RV32 asm emitters need 32-bit variants.

## 2026-10-21 — Updated `isa-backends.md`
- Fixed RAW AOT startup for RISC-V: added custom _start code in elf_emitter.h that sets up stack, calls main via jal, and exits via sys_exit (a7=93). Bypasses system linker for ZC_RAW_AOT=1 by adding compile_and_emit_exe path in main.c. Core fixes: (1) internal call jal patching at codegen time; (2) RV64 prologue/leave sd/ld (f3=3); (3) RV32 vtable completion (144 slots). Verified: make test (142/142), make test-opt (106/106), sret_verifier (6/6), RV64 text path (10_retcode.c=79).

## 2026-10-22 — Updated `isa-backends.md`
- Document RISC-V 64 raw AOT execution fixes (slli f3=1, param_save_int -slot(s0), ld f3=3, 10_retcode/11_retstruct passing under QEMU)

## 2026-10-22 — Updated `isa-backends.md`
- Complete RISC-V 64 raw AOT binary emitter and fix psABI relocations (11/11 passing under QEMU)

## 2026-10-22 — Updated `build-and-verification.md`
- Update measured state for raw emitter QEMU gate: riscv64 11/11 passing

## 2026-10-22 — Updated `isa-backends.md`
- Achieve 44/44 green cross-ISA QEMU gate: complete AArch64 and RISC-V 32 raw AOT emitters

## 2026-10-22 — Updated `build-and-verification.md`
- Update cross-ISA raw emitter QEMU gate status to 44/44 green across all 4 target ISAs

## 2026-10-22 — Updated `isa-backends.md`
- Document RAW binary AOT emission default for all 4 ISAs and ZC_TEXT_AOT escape hatch

## 2026-10-22 — Updated `build-and-verification.md`
- Document RAW binary AOT default and cross-ISA QEMU verification instructions

## 2026-10-22 — Updated `frontend-and-types.md`
- Document C23 typeof/typeof_unqual, {} empty initializers, and __STDC_NO_COMPLEX__

## 2026-10-22 — Updated `optimization-passes.md`
- Document LICM fixed-register protection invariant and mem2reg redefinition slot invalidation

## 2026-10-22 — Updated `frontend-and-types.md`
- Preprocessor include-path conformance (Sprint 42): main-file-dir quoted include resolution (C99 6.10.2p2) via zero_cc_preprocess_ext2, missing-include hard error with gcc-style diagnostic, __FILE__ main-file expansion, legacy API contract preserved

## 2026-10-22 — Updated `build-and-verification.md`
- Project-header include convention (Sprint 42): single include-root relative style (-Isrc -Iinclude); 262 non-canonical project includes across 74 files normalized; ../ and bare-sibling project includes disallowed

## 2026-10-22 — Updated `build-and-verification.md`
- Documented include-order prescription and the flat include/ design decision (Sprint 42): why include/<project>/ subdirectory is deliberately not used (threads.h system emulation, single umbrella API) and the revisit trigger (second public header family, e.g. zero_rt); new docs/coding_style.md as the repo style reference

## 2026-10-22 — Updated `build-and-verification.md`
- Added tools/style_check.py (ruff-style C convention checker codifying docs/coding_style.md: include rules, whitespace, EOF newline, header guards, <module>_<action> public-function prefix NAM001, unbounded-string-call warnings) to the verification tool list; corrected the naming convention to the actual per-module prefix scheme; trimmed 921 trailing-whitespace lines + 4 missing EOF newlines across 58 files

## 2026-10-22 — Updated `architecture-pipeline.md`
- Pipeline stage 3 rewritten: the live semantic pass is sem_analyze_ref over each function body from jit/compile.h (new diagnostics set had_error); documented the four enforced C99 constraint violations (6.7.5.3p6 arity via TypeInfo.function.is_prototype / ZeroCCFunction.has_prototype, 6.8.4.3p1 goto labels via sem_collect_labels, 6.5.2.1p3 struct members, 6.8p10 block terminator) and the tag-aware AST-walker requirement (aux.step/aux.alt alias the payload union on non-FOR/IF/TERNARY tags)

## 2026-10-22 — Updated `frontend-and-types.md`
- NODE_CALL now enforces C99 6.7.5.3p6 argument counts against prototype types (is_prototype); unprototyped/enum conversions still accepted

## 2026-10-22 — Updated `architecture-pipeline.md`
- Added 'did you mean' suggestion machinery to the semantic stage: diag_levenshtein (early exit at 3) + diag_best_match (distance <= 2 and <= max(1,len/2), unique-best only — ties suppressed), candidates from scope_chain_names (scope.c) for identifiers and the field table for members; test/semantic/did_you_mean_test.c

## 2026-10-22 — Updated `build-and-verification.md`
- Added tools/diag_battery.py to the verification tool list: differential compiler-diagnostics gate vs gcc over the test/diag/*.c broken-program corpus, category-based comparison with a ratcheting baseline (test/diag/baseline.json); the validation gate for the pending multi-error-recovery work

## 2026-10-22 — Updated `frontend-and-types.md`
- SemContext single-canonical-definition invariant (decl.h/expr.h stale 8-field copies removed after the Sprint 42 multi-error-recovery work exposed a dormant cross-TU ABI stack overflow)

## 2026-10-22 — Updated `build-and-verification.md`
- zero_cc_compiler_free public destroy API; asan-semantic 16/16 (LeakSanitizer-clean semantic harnesses); make asan documented as non-gate with the known remainder

## 2026-10-22 — Updated `build-and-verification.md`
- make asan now a green tracked gate (147/147) after the full teardown sweep; per-compile struct member array teardown documented in frontend-and-types

## 2026-10-22 — Updated `frontend-and-types.md`
- Struct member array lifetime: freed per-compilation by zero_cc_free_all_struct_arrays while the type pool is live

## 2026-10-22 — Updated `multi-tu.md`
- New page: multi-TU driver (T1), JIT image merge, registry snapshot discipline, extern-object load/store, unnamed prototype params

## 2026-10-23 — Updated `own-linker.md`
- T2 own linker: ELF64 reader + ar reader + static ET_EXEC output (--zerocc-link), 14/14 differential gate, latent shndx emitter bug fixed

## 2026-10-23 — Updated `multi-tu.md`
- cross-TU &global pointer initializers fixed: kind-2 by-name data reloc (emitter + AOT UND symbol + JIT merged-registry patch); 21/21 driver, 15/15 linker

## 2026-10-23 — Updated `own-linker.md`
- cross-TU pointer-init limitation closed (see multi-tu.md)

## 2026-10-23 — Updated `ir-and-regalloc.md`
- OP_SYSCALL: __zerocc_syscall OS-boundary builtin (Phase 0 prerequisite) - OP_CALL arg packing, call-position clobber semantics, parallel-move invariant, x86-64 only

## 2026-10-23 — Updated `zero-rt.md`
- Zero_RT Phase 0 (Linux x86-64): runtime compiled by zerocc, --zerocc-link auto-link, zeroc_main_wrapper entry routing

## 2026-10-23 — Updated `frontend-and-types.md`
- String-literal initialization of char arrays (C11 6.7.9p13): parse-time byte expansion, semantic admission, local lowering, symbol-index invariant

## 2026-10-23 — Updated `global-data-relocs.md`
- File-scope string init: unsigned char arrays + struct char-array members copy bytes (not pointers)

## 2026-10-23 — Updated `own-linker.md`
- _start stub target routing: main vs zero_rt zeroc_main_wrapper

## 2026-10-23 — Updated `index.md`
- Add Zero_RT concept page

## 2026-10-23 — Updated `optimization-passes.md`
- Sprint 42: auto-vectorizer unit-stride & value-chain hardening (classify_addr_temp, value_is_vec_chain, has_int_divmod, resolve_const, loop_var_temp reuse) - fixes -O3 miscompiles on v[2*i] and integer div/mod loops

## 2026-10-23 — Updated `frontend-and-types.md`
- Sprint 43: char-array string-init truncation diagnostic (local/static-local/file-scope) + static-local copy clamp + diag_report_warning va_list-UB fix

## 2026-10-23 — Updated `frontend-and-types.md`
- Implement C-only declarator state machine stack in zero_cc_declarator_stack.h and swap into ast_stmt.h

## 2026-10-23 — Updated `frontend-and-types.md`
- Implement C-only declarator state machine in zero_cc_declarator_stack.h and swap into ast_stmt.h

## 2026-10-23 — Updated `frontend-and-types.md`
- Add C literals specification and test suite tools/literal_verifier.py

## 2026-10-23 — Updated `isa-backends.md`
- Add selective callee-saved register spilling and fallthrough jump elimination in prologue/epilogue

## 2026-10-23 — Updated `build-and-verification.md`
- Document prologue_epilogue_verifier.py tool

## 2026-10-23 — Updated `frontend-and-types.md`
- Implement high-ROI GNU/Clang extensions (__builtin_offsetof, case ranges, init ranges, statement expressions)

## 2026-10-24 — Updated `isa-backends.md`
- Document Red Zone, inline epilogue, frameless leaf functions, and extension verifier

## 2026-10-24 — Updated `isa-backends.md`
- Document Red Zone, inline epilogues, and frameless leaf optimizations

## 2026-10-24 — Updated `isa-backends.md`
- Document frameless leaf parameter promotion optimization

## 2026-10-24 — Updated `ir-and-regalloc.md`
- Document OP_PARAM_LOAD opcode and semantics

## 2026-10-24 — Updated `isa-backends.md`
- Document frame pointer omission on fixed-frame functions (-fomit-frame-pointer): per-ISA FP-less prologues/epilogues, frame-slot SP rebasing via zero_cc_frame_base_reg/zero_cc_frame_disp, and RISC-V immediate-range fallback

## 2026-10-24 — Updated `ir-and-regalloc.md`
- Document frame-slot base rebasing (FP-relative IR offsets resolved via frame_delta when the frame pointer is omitted)

## 2026-10-24 — Updated `build-and-verification.md`
- prologue_epilogue_verifier.py now covers omit-frame-pointer shapes and SP-based CSR symmetry

## 2026-10-24 — Updated `frontend-and-types.md`
- Sprint 45: unified C99 6.7.8/6.7.9 aggregate-initializer engine (shared designator/positional resolution, AST abs-stamps, recursive global writer, cursor advancement, string-fill semantics)

## 2026-10-24 — Updated `tls-static.md`
- Sprint 46: static TLS (_Thread_local) end-to-end - OP_TLS_LEA initial-exec idiom, .tdata/.tbss packing, TPOFF32 relocations, -S/ELF/JIT paths

## 2026-10-24 — Updated `index.md`
- Link tls-static concept node

## 2026-10-24 — Updated `abi-sysv-x86_64.md`
- Sprint 47: FP-arg load ordering invariant in OP_CALL (FP loads before stack-arg pushes; fixes float mispass on 7+ int args)

## 2026-10-24 — Updated `frontend-and-types.md`
- Sprint 48: ZERO_CC_MAX_PARAMS (32) and parameter parser bounds

## 2026-10-24 — Updated `abi-sysv-x86_64.md`
- Sprint 48: dynamic SP shift tracking (zero_cc_frame_disp_at) for caller stack arguments

## 2026-10-24 — Updated `frontend-and-types.md`
- Dynamic arena-allocated slices for ZeroCCFunction parameters with C99/C11 127+ parameter limit (ZERO_CC_MAX_PARAMS=256)

## 2026-10-25 — Updated `zero-rt.md`
- Sprint 49: variadic va_list ABI, unsigned div/mod/compare codegen, and zero_rt printf closeout

## 2026-10-25 — Updated `multi-tu.md`
- Document completed Toolchain T0 dependency generation (-MMD/-MF/-MT)

## 2026-10-25 — Updated `multi-tu.md`
- Sprint 50: 27 driver cases; -S default dep target uses .s basename; derive_default_output() refactor

## 2026-10-25 — Updated `multi-tu.md`
- Sprint 51: -E preprocess-only mode (gcc-parity dep defaults, -o single-input)

## 2026-10-25 — Updated `multi-tu.md`
- Sprint 52: build-system compat flags (-U/-include/-isystem + accept-and-ignore list, 37 driver cases)

## 2026-10-25 — Updated `own-linker.md`
- Sprint 53: AArch64 support (e_machine plumbing, R_AARCH64 reloc set with empirically-derived ADRP imm21 layout, bl-based _start stub, 0x400000 base; 18/18 gate incl. QEMU differentials)

## 2026-10-26 — Updated `own-linker.md`
- feat(linker): RISC-V64 support in own linker (T2)

## 2026-10-26 — Updated `zero-rt.md`
- Sprint 55: zero_rt freestanding runtime cross-platform parity across x86-64, AArch64, and RISC-V64

## 2026-10-26 — Updated `frontend-and-types.md`
- Sprint 56: virtual <stdatomic.h>/<stdnoreturn.h>/<threads.h> added to the preprocessor virtual-header set (mirroring include/ reference copies)

## 2026-10-26 — Updated `abi-sysv-x86_64.md`
- Sprint 56: non-identifier by-value struct arg (9-16B) chunk-source convention: locals from stack slot, compound literals/call results from secondary value register (rdx / x1 / a1); >16B by-value args pass by pointer

## 2026-10-26 — Updated `ir-and-regalloc.md`
- Sprint 56: NODE_CALL general-expression indirect calls (a[i](), p->cb(), (*fp)()) share the is_fp=2 indirect machinery; return type from semantic TypeInfo; pointer-to-function deref is identity

## 2026-10-26 — Updated `abi-sysv-x86_64.md`
- Sprint 56 fix: canonical secondary value register is VREG_2 on all ISAs (regalloc maps to rdx/x1/a1); compound literals and call args now use VREG_2 uniformly

## 2026-10-26 — Updated `ir-and-regalloc.md`
- Sprint 56: documented canonical VREG_2 convention for 9-16B value passing; DCE root set marks VREG_0 and VREG_2 live at exit

## 2026-10-26 — Updated `c-torture-gate.md`
- Sprint 57: GCC c-torture conformance gate (tools/torture_run.py, test/torture/baseline.json, 3940 tests: 1714 PASS, 218 MISCOMPILE, 954 REJECT, 699 SKIP-C89, 10 TIMEOUT) and initial MISCOMPILE/TIMEOUT bug fixes

## 2026-10-27 — Updated `c-torture-gate.md`
- Document chunked c-torture burn-down plan (Chunks M1-M3 for 228 TIMEOUT/MISCOMPILE, Chunks R1-R4 for 954 REJECT)

## 2026-10-27 — Updated `c-torture-gate.md`
- M1 hang-triage tooling in tools/torture_run.py (--hang-debug gdb capture, --only-class, --repro, --keep, --timeout); first findings: pr94238 hangs in zero_cc_stmt_enum, 20011126-2 is an O2 runtime infinite loop

## 2026-10-28 — Updated `c-torture-gate.md`
- Complete c-torture Chunk M1: burn down all 10 TIMEOUT cases to 0

## 2026-10-29 — Updated `c-torture-gate.md`
- Chunk M2 partial-mode MISCOMPILE burn-down: 50/67 resolved (was 78 at plan time). Fixed missing external-call symbol registration (`__builtin_*` routines now registered as external `ZeroCCFunction` entries so AOT emits PLT relocations instead of `call +0`) and added builtin lowerings (`__builtin_expect`, `__builtin_prefetch`, `__builtin_constant_p`, `__builtin_bswap32`, `__builtin_ffs`, `__builtin_inf`, `__builtin_mul_overflow`). Remaining 17 involve statement expressions + mem2reg, nested functions, float codegen, JIT-only symbol resolution, and missing builtins (`__builtin_fabsf`, `__builtin_isinf`, etc.).

## 2026-10-28 — Updated `c-torture-gate.md`
- M2 partial-mode MISCOMPILE burn-down progress: 50/67 resolved

## 2026-10-28 — Updated `log.md`
- M2 progress recorded

## 2026-10-28 — Updated `c-torture-gate.md`
- Resolve 20000917-1: dereference assignment evaluation order and multi-chunk struct copying

## 2026-10-28 — Updated `c-torture-gate.md`
- Chunk M2 continued: repaired all 25+ external-registration regressions (implicit return 0, fptr-as-extern, extern-object/function scope distinction, OP_TRAP, ctz/clz/popcount lowerings, >16B struct deref copy, 9-16B struct ident loads); 52 tests fixed (48 partial-mode M2 + 3 M3 + fp-cmp-7 improvement); gate re-baselined to true tree state: 1009->956 bad since 6215d10, M2 49/67 resolved, 0 regressions

## 2026-10-28 — Updated `c-torture-gate.md`
- Support comma-separated --only patterns and --chunk (m1/m2/m3) in torture harness

## 2026-10-28 — Updated `c-torture-gate.md`
- Resolve 6 Chunk M2 tests (overflow builtins & float branches); ratchet baseline PASS 1964->1970, MISCOMPILE 207->201

## 2026-10-28 — Updated `c-torture-gate.md`
- Resolve 5 Chunk M2 miscompiles and 1 reject; ratchet baseline to 1976 PASS, 196 MISCOMPILE

## 2026-10-28 — Updated `c-torture-gate.md`
- Complete Chunk M2 (5/5 PASS): LP64 type macros, HW division regalloc intervals, struct memory lval/rval marshaling, and function identity tracking; PASS reaches 2042

## 2026-10-28 — Updated `c-torture-gate.md`
- Chunk M3: 22 MISCOMPILEs resolved (-22 to 148, PASS +22 to 2064) via generalized lvalues, non-local va_list/va_copy, multi-chunk struct passing & prototype conversion

## 2026-10-29 — Updated `c-torture-gate.md`
- Sprint 57 Chunk M4: burn down 19 MISCOMPILEs and 15 REJECTs (PASS reaches 2101, MISCOMPILE drops to 129)

## 2026-10-29 — Updated `frontend-and-types.md`
- Integer literal typing (C99 6.4.4.1) via type_int_literal + const_eval.h sizeof(literal); mixed 32-bit signed/unsigned binops zext BOTH operands; unary minus width-marked at promoted operand width (6.5.3.3). Fixes c-torture 20020226-1/20020508-2, bitfld-4, pr49039, pr56250; baseline 828 -> 809.

## 2026-10-29 — Updated `isa-backends.md`
- AArch64 FP compare: use fcmpe + IEEE-754 condition codes (mi/ls/gt/ge) for correct NaN handling

## 2026-10-29 — Updated `frontend-and-types.md`
- Torture baseline improved: 809 -> 806 bad (3 fewer failures) via AArch64 IEEE-754 FP compare fix

## 2026-10-29 — Updated `c-torture-gate.md`
- Sprint 58: c-torture MISCOMPILE burn-down down to 81, PASS up to 2154. Fixed sizeof/alignof on arrays/strings, float constant-folding, struct array assignments, compound literal postfix increments, and struct pointer dereference copies.

## 2026-10-29 — Updated `c-torture-gate.md`
- Sprint 58: Fix named variadic macros, sizeof paren consumption, abs builtins, wide characters/strings and target endianness macros; burn down MISCOMPILE to 75 (+16 PASS)

## 2026-10-29 — Updated `c-torture-gate.md`
- Torture baseline improved: 758 -> 758 bad (44 MISCOMPILE, 0 REJECT, 0 SKIP-C89). AArch64 IEEE-754 FP compare fix.

## 2026-10-30 — Updated `c-torture-gate.md`
- Sprint 59 / Chunk M2 Final: resolved the last 7 partial-mode MISCOMPILEs (libc-builtin return typing, JIT param specifier runs, pointer width-mark exclusion); baseline re-recorded to PASS 2209 / MISCOMPILE 48 / 0 partial-mode

## 2026-10-30 — Updated `frontend-and-types.md`
- Documented libc-mapped __builtin_* return typing (type_builtin_libc_return), full specifier-run param parsing in jit/compile.h, and the pointer (&) exclusion from C99 width-mark analysis

## 2026-10-30 — Updated `ir-and-regalloc.md`
- Documented the Instr.width C99 narrow-int mark field and the pointer-result exclusion (no SEXT32 on pointer arithmetic)

## 2026-10-30 — Updated `c-torture-gate.md`
- Chunk M3 Part 0: nested-function definition diagnostic (GNU extension) — 7 execute MISCOMPILEs + 13 compile tests move to SKIP-C89; MISCOMPILE 48->41 (17 vector_size + 24 true bugs remain)

## 2026-10-30 — Updated `frontend-and-types.md`
- Documented GNU nested-function clean rejection (C99 6.9p15): definition path in statement parser errors and brace-skips; local prototypes remain legal

## 2026-10-30 — Updated `c-torture-gate.md`
- Sprint 60: __vector_size__ vector feature — all 17 vector c-torture tests pass; MISCOMPILE 41→23

## 2026-10-30 — Updated `frontend-and-types.md`
- Sprint 60: __vector_size__ vector type semantics documented

## 2026-10-30 — Updated `ir-and-regalloc.md`
- Sprint 60: vector lowering section (per-lane expansion, ABI, FP model, 136 opcodes)

## 2026-10-30 — Updated `c-torture-gate.md`
- Sprint 61: aligned(N) attribute, real overflow builtins, ffs/ctz/clz, VLA-goto crossing rejection + VLA-after-block alloc fix, 11 correctness fixes; PASS 2216->2242, MISCOMPILE 20->10, REJECT 671->658

## 2026-10-30 — Updated `frontend-and-types.md`
- Sprint 61: aligned(N) attribute support in all declaration contexts; pointer-star qualifier/marker parsing

## 2026-10-30 — Updated `ir-and-regalloc.md`
- Sprint 61: VLA hybrid rules + goto-crossing rejection, stashed-symbol VLA resolution, stmt-expr scoping, real __builtin_*_overflow detection

## 2026-10-30 — Updated `c-torture-gate.md`
- Sprint 62: MISCOMPILE 10 -> 0 (last 10 fixed: bitfld-3, pr47538, pr60017, pr53084, 20071202-1, pr66233, 920710-1, 960801-1, alias-3/4); PASS 2242 -> 2252; baseline 668 -> 658 bad

## 2026-10-30 — Updated `frontend-and-types.md`
- Sprint 62: GNU alias attribute (__zc_alias__ marker, alias-transparent find_global_var); nested multi-dim struct member types; whole-member positional initializer placement

## 2026-10-30 — Updated `ir-and-regalloc.md`
- Sprint 62: bit-field arithmetic result reduction (GCC rules + promotion exception), ++/-- bitfield RMW, global-struct by-value returns

## 2026-10-31 — Updated `c-torture-gate.md`
- Sprint 63 baseline: PASS 2393, REJECT 519, 12 newly-exposed MISCOMPILEs tracked

## 2026-10-31 — Updated `frontend-and-types.md`
- Sprint 63: file-scope struct/union declarators, const-expr member array dims, predefined macro set

## 2026-10-31 — Updated `ir-and-regalloc.md`
- Sprint 63: union member float-ness (typeinfo-first, name-before-offset)

## 2026-10-31 — Updated `c-torture-gate.md`
- Sprint 64: MISCOMPILE burn-down to zero (12+5+2 latent bugs); PASS 2,393->2,447, REJECT 519->480

## 2026-10-31 — Updated `frontend-and-types.md`
- Sprint 64: __*_TYPE__ macros, local inline aggregates, FAM initializers, bitfield value rules

## 2026-10-31 — Updated `ir-and-regalloc.md`
- Sprint 64: VREG_2 immediate-capture invariant, scalar-to-vector low-byte casts

## 2026-10-31 — Updated `abi-sysv-x86_64.md`
- Sprint 64: >16B by-value struct args use hidden-pointer convention with deferred callee copies

## 2026-10-31 — Updated `c-torture-gate.md`
- Sprint 65: file-scope REJECT burn-down (K&R, const pointer inits, long double) + 5 MISCOMPILE fixes; PASS 2,447->2,682, REJECT 480->322

## 2026-10-31 — Updated `ir-and-regalloc.md`
- Sprint 65: u64-aware int->float conversion helper (zc_emit_int_to_double), sticky-bit collapse join fix, zero-size struct phantom load/store fix

## 2026-10-31 — Updated `c-torture-gate.md`
- Sprint 66: function-pointer return typing + O2 inlining indirect-call fix + __builtin_choose_expr; PASS 2682->2708, REJECT 322->298, MISCOMPILE 3->0

## 2026-10-31 — Updated `c-torture-gate.md`
- Sprint 67: C99 complex support + function-name hash (PASS 2,708->2,789, REJECT 298->223, MISCOMPILE 0, TIMEOUT 0)

## 2026-10-31 — Updated `frontend-and-types.md`
- Sprint 67: C99 _Complex type representation & lowering invariants

## 2026-10-31 — Updated `c-torture-gate.md`
- Sprint 68: PASS 2,789->2,864, REJECT 223->151; array-decay binop typing, cast const-eval, global-base sentinel

## 2026-10-31 — Updated `frontend-and-types.md`
- Sprint 68: array operands of +/- decay to pointer-to-element in visitor binop typing

## 2026-10-31 — Updated `c-torture-gate.md`
- Sprint 68: inlined-function member access fix (20071120-1 O2/O3) - last MISCOMPILE resolved; PASS 2,865, REJECT 151, MISCOMPILE 0

## 2026-10-31 — Updated `c-torture-gate.md`
- Sprint 69: glibc va_list shadow + math.h/stdio.h + ELF prototype hygiene; baseline PASS 2873 / REJECT 144 / MISCOMPILE 0 / REF-BUG 3

## 2026-10-31 — Updated `abi-sysv-x86_64.md`
- Sprint 69: external va_list shadow section

## 2026-10-31 — Updated `ir-and-regalloc.md`
- Sprint 69: -O1+ branchy clamp/restore fragility note

## 2026-10-31 — Updated `frontend-and-types.md`
- Sprint 69: math.h virtual header + stdio.h/va_end notes

## 2026-11-01 — Updated `frontend-and-types.md`
- Sprint 70: type-identity invariants (const members, K&R stars, comma declarators, funcptr typedef spiral rule, incomplete struct materialization, param tag preservation, anonymous aggregate comma declarators, bswap64, label-table growth)

## 2026-11-01 — Updated `c-torture-gate.md`
- Sprint 70 baseline: PASS 2920, REJECT 103, MISCOMPILE 0

## 2026-11-01 — Updated `c-torture-gate.md`
- Sprint 71: baseline PASS 2,946 / REJECT 80 / MISCOMPILE 0 (last MISCOMPILE va-arg-22 fixed: struct-copy tail family + 9-16B VREG_2 tails + va_arg >16B one-deref rule)

## 2026-11-01 — Updated `abi-sysv-x86_64.md`
- Sprint 71: struct-copy tail completeness (4+2+1 / byte-by-byte), 9-16B VREG_2 tail = exactly size-8 bytes via zc_store_low_bytes, va_arg >16B hidden pointer = one deref

## 2026-11-01 — Updated `frontend-and-types.md`
- Sprint 71: static-local aggregate/funcptr brace initializers (.data direct, has_init pre-detection, data_patch_off=0), funcptr struct members (is_func_ptr propagation), indirect-call return typing from callee TypeInfo

## 2026-11-01 — Updated `ir-and-regalloc.md`
- Sprint 71: zc_store_low_bytes — spill+byte-copy, never shift temps (LRA shift-temp collision), zc_emit_mem_copy 4+2+1 tail

## 2026-11-01 — Updated `global-data-relocs.md`
- Sprint 71: ZeroCCDataReloc kinds 0-4 documented (kind 3 = function symbol for funcptr data initializers)

## 2026-11-01 — Updated `c-torture-gate.md`
- Sprint 72: extern/funcptr/struct-member declarator burn-down + static-local address-of (PASS 2946->2970, REJECT 80->61, MISCOMPILE 0); bool/_Bool typedef work shelved pending pre-existing O2 packed-struct fix

## 2026-11-01 — Updated `frontend-and-types.md`
- Sprint 72: funcptr member pointer-wrap rule (all stars), funcptr/comma struct members, extern specifier runs + funcptr objects, static-local address-of data-section routing

## 2026-11-01 — Updated `ir-and-regalloc.md`
- Sprint 72: static-local address-of routes to data-section __sl_N (pre-scan static_addr_taken), not the dead per-call stack mirror

## 2026-11-02 — Updated `c-torture-gate.md`
- Sprint 73: baseline PASS 2996 / REJECT 38, MISCOMPILE 0 (nested-subscript lexer, const declarators, pointer/null comparison, member pointer-arrays & multi-level pointers, dot-on-array, sizeof extern-object subscript, convertvector).

## 2026-11-02 — Updated `frontend-and-types.md`
- Sprint 73: const-qualified scalar typedefs & const funcptr params, pointer vs null-pointer-constant comparison, [[/]] not two-char tokens, sizeof of extern-object subscript, struct/union member pointer-arrays & multi-level pointers, dot-on-array member access.

## 2026-11-02 — Updated `global-data-relocs.md`
- Sprint 74: Document file-scope compound literals, upfront storage reservation invariant, and member offsetof

## 2026-11-02 — Updated `c-torture-gate.md`
- Sprint 74: Ratchet baseline to PASS 3003, REJECT 31, MISCOMPILE 0

## 2026-11-02 — Updated `c-torture-gate.md`
- Sprint 75: REJECT burned down to 15 (all architectural limits), MISCOMPILE 0, PASS 2,993. Address-of part extractors, large struct TypeInfo preservation, cast/string pointer inits, and GNU designators.

## 2026-11-02 — Updated `architecture-pipeline.md`
- Sprint 76: Modularize ast_lowering_expr.c into ast_lowering_{call,assign,binop,expr}.c

## 2026-11-02 — Updated `architecture-pipeline.md`
- Split GCC/Clang builtins, intrinsics, and atomics out of ast_lowering_call.c into ast_lowering_builtins.c

## 2026-11-02 — Updated `c-torture-gate.md`
- Sprint 77: Fix attribute infinite loop and compound literal brace init regressions; PASS 2,993 -> 3,039, REJECT 15 -> 13, TIMEOUT 73 -> 18, MISCOMPILE 0

## 2026-11-02 — Updated `architecture-pipeline.md`
- Sprint 78: Modularize preprocessor.h, definitions.h, and compile_unit.h into virtual_headers.h, cpp_expr.h, decl_struct.h, decl_enum.h, and const_ptr_eval.h

## 2026-11-02 — Updated `architecture-pipeline.md`
- Modularize const_eval.h into type_parser.h, ast_stmt.h into decl_local.h, and main.c into driver_opts.h and driver_asm.h

## 2026-11-02 — Updated `architecture-pipeline.md`
- Sprint 80: g_compiler & global_vars to compiler context

## 2026-11-02 — Updated `global-data-relocs.md`
- Sprint 80: migrate global_vars to ZeroCCCompiler instance

## 2026-11-02 — Updated `ir-and-regalloc.md`
- Eliminate g_compiler from flat IR emitter, making ZeroCC 100% thread-reentrant

## 2026-11-02 — Updated `architecture-pipeline.md`
- Refactor monolithic header implementations (preprocessor.h, compile_unit.h, decl_struct.h) into standalone .c compilation units

## 2026-11-02 — Updated `sanitizer-roadmap.md`
- Added sanitizer-roadmap.md: analysis & phased roadmap for the optional, parked zero_san target-side safety feature (UBSan-lite signed-overflow v1; bounds/leaks/ASan-LSan scoped out)

## 2026-11-02 — Updated `frontend-and-types.md`
- Consolidate ZeroCCType and TypeInfo classification predicates and bridge conversions

## 2026-11-02 — Updated `frontend-and-types.md`
- Document GNU Extension Labels as Values (&&label) and Computed Goto (goto *ptr;)

## 2026-11-02 — Updated `ir-and-regalloc.md`
- Document OP_LEA_LABEL and OP_JMP_INDIRECT IR instructions and optimizer invariants

## 2026-11-02 — Updated `isa-backends.md`
- Document emit_jmp_reg and emit_lea_label across x86-64, AArch64, and RISC-V backends

## 2026-11-02 — Updated `frontend-and-types.md`
- Document GNU C integer complex support and ABI rules

## 2026-11-02 — Updated `ir-and-regalloc.md`
- ir-and-regalloc: measured 16-bit temp-id calibration (3 temps/stmt, ~22k stmts/function) + code-smell analysis (overloaded ID namespace + non-recycling) and ranked fixes

## 2026-11-02 — Updated `frontend-and-types.md`
- Document __builtin_return_address, __builtin_frame_address, and empty structs (Sprint 86)

## 2026-11-02 — Updated `ir-and-regalloc.md`
- Document linear scan live interval extension across OP_JMP_INDIRECT loop backedges (Sprint 86)

## 2026-11-02 — Updated `c-torture-gate.md`
- Remove return_address from UNSUPPORTED_TARGETS and document Sprint 86 conformance expansion

## 2026-11-02 — Updated `c-torture-gate.md`
- Sprint 87: Unskip label_values, inline asm triage, static label difference relocs, and ratchet baseline (PASS 3,162, MISCOMPILE 0, TIMEOUT 18)

## 2026-11-02 — Updated `global-data-relocs.md`
- Document kind 5 (code label) and kind 6 (static label difference) data relocations

## 2026-11-03 — Updated `ir-and-regalloc.md`
- Replace x86-64-only jmp_runtime.c with multi-ISA inline OP_SETJMP/OP_LONGJMP IR & backend emission, returns_twice LRA/optimizer semantics, and C99 6.7.6.3p7 array-typedef parameter decay

## 2026-11-03 — Updated `c-torture-gate.md`
- Sprint 89: 0 REJECT / 0 MISCOMPILE in c-torture (3,234 PASS, 1 TIMEOUT: memclr JIT at the 5 s budget edge); ratchet invariants corrected to match the harness (bad count + per-test PASS→MISCOMPILE/REJECT)

## 2026-11-03 — Updated `isa-backends.md`
- Sprint 89: 221/221 4-ISA vtable parity and cross-ISA OP_SYSCALL verified under QEMU (52/52)

## 2026-11-03 — Updated `architecture-pipeline.md`
- Fixed O(n²) scope registration in `zero_cc_compile_function` (`src/zero_cc/jit/compile.h`): the per-function registration loop re-scanned ALL previously compiled functions and ALL global variables, performing `zero_cc_type_to_typeinfo` + `type_function` conversion before duplicate checking. Added `scope_lookup_current()` early-exit before type conversion in both the function and global-variable registration loops. c-torture `execute/memclr.c` (2048 static functions) JIT -O1+: 6.7s → 2.0s; -O2/-O3: 7.7s → 2.3s (all under 5s budget). Verified by `make test-torture` (0 regressions) and `make test-opt` (120/120 OK).
- **Phase 1: GNU Nested Functions / Trampolines (JIT-only, x86-64, no closure capture)**: GNU C nested function definitions inside a block (C99 6.9p15 extension) are now accepted instead of rejected. The parser registers the nested function signature, saves the body token range, and skips past the body; after the enclosing function is lowered, each nested function's body is re-parsed, lowered, and emitted as a separate function in the same text section. Non-capturing nested functions (e.g. c-torture `nestfunc-1.c`) compile and run correctly at all optimization levels in both JIT and AOT. Tests requiring capture semantics remain `SKIP-TARGET:trampolines` until Phase 2 implements trampoline/context-pointer support. Files changed: `src/zero_cc/jit/compile.h`, `src/zero_cc/parser/decl_local.h`, `src/zero_cc/types/symbols.h`, `src/zero_cc/types/type_system.h`. Verified by `make test-torture` (8 expected regressions in capture-dependent tests; 0 regressions in non-capture tests) and `make test-opt` (120/120 OK).

## 2026-11-03 — Updated `architecture-pipeline.md`
- Sprint 90: nested-functions Phase 2 — closure capture (by-value hidden leading params) for direct calls; five nested-path codegen fixes (parse-after-table-reset, frame-reset-before-params, sem pass on re-parsed body, explicit lowering fn identity, is_variadic); 21 tests flipped, pr71494/pr22061-3/4 -> SKIP-TARGET:nested_capture_writeback (Phase 3 by-reference writeback)

## 2026-11-03 — Updated `c-torture-gate.md`
- Sprint 90 baseline: PASS 3,253, 0 REJECT / 0 MISCOMPILE / 0 TIMEOUT; new SKIP-TARGET:nested_capture_writeback (3); memclr.c TIMEOUT passed on idle re-record

## 2026-11-03 — Updated `architecture-pipeline.md`
- Phase 3a by-reference nested capture: mirror in/out, mangled nested symbols, enclosing-chain callee resolution, multi-level nesting + pointer-param rename fixes

## 2026-11-03 — Updated `c-torture-gate.md`
- Baseline 3,255 PASS after Phase 3a (pr71494 + 20061220-1 unskipped, 0 bad)

## 2026-11-03 — Updated `architecture-pipeline.md`
- Sprint 92: Phase 3a-2 runtime VLA param dimensions + runtime VLA sizeof (pr22061-3/4 pass, 3257 PASS)

## 2026-11-03 — Updated `c-torture-gate.md`
- Sprint 92: baseline 3257 PASS, nested_capture_writeback category eliminated

## 2026-11-03 — Updated `architecture-pipeline.md`
- Sprint 93 (Phase 3b): real trampoline stubs + pointer-routed by-reference capture; unskipped 8 trampolines tests (PASS 3,266)

## 2026-11-03 — Updated `c-torture-gate.md`
- Sprint 93: trampolines no longer blanket-skipped (11 targeted), PASS 3,266

## 2026-11-03 — Updated `frontend-and-types.md`
- Sprint 94: C11 _Alignas/_Alignof correctness (file-scope any N, local cap 16 + warning, sizeof unchanged) and __STDC_NO_COMPLEX__ removal

## 2026-11-03 — Updated `frontend-and-types.md`
- Sprint 95: _Alignas(N>16) locals via GCC-style runtime SP realign prologue (frame_align mode, and $-N,%rsp, leave epilogue); cap-with-warning only for ineligible fns / other ISAs

## 2026-11-04 — Updated `isa-backends.md`
- Sprint 96: cross-ISA _Alignas(N>16) realign (AArch64 + RISC-V 64/32), encoding pitfalls (64-bit AND masks, SP-as-Rm, RV64 funct7, RV32 large displacements, binutils sym+offset bug)

## 2026-11-04 — Updated `isa-backends.md`
- Sprint 97: trampoline stubs ported to AArch64 + RISC-V64; per-ISA encoding invariants (LDUR/STUR families, PC-relative bl/jal, pre-push pad, separate entry namespaces, RISC-V nested-call relocation) and the >8-arg MC call fix

## 2026-11-04 — Updated `frontend-and-types.md`
- Sprint 98: K&R (old-style) definitions - top-level no-decl-list int default + type-name guard, GCC-matching diagnostics (redefinition / no-such-parameter / storage class), nested K&R with token-rewind chunk rebuild, zero_cc_parse_kr_decl moved to zero_cc_declarator_stack.h, 76 SKIP-C89 -> PASS

## 2026-11-04 — Updated `c-torture-gate.md`
- Sprint 99: nonlocal_goto unskipped; baseline ratcheted to PASS 3,575 (0 bad); SKIP-C89 100; TRAMPOLINES_BLOCKED down to 1 (nested-1 VLA struct member)

## 2026-11-04 — Updated `architecture-pipeline.md`
- Sprint 99: named nonlocal goto across nested scopes (OP_NONLOCAL_GOTO, hidden fp/sp pair params); computed goto to ancestors rejected like GCC; trampolines cluster down to nested-1

## 2026-11-04 — Updated `frontend-and-types.md`
- Sprint 99: fn-ptr prototype params (fn_params), gnu89 implicit decls, extra-paren fnptr params, pp-number atomicity in preprocessor

## 2026-11-04 — Updated `abi-sysv-x86_64.md`
- Sprint 99: effective param signature for indirect calls, no-prototype complex arg rule, mirror-form subscript, VLA param row stride

## 2026-11-04 — Updated `c-torture-gate.md`
- Sprint 100: VLA struct members unblocked nested-1 (last trampolines holdout) + 9 more; PASS 3586, baseline ratcheted

## 2026-11-04 — Updated `frontend-and-types.md`
- Sprint 100: GNU variably-modified struct/union members (type system, definition-time capture, tag-collision diagnostics)

## 2026-11-04 — Updated `abi-sysv-x86_64.md`
- Sprint 100: VLA struct by-value ABI (2 GPR units: pointer + runtime size)

## 2026-11-04 — Updated `architecture-pipeline.md`
- Sprint 100: all 11 trampolines-cluster c-torture tests now run (nested-1 PASS)

## 2026-11-04 — Updated `build-and-verification.md`
- Docs refresh: full test suite 165/165 (13 unit / 17 semantic / 38 abi / 97 jit), optimizer sweep 126 tests, own-linker 21/21, ASan 165/165, QEMU battery 59/59 (15/15 x86-64+aarch64+riscv64, 14/14 riscv32)

## 2026-11-04 — Updated `c-torture-gate.md`
- Docs refresh: mark Sprint 57 chunked burn-down plan complete; corpus state PASS 3,586 / 0 bad; resolve stale bool/packed-struct note (Sprint 74, both PASS on all 8 configs)

## 2026-11-04 — Updated `isa-backends.md`
- Docs refresh: QEMU battery 59/59 (was 44/44), riscv64 raw 15/15, 13_syscall battery note, Sprint 100 VLA-struct 2-GPR-unit trampoline model

## 2026-11-04 — Updated `ir-and-regalloc.md`
- Docs refresh: 141 opcodes (OP_NONLOCAL_GOTO), OP_SYSCALL now all 4 ISAs (Sprint 89)

## 2026-11-04 — Updated `architecture-pipeline.md`
- Docs refresh: 141-opcode lowering, add Sprint 97 cross-ISA trampoline stubs to nested-function notes

## 2026-11-04 — Updated `frontend-and-types.md`
- Docs refresh: nested-function section current-state (JIT + raw AOT on x86-64/AArch64/RISC-V64 since Sprint 97)

## 2026-11-04 — Updated `index.md`
- Docs refresh: 141-opcode IR description, c-torture baseline summary (Sprint 100: PASS 3,586, 0 bad)

## 2026-11-04 — Updated `zero-rt.md`
- Docs refresh: Sprint 55 cross archives (zero_rt_aarch64.a / zero_rt_riscv64.a), per-target auto-linking, title updated

## 2026-11-04 — Updated `own-linker.md`
- Docs refresh: zero_rt per-target auto-linking (Sprint 55) instead of x86-64-only

## 2026-11-04 — Updated `c-torture-gate.md`
- Sprint 101: C89 implicit-int cluster unlocked (PASS 3,615, SKIP-C89 61)

## 2026-11-04 — Updated `frontend-and-types.md`
- Sprint 101: C89 implicit-int & gnu89 tail leniencies documented

## 2026-11-04 — Updated `index.md`
- Sprint 101 gate state (PASS 3,615, SKIP-C89 61)

## 2026-11-04 — Updated `c-torture-gate.md`
- Sprint 102: asm labels, SSO big-endian, mixed-base bit-field packing (PASS 3,626, SKIP-C89 51)

## 2026-11-04 — Updated `frontend-and-types.md`
- Sprint 102: GNU asm labels, scalar_storage_order, __FUNCTION__ documented

## 2026-11-04 — Updated `index.md`
- Sprint 102 gate state (PASS 3,626, SKIP-C89 51)

## 2026-11-04 — Updated `c-torture-gate.md`
- Sprint 103: __builtin_apply/__builtin_apply_args; untyped_assembly fully unskipped (PASS 3,665, SKIP-C89 52)

## 2026-11-04 — Updated `frontend-and-types.md`
- Sprint 103: __builtin_apply/__builtin_apply_args documented

## 2026-11-04 — Updated `index.md`
- Sprint 103 gate state (PASS 3,665, SKIP-C89 52)

## 2026-11-04 — Updated `c-torture-gate.md`
- Sprint 104: standalone _Complex = _Complex double (PASS 3,667, SKIP-C89 50)

## 2026-11-04 — Updated `frontend-and-types.md`
- Sprint 104: standalone _Complex documented

## 2026-11-04 — Updated `index.md`
- Sprint 104 gate state (PASS 3,667, SKIP-C89 50)

## 2026-11-04 — Updated `c-torture-gate.md`
- Sprint 105: floating-literal suffix order (1.0fi ≡ 1.0iF), 3 SKIP-C89 -> PASS (PASS 3,670, SKIP-C89 47)

## 2026-11-04 — Updated `frontend-and-types.md`
- Sprint 105: complex literal suffix order documented

## 2026-11-04 — Updated `index.md`
- Sprint 105 gate state (PASS 3,670, SKIP-C89 47)

## 2026-11-04 — Updated `c-torture-gate.md`
- Docs refresh: corrected SKIP-TARGET bucket total 12 -> 13 (int128 7 + named_sections 3 + unwrapped 2 + arm 1) to match the ratcheted baseline; aligned README + development plan with Sprint 105 gate state (PASS 3,670, SKIP-C89 47)

## 2026-11-04 — Updated `c-torture-gate.md`
- Sprint 106: freopen declared in stdio.h virtual header; SKIP-TARGET:unwrapped unblocked (PASS 3,672, SKIP-TARGET 11)

## 2026-11-04 — Updated `frontend-and-types.md`
- Sprint 106: freopen/stdio virtual header documented

## 2026-11-04 — Updated `index.md`
- Sprint 106 gate state (PASS 3,672)

## 2026-11-04 — Updated `c-torture-gate.md`
- Sprint 107: __attribute__((section)) on objects - SKIP-TARGET:named_sections object tests unblocked (PASS 3,672 -> 3,674, named_sections 3 -> 1; function-section test compile/section.c remains)

## 2026-11-04 — Updated `frontend-and-types.md`
- Sprint 107: GNU __attribute__((section)) named object sections (preprocessor marker, file-scope strip, add_global_var, AOT -c emission)

## 2026-11-04 — Updated `isa-backends.md`
- Sprint 107: named object sections in AOT -c ELF emission
