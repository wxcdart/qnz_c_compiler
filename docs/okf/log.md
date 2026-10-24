---
type: Log
title: ZeroCC OKF Knowledge Bundle Log
description: Sequential changelog tracking how and when the ZeroCC OKF knowledge bundle evolves alongside compiler sprints.
resource: okf/log
tags: [zerocc, changelog, log, okf]
timestamp: 2026-10-24T23:19:32Z
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
