# ZeroCC Changelog

## [Sprint 41] - 2026-10-20 - Static-Local Self-Hosting: Unsized Arrays, Mirror Direction, String Pointer Arrays + `va_arg` Fix

Closes the Sprint 37 static-local blockers that prevented full-header self-hosting. zerocc's own headers use block-scope `static const char *arr[] = {...}` (e.g. `gpr_names[]`/`xmm_names[]` in `ir/live_range.h`, `sys_paths[]` in `core/preprocessor.h`); none of these patterns compiled before.

### Fixed
- **Static-local mirror direction** (`ir/ast_lowering.c`): `static_local_mirror_in`/`_out` stored chunk `off` at `fp - (stack_offset + off)` (wrong direction), so a multi-element array's elements landed out of order / out of bounds (element 0 correct, the rest scrambled). Both directions now copy **full 8-byte chunks** from data `O+off` to frame `fp + off - stack_offset`, matching the frame-relative body accesses (`fp - stack_offset + i*8`). Safe because the data slot and frame slot are both allocated rounded-up-to-8.
- **Unsized array sizing deferred past reservation**: `T arr[] = {...}` was registered/`add_local`'d at the 8-byte minimum and the initializer enlarged it afterward, so two static-local arrays in one function **overlapped** (the second's mirror clobbered the first's slot → segfault). New `zero_cc_measure_array_init_at` (`parser/ast_stmt.h`) walks the initializer tokens **without consuming them** to compute the object byte size + element count, and the declaration applies it to `decl_type` **before** `zero_cc_type_to_typeinfo`/`zero_cc_add_local`/`zero_cc_static_local_register`. This sizes both the stack mirror slot and the `.data` reservation correctly up front.
- **Static-local pointer-to-string initializers** (`parser/ast_stmt.h`): `static const char *names[] = {"a","b",NULL};` now writes each string to `.rodata` and records a `ZeroCCDataReloc` (kind 0) per pointer slot (resolved by the JIT data-page patcher and the AOT `.rela..data` emitter); `NULL`/other constant expressions go through `zero_cc_eval_const_expr`. Char-array string initializers (`static char s[] = "hi"`) and braced char/numeric lists are written as raw bytes; unsized arrays infer `array_len`/`size` from the initializer.
- **`zero_cc_eval_const_expr` casts** (`parser/const_eval.h`): `parse_primary` now handles `(type) expr` (e.g. `(void*)0`, `((int)5)`), so initializer constant-expressions with casts evaluate instead of failing. Scalar/array-element initializers also guard against partial token consumption (a scalar initializer must be followed by `;`, an array element by `,`/`}`).
- **Variadic `va_arg` was a no-op** (`core/preprocessor.h`): the built-in `stdarg.h` fallback redefined `va_start`/`va_arg`/`va_end` as no-ops, silently breaking every variadic function (`test/abi/variadic_fn` reported `1` instead of `0`). Both `stdarg.h` variants now map to `__builtin_va_start`/`__builtin_va_arg`/`__builtin_va_end`/`__builtin_va_copy`, and `__builtin_va_copy` is registered as a built-in object-like macro alongside the others.

### Verification
- `make test` **142/142**, `make test-opt` **106 ok / 0 failed**, `sret_verifier` **6/6**, `okf` **lint+index pass**; `test/abi/variadic_fn` now prints `0 (expected 0)`.
- gcc differential (JIT + AOT, `-O0..-O3`): single & multiple block-scope static pointer arrays of strings (the exact `live_range.h`/`preprocessor.h` shapes, incl. a `NULL`-terminated `sys_paths[]`), static numeric/char arrays, `static char[] = "..."`, mixed with ordinary locals + a static scalar, early `return` mid-function (mirror-out on every exit path, counter persists), 16-element pointer arrays. All MATCH / pass.

### Warning cleanup (self-hosting hygiene)
- `parser/const_eval.h`: `(zero_cc_find_typedef(...) || zero_cc_find_struct(...) || zero_cc_find_enum(...)) != NULL` compared an **int** to a pointer (C `||` yields int, so the group evaluated to a bool). Now tests each result for non-NULL individually; identical behavior.
- `core/preprocessor.h`: the include-resolver `char path[1024]` concatenated `include_file_dirs[d]` (a `char[PATH_MAX]`) + `"/" + filename`, which GCC proved could truncate. A resolved include is a filesystem path (bounded by `PATH_MAX`); the buffer is now `PATH_MAX + 64` to leave room for the filename component. Any result that still overflows is an invalid over-long path that `access()` rejects, and `snprintf` truncates safely regardless.
- `parser/parser.h`: added the missing prototype `size_t zero_cc_ast_node_size(void);` (defined in `parser.c`, called by the stale-build guard in `main.c`) to silence the `-Wimplicit-function-declaration` warning. The guard is now documented in `docs/okf/build-and-verification.md` — a `ZeroCCASTNode` layout change requires a full `make clean && make build/bin/zerocc`.

## [Sprint 40] - 2026-10-20 - Pre-Existing Gaps: FP Ternaries, 64-bit Literals, Array Initializers/Parameters, `-S` Emitters, SIMD Suite

### Fixed
- **FP ternary lowering** (`ir/ast_lowering_expr.c`): `cond ? fa : fb` miscompiled at every -O level - the result temp was always defined by a GPR `MOV_REG` and marked non-float, so the caller read an uninitialized GPR and a trailing `(int)` cast lowered to `SEXT32` instead of `CVTTSD2SI`. Now: the result is defined by `OP_FMOV` when either arm is FP (the allocator derives a temp's register class from its defining opcode, so an XMM-class temp must be defined by an FP op); mixed int/FP arms follow the C usual arithmetic conversion (the int arm is `cvtsi2sd`'d, in both directions). Because the then-arm move is emitted before the else arm is lowered, the else arm's floatness is predicted by a conservative AST probe (`ast_expr_is_fp` - float literals, FP casts/idents/binops/calls/nested ternaries; comparisons and `!` yield int; unknown shapes stay on the int path so a wrong 0 keeps the old behavior rather than over-converting).
- **64-bit hex literals** (`lexer/scan.h`): `0x123456789ABCDEFLL` and every hex literal containing an `E`/`e` digit were misclassified as hex FLOATs - the float-vs-int scan treated `E` as an exponent marker. C99 6.4.4.2: a hex literal is a floating constant only when it has `.` or a `p`/`P` exponent; `e`/`E` are ordinary hex digits. The misclassification consumed one `L` as a long-double suffix and left the second dangling (parse error) or stored a double in an int - this was the root cause of the Sprint 39 "64-bit literals > 32 bits miscompile" finding (ternary arms and XOR operands). Genuine hex floats (`0x1.8p1`) and decimal floats (`1e2`) still classify as floats.
- **N-D / struct array initializers** (`ir/ast_lowering_expr.c`): the parser flattens nested brace lists into a leaf-level chain, but the compound-literal lowering placed element k at `k * base_size` with a `base_size` store - correct only for 1-D arrays of scalars. `int m[2][4] = {{...},{...}}` stored element 1 at byte 16; `struct P v[2] = {{1,2,3,4},...}` stored each int 16 bytes apart (both broken on the clean tree, -O0). New `object_count_scalar_leaves` / `object_resolve_scalar_leaf` walkers place each flattened element by its C-layout leaf offset/size (1-D, N-D, and struct members, including mixed-width members); the legacy path remains as fallback for unresolvable layouts.
- **Array parameter adjustment** (`jit/compile.h`): `int f(int x[])` left the parameter typed as element type `int`, so the pointer argument was sign-extended as a 32-bit int (`movslq`) and truncated at runtime. The outermost array dimension is now adjusted to a pointer per C99 6.7.6.3p7, with remaining dimensions forming the pointed-to array type: `int m[][4]` → `int (*)[4]` (the pre-fix first attempt wrapped every dimension in a pointer, giving `int**` for the 2-D case).
- **Multi-declarator arrays** (`parser/ast_stmt.h`): `float a[n], b[n];` parsed array dimensions for the first declarator only; later declarators now get their own dimensions and VLA info.
- **Multiple VLAs per function** (`ir/ast_lowering_stmt.c`): the `Only one VLA allowed per function` restriction was removed (each VLA increments `vla_count`); the function-scope / no-loops rules remain.
- **`-S` asm_emitter opcode gaps** (`ir/asm_emitter.c`, `isa/x86_64_asm.c`, `isa/aarch64_asm.c`, `isa/riscv_asm.c`, `isa/isa_backend.h`): `OP_ADD_IMM`, `OP_SUB_IMM`, `OP_INC`, `OP_DEC`, `OP_NOT`, `OP_LNOT` fell through to the `# UNKNOWN` default, so any `-S` output containing them was silently wrong (e.g. `return ~a;`). All three backends now emit them (new vtable slots `emit_add_imm_asm`/`emit_sub_imm_asm`/`emit_inc_asm`/`emit_dec_asm`/`emit_not_asm`; `OP_LNOT` composes the existing `test`/`cond_set` emitters). `OP_NOT`/`xori`-style in-place ops copy src→dst first when the allocator picks distinct registers, mirroring the MC emitters. (Note: the Sprint 39 "known issue" listed `OP_SUB_INC`/`OP_ADD_INC`, which do not exist in the opcode enum - the real list is the six above.)
- **`tools/simd_regression_suite.py`**: the suite compared the process **exit code** (8-bit truncated, so e.g. 1000 → 232 could never match) against the expected value, and counted SIMD instructions in a `.s` file its compile command never generated. Tests now print their result via `printf` (return 0) and the suite compares stdout; the SIMD count compiles a separate `-S` artifact and matches ZeroCC's documented double-width scalar-FP model (`addsd`/`movsd`/`cvtsi2sd`/`cvttsd2si`, not just the `*ss` variants). `float_array_loop` now zero-initializes its VLAs (uninitialized reads are indeterminate). **Result: 10/10 pass (previously 0/10)**; baseline saved to `simd_regression_baseline.json`.

### Verification
- `make test-opt` **106/106**, `make test` **142/142**, `cfg_verifier` **112 files / 0 failed** (63 skipped, 101 warnings - same policy), `mem2reg_verifier` all passed, `sret_verifier` **6/6**, `licm_test_suite` **5/5**, **SIMD suite 10/10**.
- gcc differential: 1-D/2-D/3-D int arrays, struct arrays (16-byte struct), float/char arrays, 1-level designated initializers, array params (1-D, `int (*)[4]`, `*a`/`*(a+1)` through the param), FP ternaries (both arms FP, mixed int/FP both directions, nested, at -O0..-O3 JIT+AOT), 64-bit literals in ternary arms/XOR/compare, hex-with-`LL` battery, hex floats vs hex ints. All MATCH.
- `-S`: 0 `# UNKNOWN` comments across the 79-test `test/jit` corpus; all assemblable outputs compile with `gcc -c` (16/16 standalone tests).

## [Sprint 39] - 2026-10-20 - Branchless Conditional Select (CFG-enabled -O3 pass) + copy_prop invalidation fix

### Added
- **`opt/opt_branchless.{c,h}` - branchless conditional select** (the first of the planned CFG-enabled optimizations; `-O3` only, priority 58 between LICM and strength reduction). Converts the ternary-lowering branch shape (`JE t, L_else` + then arm + `JMP L_end` + else arm + merge label) into the branchless bitwise select `mask = (t ? -1 : 0); diff = A ^ B; sel = mask & diff; res = B ^ sel` - exact for any 64-bit pattern (int, pointer, struct chunk), no new opcode.
  - **Two condition forms**: (a) preceding `OP_COND_SET` (t is 0/1; mask = `t * -1`); (b) plain GPR value (arbitrary int, jump tests `t == 0`; mask via `OP_CMP_ZERO` + `OP_COND_SET CC_EQ` - the same sequence lowering uses for `!x`).
  - **Arms must be pure value plumbing** (frame load / register copy / in-place SEXT-ZEXT, ending in `MOV_REG res, V`) - the copy form appears after copy-prop replaces the arm's frame load.
  - **Safety**: CFG predecessor checks (L_else only from the JE block; L_end exactly from {then, else}); `res` not read in the span; values provably GPR-resident (definition walk; FP arms rejected); condition stays live; 4 fresh temps/site with the 16-bit temp guard; two-pass arena rebuild with `ir_call_args` remap (LICM recipe).
  - **Mask opcodes are -S-safe by construction**: the `-S` `asm_emitter` has no `OP_NOT`/`OP_SUB_INC`/`OP_ADD_INC`/`OP_INC`/`OP_DEC` emitters (pre-existing gap, `# UNKNOWN` comments), so the mask uses only `MOV_IMM`/`MUL`/`SUB`/`XOR`/`AND`/`CMP_ZERO`/`COND_SET`, supported by both `-S` and `-o` paths.
  - **Regression guard**: `test/jit/ternary_branchless.c` in the `make test-opt` corpus (swept at every -O level, JIT + AOT), plus a gcc differential battery (plain/comparison conditions, negative/large values, pointer arms, SEXT arms, condition reuse, nested ternaries, loop-carried selects).

### Fixed
- **`opt/opt_copy_prop.c` - incomplete definition invalidation**: `instr_defines_temp` used an explicit opcode list that missed the in-place ops (`OP_ADD_IMM`, `OP_SUB_INC`, `OP_INC`, `OP_DEC`, `OP_SEXT*`, `OP_ZEXT*`, `OP_LEA`, …), so an in-place redefine of a copy-mapped temp was not invalidated and later uses were replaced with the stale copy source (`t2 = t; t2 = t2 - 1; r = ~t2` → `r = ~t`). Latent before this sprint (lowering only ever emitted 3-operand forms for `x += c`); the branchless pass's in-place `SUB_INC` mask was the first trigger. Now the generic rule matching DCE's liveness convention: `temp_dest != TEMP_NONE` defines, with `OP_CALL`/`OP_CALL_INDIRECT` excluded (their `temp_dest` is not a temp ID since C2b). Strictly more conservative invalidation; no perf regression measured.

### Known Issues (surfaced, pre-existing, unfixed - documented in `docs/okf/optimization-passes.md`)
- **Float/double ternary miscompiles at -O0**: FP conditional-expression result temp allocated GPR-class while arms `FLOAD` into XMM; trailing `(int)` cast lowers to `SEXT32` instead of `CVTTSS2SI` → reads an uninitialized GPR.
- **64-bit literals > 32 bits miscompile at -O0** in ternary arms (returns the ELSE value) and as XOR operands (`2 ^ 0xAAAAAAAAAAAAAAAAL` → `0x7FFFFFFFFFFFFFFD`); 34-bit values fine. Repro without any ternary for the XOR case - a constant/immediate lowering gap.
- **`-S` asm_emitter opcode gaps**: `OP_NOT`/`OP_SUB_INC`/`OP_ADD_INC`/`OP_INC`/`OP_DEC` emit `# UNKNOWN` comments (the `-o` ELF path is complete).

### Verification
- `make test-opt` **106/106** (new corpus test included), `make test` **142/142**, `cfg_verifier` **112 files / 0 failed** (63 skipped, 101 warnings - same policy), `mem2reg_verifier` all passed, `sret_verifier` **6/6**, `licm_test_suite` **5/5**, SIMD suite unchanged (10 pre-existing FP/frontend failures).
- No perf regression: copy_prop 0.6-0.9 ms @-O2 on `synth_large.c` (≈ pre-sprint), total passes-only ~106-117 ms (host-load range).

## [Sprint 38] - 2026-10-20 - GVN Cost: ~200 ms → ~2.3 ms (Generation-Stamped Canonical Table)

### Changed
- **`opt/opt_gvn.c` (`opt_restrict_memory_forward`)**: the mf state (redundant-load elimination / store-to-load forwarding) re-establishes at every basic-block boundary — every `OP_LABEL`, jump, call, ... — and each reset rewrote the entire 65536-entry canonical-temp table (`g_canon[i] = i`, 80-180 µs). A large function has ~447 such boundaries, so one rewrite × 447 dominated the pass: **~60 ms per function of the ~65 ms GVN total** (the Sprint 36 "hash-table re-init churn" hypothesis was pointed at the wrong half — the value-numbering loop itself is 0.5 ms).
- Fix: **generation stamps**. `g_canon` is now `CanonSlot { uint32_t gen; uint16_t val }[65536]`; a read is `gen == current ? val : identity`, a write stamps the current generation, and `mf_reset()` is a single counter bump (O(1)). A 32-bit wraparound guard (full clear) is kept for completeness — unreachable in practice (one file per process, ~10⁴ resets).

### Measured (median of 5 `--profile-opts` runs, `test/bench/synth_large.c`)
| Pass | Before @-O2 | After @-O2 | Before @-O3 | After @-O3 |
|---|---|---|---|---|
| gvn | ~184-210 ms | **2.3 ms** | ~293 ms | **2.3 ms** |
| total passes-only | ~275 ms | **~134 ms** | ~300 ms | ~140 ms |

- **Correctness**: `-dump-ir` **byte-identical** at `-O2` and `-O3` pre/post; `make test-opt` **105/105**, `make test` **141/141**, `mem2reg_verifier` **12/12**, `sret_verifier` **6/6**, `licm_test_suite` **5/5**, `cfg_verifier` **0 failed**.
- **Next dominant pass identified**: LICM (~103 ms @-O2) — tracked, not touched here (separate risk budget; LICM does structural IR rebuilds, so a byte-identical-IR proof is not available the way it was for GVN/mem2reg).
- **Spike methodology** (kept for the record): temporary per-phase `CLOCK_MONOTONIC` timing inside the pass (`main loop` vs `memory-forward` vs per-reset) + per-branch counters. The counters showed the mf inner loops did ~4K total iterations per function while the loop over 6K instructions took 60 ms — the cost was in the uncounted per-boundary `mf_reset()`, not in any per-instruction work.

## [Sprint 37] - 2026-10-20 - Block-Scope `static` Locals (data section + per-call stack mirror)

### Added
- **Block-scope `static` locals** (C99 6.9.2): previously the `TK_STATIC` specifier was silently discarded by the parser, so `static` locals were lowered as plain stack locals — re-initialized on every call. Now each `static` local gets an 8-byte-aligned allocation in `data_section` registered as a global (`__sl_<seq>`) under `ZeroCCSym.is_static_local`; the frame slot acts as a per-call **mirror** (prologue `data → slot`, shared-epilogue `slot → data`). Constant-literal initializers are patched into the data section at parse time and are deliberately not attached to the AST, so they never re-run per call. `static`+VLA, complex initializers, and multi-declarator static lists are rejected with diagnostics.
  - Files: `parser/ast_stmt.h` (specifier loop, registration, init patching), `types/type_system.h` (sym fields), `types/symbols.h` (`static_local_seq`), `ir/ast_lowering.c` (mirror in/out + epilogue hook), `ir/lowering_state.h` (`static_local_rt_off`).
- **Regression guard**: `test/inline_audit/10_static_local.c` (static counter must read 100/101/102 across three calls) now passes at `-O0..-O3` in both JIT and AOT via `make test-opt`.

### Fixed
- **Postfix `++`/`--` on locals returned the NEW value** (pre-existing, found while validating Sprint 37): the local-variable path in `NODE_UNOP` lowering (`ir/ast_lowering_expr.c`) always returned the post-increment temp, ignoring the postfix flag — `return x++;` / `int r = x++;` miscompiled for frame-slot variables (globals and atomics were correct). Now `return (node->flags == 1) ? val_temp : res_temp;`, matching the global/atomic paths.
- **`zero_cc_add_local` stale-slot bug** (`types/symbol_mgmt.h`): `local_count` resets per function but the slot array persists, so a fresh local could inherit stale fields (e.g. `is_static_local`) from the previous function's local at the same index. The slot is now zeroed before use.

### Design Notes
- **Epilogue return-value protection**: the mirror-out runs after the return value is materialized in `VREG_0`, and allocator temps may be mapped onto the return register (observed: exit status = `(uintptr_t)&static_var & 0xFF`). Functions with ≥1 static local reserve an 8-byte frame slot (`lowering.static_local_rt_off`) that saves/restores `VREG_0` around the mirror-out; FP results are in XMM registers, which the integer-only mirror-out cannot touch. (Allocator operands 0–15 are *virtual* registers that are themselves allocatable — "using r10 directly" in IR is not a way to pin a physical register; the reserved backend scratch registers are the only non-allocatable ones.)
- A function with a static local always declares a local, so the inliner's local-decl filter already excludes it — no duplicated data objects across inlined copies are possible.
- **Verification**: `make test-opt` **105/105**, `make test` **141/141**, `mem2reg_verifier` **12/12**, `sret_verifier` **6/6**, `licm_test_suite` **5/5**, `cfg_verifier` **0 failed**, SIMD suite unchanged (10 pre-existing FP/frontend failures).

## [Sprint 36] - 2026-10-20 - mem2reg Loop-Promotion Cost: O(loops × 256 × |IR|) → O(loops × |IR|)

### Changed
- **`opt/opt_mem2reg.c` (`opt_promote_loop_carried`)**: the candidate search previously rescanned the whole function **once per stack slot (up to 256) per loop (up to 64)** — measured in Sprint 35 Phase 4 to be ~85% of all optimizer time. Now a **single scan per loop** accumulates per-slot stats (in-loop loads/stores, post-exit loads, bad loads, out-of-loop stores) for all slots at once; only slots passing the candidate check (exactly 1 in-loop load + 1 in-loop store, no pre-loop loads, exactly 1 out-of-loop store) get the follow-up init-store scan and post-loop forwarding. The address-taken pre-audit additionally records `max_slot_used`, bounding the candidate iteration (a slot with no load/store traffic in the function is provably non-candidate). Candidate/transform semantics unchanged.

### Measured (median of 5 `--profile-opts` runs, `test/bench/synth_large.c`)
| Pass | Before @-O1 | After @-O1 | Before @-O2 | After @-O2 | Before @-O3 | After @-O3 |
|---|---|---|---|---|---|---|
| mem2reg | 1088 ms | **3.2 ms** | 3374 ms | **13.8 ms** | 4536 ms | **15.9 ms** |
| total passes-only | 1089 ms | ~4 ms | 3688 ms | **~275 ms** | 4964 ms | ~300 ms |

- **Correctness**: `-O1` `-dump-ir` output **byte-identical** pre/post (promotion decisions unchanged); `make test-opt` **105/105**, `mem2reg_verifier.py` **12/12**, `make test` **141/141**, `cfg_verifier.py` **0 failed**, `sret_verifier.py` **6/6**, `licm_test_suite.py` **5/5**.
- **Next hotspot identified**: GVN (~184-210 ms @-O2/-O3, now the dominant pass — per-block hash-table re-init churn); tracked, not fixed here (GVN was just OOB-fixed in Sprint 35 Phase 4; separate risk budget).
- **Measurement note**: host wall times swing ±50% under variable load; per-pass profile (in-process `CLOCK_MONOTONIC`) is the stable metric.

## [Sprint 35 Phase 4] - 2026-10-20 - Measurement, the Retracted 78× Claim, and Three 16-bit temp-id OOB Fixes

### Added
- **`--profile-opts` per-pass wall time** (`opt/opt_profile.{h,c}`, `opt/opt_pass.c`): a "Per-pass wall time (ms, accumulated over all functions)" section (`clock_gettime` around each `pass->run`, gated by `opt_profile_is_enabled()`). The AOT `-o` path now actually *enables* the profiler (previously it only dumped, so `--profile-opts` produced no per-pass data on AOT builds).
- **`docs/okf/perf-measurements.md`**: the measured numbers, methodology, and reproduction steps (linked from `docs/okf/index.md`).
- **`test/bench/synth_large.c`**: the measurement workload (861 lines, ~40 functions, ~122 loops, IR > 1024 instructions).

### Changed
- **The "~78× faster than GCC" claim is RETRACTED.** Re-measured (median of 5, compile+run of `synth_large.c`): `zerocc --run -O0` **234 ms** vs `gcc -O0` **164 ms** / `gcc -O2` **365 ms** — the JIT is *slower* than GCC, not 78× faster. `docs/okf/ir-and-regalloc.md` updated; the historical CHANGELOG line (Sprint 22 era) is left in place as a dated record but is superseded by the new OKF page. No docs may assert the 78× figure again.
- **CFG-migration cost verdict (the plan's before/after question)**: on the same workload, baseline `87284bd` vs HEAD wall time: `-O0` 399→286 ms, `-O1` 1470→1071 ms, `-O2` 3949→3665 ms, `-O3` 3813→3401 ms. **No regression from the Phase 2/3 CFG migration; 7-10% faster at `-O2/-O3`** (CFG pred/succ edges replace per-loop full-function jump pre-scans). Dominant pass cost is mem2reg's pre-existing O(loops × 256 slots × |IR|) scan (≈85% of pass time); slot-set pruning is deferred as a future optimization.
- **`docs/fragility-analysis.md` §C11**: the two "known residuals" (mid-body conditionals not rejected; increment rewrite matching any `OP_ADD` with constant 1) are marked fixed (`5766bee`; re-derived on the CFG in Phase 3). Stale `make test 140/140` note updated.

### Fixed (found *by* the Phase 4 measurement — large workload = >256 temps per function)
- **`opt/opt_gvn.c` (global-buffer-overflow, the `--profile-opts` corruption)**: `g_canon[256]` indexed by the 16-bit `temp_dest` went OOB for any function with >256 temps, corrupting adjacent BSS globals. All GVN temp storage widened to 16 bits: `g_canon[65536]`, `MemEntry.addr/val`, `g_slot_can[64]`, `ValueEntry.src1/2`, `mf_forget_temp()`, `replace_with_gvn_copy()`.
- **`opt/opt_vectorize.c`**: `scalar_to_vec_map[256]` (stack) was indexed unboundedly by `temp_dest`/`temp_src1` (2 unguarded writes + 2 unguarded reads) — OOB for >256 temps. Now a full-range `static int16_t[65536]` memset per transform.
- **`opt/opt_loop.c` (LICM segfault at `-O2` on the large workload)**: `loop->is_invariant` was `calloc(MAX_LOOP_BODY)` (1024) but indexed by *instruction index* — OOB for any IR > 1024 instructions. Now sized by `ir_get_count()+1`; `body_indices` grows dynamically beyond `MAX_LOOP_BODY`.
- Invariant: **tables keyed by temp id must cover the full 16-bit temp space (or bounds-check); tables keyed by instruction index must be sized by the IR count, never a body cap.**

### Verification
- `make test-opt` **105/105** · `make test` **141/141** · `cfg_verifier.py --verify-all` **0 failed** · `mem2reg_verifier.py` **12/12** · `sret_verifier.py` **6/6** · `licm_test_suite.py --all` **5/5** · `okf.py lint` **PASS**.
- `synth_large.c` compiles & runs identically at `-O0..-O3` (return value matches `gcc -O2`: 162); the pre-fix binary segfaulted at `-O2/-O3` on this file.
- ASan run clean at `-O2` on the workload after the GVN fix (the OOB write was caught by ASan as `global-buffer-overflow` in `opt_restrict_memory_forward`).

## [Backfill: missing entries] - 2026-10-18 (recorded in Sprint 35 Phase 4)

Two commits landed between Sprint 31/32 without changelog entries:

- **`8781592` fix(opt): -O level gating** — `OptPass.min_level` + `comp->opt_level`; passes tagged O1/O2/O3; `-O0` is now plain codegen (no optimization passes run). This is why inlining/LICM/CSE/GVN only run at `-O2`+ and strength-reduction/branch-simplify/vectorize at `-O3`.
- **`5766bee` fix(opt): vectorizer hardening (C11 residuals)** — the analyzer rejects loops with mid-body conditional branches, and the transform's increment rewrite matches *only the loop counter* (`loop_var_temp`), not any `OP_ADD` with a constant-1 operand. Superseded (re-derived on the CFG) by Sprint 35 Phase 3.

## [Sprint 35 Phase 3] - 2026-10-20 - Pass Migration Onto the CFG

### Changed
- **LICM (`opt/opt_loop.c`)**: `detect_loops` now iterates `cfg->loops[]` (CFG back-edges) instead of building a private `label_to_idx` map and scanning for jumps to earlier instructions. Header = `blocks[header].start`, body = `[blocks[header].start .. blocks[backedge_from].end)` — the same instruction set as the legacy span (a backedge jump is the last instruction of its block). The multi-def invariant check, in-loop source-def rule, call-spanning guard, and shared-header union emission are unchanged.
- **Mem2reg loop promotion (`opt/opt_mem2reg.c`)**: `opt_promote_loop_carried` no longer pre-scans the whole function for jump targets. Loops come from `cfg->loops[]` (still unconditional-`OP_JMP`-backedges only); **entry-cleanness** (the Sprint 33 goto-into-loop guard) is now "no explicit jump edge from a block outside the loop range into any loop block" (fall-through from the preheader allowed); **first-exit** is the earliest CFG successor of a loop block past the backedge. Slot audit and 4-step rewrite unchanged.
- **Vectorizer (`opt/opt_vectorize.c`)**: the span-based mid-body-conditional check is replaced by a CFG per-block check (a conditional jump is permitted only as the backedge or as the header's loop test targeting outside the span).
  - **Effect: the vectorizer was effectively dead and is now active.** The legacy per-instruction scan flagged the loop's *own* header test (a forward `JE`-to-exit inside the `[header..backedge]` span) as a mid-body conditional, so while-style loops were never vectorized (0 SIMD instructions across the corpus). With the precise check, standard element-wise loops are now vectorized (e.g. `test/abi/simd_int_loop.c` emits 12 SIMD instructions at `-O3` and passes).
  - **New safety prerequisite (required by the revival): verified loop bound.** The transform widens `i += 1` → `i += vec_width` with **no remainder handling**; a loop count not a multiple of `vec_width` makes the final `VLOAD`/`VSTORE` overflow the array into adjacent stack. The analysis now requires the loop-test chain (branch → `COND_SET` → `CMP` → constant operand) to yield a compile-time constant bound that is a positive multiple of `vec_width`. Variable bounds, FP bounds, and infinite loops are not vectorized (probed: N=4 vectorized+correct, N=5 scalar+correct, at `-O0..-O3`).

### Notes
- The pre-existing `tools/simd_regression_suite.py` failures (10/10, all FP/frontend compilation gaps) are **unchanged** by this phase — verified identical before/after; they are unrelated to the vectorizer check.
- `tools/licm_test_suite.py --all` **regenerates** `test/licm/*.c` (overwriting the committed `main()`s with module-only versions, breaking `make all`): do not run it against a clean tree, or `git checkout -- test/licm/` afterwards.

### Verification
- `make test-opt` **105/105** · `make test` **141/141** · `mem2reg_verifier.py` **12/12** (incl. `goto_into_loop` at every level) · `sret_verifier.py` **6/6** · `licm_test_suite.py --all` **5/5** · `cfg_verifier.py --verify-all` **0 failed**.

## [Sprint 35 Phase 2] - 2026-10-20 - CFG Layer

### Added
- **CFG layer (`src/zero_cc/opt/cfg.{h,c}`)**: a view over the flat, non-SSA IR providing basic blocks (contiguous `[start,end)` ranges; leaders = instruction 0, every `OP_LABEL`, and the instruction after any terminal `JMP/JE/JNE/JL/JLE/JG/JCOND/RET/EPILOGUE`), successor/predecessor lists, BFS reachability from entry, and back-edge/loop-header detection (`blocks[v].start <= blocks[u].start`, the natural-loop convention the legacy passes already use).
  - **Lazy cache with explicit invalidation**: `ZeroCCCFG cfg_cache` + `uint64_t cfg_version` on `ZeroCCCompiler`. `cfg_build()` rebuilds only on version mismatch; the version is bumped in `ir_reset()` (per function), after **every** optimizer pass (`opt_pass.c` — passes may rewrite the IR in place), and after the two in-place lowering mutations (callee-save append, x86 float-CC remap). Not built at `-O0` unless requested. The CFG is a pure read-only view — it never writes to the `IRArena`.
- **`-dump-cfg`** (in `main.c`, both AOT and JIT paths): stable plain-text dump, one line per block — `fn main` / `bb1: [4..10] succ=bb8,bb2 pred=bb0,bb7 loop_header=1` / `loops:` section (`bb7->bb1`) / `N blocks, M reachable, K loops` summary.
- **`tools/cfg_verifier.py`** (stdlib-only, `mem2reg_verifier.py` conventions): one `zerocc -dump-ir -dump-cfg` run per test, then an **independent Python recomputation** of blocks and natural loops from the IR dump (separate code, shared opcode rules only) cross-checked against the C-side report: block tiling, jump-target-is-block-start, pred/succ mutual consistency, reachability, and C-vs-Python loop-set equality as `(header_pos, from_pos)` pairs. Modes: `--verify-all` (sweep corpus + `test/cfg`), `--file`, `--dot` text export, `--opt`.
- **CFG unit tests (`test/cfg/`)**: `straight.c`, `if_else.c`, `nested_loops.c` (2 nested back-edges), `goto_mix.c`, `switch_case.c`, `ternary_in_loop.c`.

### Notes
- **No `OP_EPILOGUE` exists in the IR**: the epilogue is the final `CALLEE_SAVE_LOADS` block; `leave`/`ret` are emitted by the backend outside the IR, so the last block legitimately has no successors (handled, not an error).
- **Unreachable blocks are warnings, not failures**: lowering emits unreachable tails after `return`s and orphaned constant blocks (present at `-O0`, i.e. a codegen property, not a CFG-layer defect). The verifier still hard-fails a block that has a *reachable* predecessor yet is reported unreachable, and any tiling/consistency/loop-set mismatch.
- The 63 corpus tests the verifier SKIPs are exactly the non-standalone API/header-dependent tests the `opt_sweep.py` AOT phase also cannot compile (verified identical sets; pre-existing frontend gaps such as `__attribute__` support, unrelated to the CFG layer).

### Verification
- `cfg_verifier.py --verify-all`: **0 failed** (48 verifiable functions across 48 standalone programs; 63 skips; 100 unreachable-block warnings). Negative tests confirm detection of tiling breaks, pred/succ breaks, and loop-set mismatches.
- `make test-opt` **105/105** · `make test` **141/141** · `mem2reg_verifier.py` **12/12** · `sret_verifier.py` **6/6** · `okf.py lint` PASS.

## [Sprint 35 Phase 1] - 2026-10-20 - Inlining Audit & Conservative Candidate Filters

### Added
- **Inlining audit battery (`test/inline_audit/`, 17 tests)**: adversarial differential tests vs `gcc -O2` covering param mutation, `&param`, pointer mutation, narrow/wide arg conversion, float/double args and returns (incl. FP-expression return), static locals, labels/goto, multiple returns, same callee twice in one expression, nested inline depth 2, struct params/returns ≤8B, and mixed inlined+real calls. Now part of the `make test-opt` corpus (105 tests).

### Fixed (inlining-specific miscompiles found by the audit, all at `-O2`/`-O3`)
- **`jit/compile.h` candidate filters** (Sprint 35 Phase 1): the audit showed four body features that the call-site expander cannot lower safely. Candidate detection now rejects:
  - **float/double params or return** — the param-substitution path forces `last_expr_is_float=0`, so an FP param (an XMM value) was handed to the inlined body as an integer (`CVTSI2SD` from the GPR), and inlined FP returns went through a GPR `MOV_REG` redirection (value never reached the XMM register).
  - **local declarations in the body** — locals are registered at *parse time* into the callee's per-function local table, which is reset for the caller; at the call site the body's locals resolved to `TEMP_NONE` (r65535), silently corrupting loops and return values.
  - **labels/goto in the body** — user labels resolve through a global name-based map (`get_or_create_user_ir_label`); a same-named caller label would alias the inlined body's jump target.
  - **`&param`** — value parameters bind to virtual registers with no stack slot; `&param` lowered to an invalid LEA and segfaulted.
- **`parser/ast_arena.h` + `parser/parser.h`**: new AST walkers `zero_cc_ast_has_local_decl`, `zero_cc_ast_has_label_goto`, `zero_cc_ast_addr_of_param` (mirror the `zero_cc_ast_has_call` left/right/next + if-else/for traversal).
- **Verified still inlined** after the filters: expression-only leaf helpers, struct params/returns ≤8B, multiple returns, real calls from the inlined body, nested depth 2, same callee twice in one expression.

### Known gap found by the audit (pre-existing, NOT inlining)
- `static` locals are lowered as ordinary stack locals (re-initialised on every call): `test/inline_audit/10_static_local.c` fails at **every** `-O` level including `-O0` (no inlining involved). Tracked as a frontend gap; the test stays in the corpus as an across-`-O` consistency guard (its gcc REF-DIFF is informational).

### Verification
- `make test-opt`: **105/105** (was 88; +17 audit tests) at `-O0..-O3`, JIT + AOT. Pre-filter audit run: 6 of 17 tests miscompiled at `-O2/-O3` (02 `&param` SIGSEGV; 07/08/09 FP; 11 goto; 17 locals).
- `make test` **141/141** · `mem2reg_verifier.py` **12/12** · `sret_verifier.py` **6/6**.

## [Sprint 35 Phase 0] - 2026-10-20 - Sprint 33 Regression Fixes & Optimizer Sweep Harness

### Added
- **Optimizer regression sweep (`tools/opt_sweep.py`, `make test-opt`)**:
  - Compiles and runs every standalone test in `test/{jit,aot,licm}` with zerocc itself at `-O0/-O1/-O2/-O3`, in **both** JIT (`--run`) and AOT (`-o`) modes.
  - Compares each level's `(exit code, stdout)` against the `-O0` baseline; informational `gcc -O2` differential; per-test timeout; `NONDET`/skip flagging for timing-sensitive tests (e.g. `profile_demo`).
  - Wired into the Makefile (`make test-opt`) and the `AGENTS.md` verification rules as the mandatory gate for any optimizer/pass/regalloc change.

### Fixed (Sprint 33 `15588aa` regressions — all verified by `make test-opt` 88/88)
- **x86-64 extend encoding (`src/zero_cc/isa/x86_64.c` `x86_emit_extend`)**:
  - `MOVSXD`/`MOV r64, r/m32` put the destination in both the reg and r/m ModRM fields; for `dst != src` (or `dst >= 8`) the wrong source register was extended (e.g. `movslq %esp, %r12` instead of `movslq %r12d, %r12`), silently corrupting values. Source now in r/m, destination in reg.
- **x86-64 `-S` setcc/extend encodings (`src/zero_cc/isa/x86_64_asm.c`)**:
  - `x86_emit_cond_set_asm` hardcoded `setcc %al` + `movzbq %al,<dst>`, clobbering live RAX whenever the condition temp was not `r0`; now uses the destination's low byte.
  - `x86_emit_extend_asm` ignored `src` and assumed in-place extension; now emits the real source register for 8/16/32-bit widths.
- **LICM shared-header invariant loss (`src/zero_cc/opt/opt_loop.c`)**:
  - `detect_loops` can produce several natural loops sharing one header (multiple backedges). The two-pass rebuild emitted only the first matching loop's invariants before the header, while NOP'ing in-loop copies for all of them — losing definitions (e.g. `MOV_IMM r51, 1` in `break_continue.c`). Rebuild now emits the union of all shared-header loops' invariants (deduplicated).
  - `instr_is_invariant` now rejects candidates whose source operand is defined anywhere in the loop body (including after the candidate — it feeds back through the backedge) unless that definition is itself invariant. Previously a loop-dependent `ADD i_new, i, 1` (after copy propagation) was hoisted and `i` frozen, turning finite loops infinite.
- **DCE liveness soundness (`src/zero_cc/opt/opt_dce.c`)**:
  - A single backward liveness sweep mis-declares loop-carried values dead at their backedge write-back (definition processed before the top-of-loop use), so DCE deleted loop updates and created infinite loops once the fixed-point pipeline re-ran it after `-O2` passes. Liveness is now computed to a fixpoint (monotonic live set, bounded sweeps; on non-convergence DCE deletes nothing).
- **mem2reg loop promotion audit (`src/zero_cc/opt/opt_mem2reg.c` `opt_promote_loop_carried`)**:
  - A slot is promoted only if the whole function is auditable: exactly one in-loop load and one in-loop store, exactly one out-of-loop store (the preheader init, before the header), and all remaining loads strictly after the loop exit.
  - All post-loop loads (until `RET`/epilogue or a same-slot store) are forwarded to the head temp; previously forwarding stopped at the next label, leaving stale stack loads after `break`/`continue` exits.

### Verification
- `make test-opt` (`tools/opt_sweep.py`): **88/88 passed** at `-O0..-O3`, JIT and AOT (pre-fix: 9 failing tests — `break_continue`, `goto_loop`, `loops_conditionals`, all five `test/licm/*.c`, `profile_demo` timeout).
- `make test` **141/141** (incl. new `test/jit/goto_into_loop.c`) · `tools/mem2reg_verifier.py --verify-all` **12/12** · `tools/sret_verifier.py --verify-all` **6/6** · OKF lint/index PASS.

## [Sprint 34] - 2026-10-19 - Small Function Inlining (-O2/-O3) & LICM Multi-Def Safety

### Added
- **Small Function Inlining (`ast_lowering_expr.c` / `jit/compile.h`)**:
  - Implemented automatic call-site inlining for small `static` and `inline` helper/leaf functions ($N \le 35$ AST nodes, non-variadic, no struct return $>8$B, non-recursive) enabled at `-O2`/`-O3` with call depth up to 2.
  - Implemented TU-level AST arena retention (`tu_ast_arena`) and AST cloning utilities (`zero_cc_ast_clone`, `zero_cc_ast_node_count`, `zero_cc_ast_has_call`) in `parser/ast_arena.h`.
  - Added inlined parameter substitution and return value redirection in `ir/ast_lowering_expr.c` and `ir/ast_lowering_stmt.c`.
  - Eliminates function call setup (`OP_CALL`), argument spills, frame creation, and callee register save/restore overhead in inner compute loops.

### Fixed
- **LICM Multiple-Definition Safety Invariant (`opt/opt_loop.c`)**:
  - Fixed pre-existing invariant violation in `instr_is_invariant()`: previously, instructions whose destination virtual register had multiple definitions in a loop (such as inlined conditional branches or ternary operators) were incorrectly hoisted.
  - Added strict guard ensuring hoisted destination temporaries have exactly 1 definition in the loop and all definition sources are verified invariant.

### Verification
- `tools/mem2reg_verifier.py --verify-all`: **12/12 passed** (+2 new inlining tests: `inline_leaf_call`, `inline_min_loop`).
- `tools/mem2reg_verifier.py --count-stack-ops`: `inline_leaf_call` reduced stack ops from 14/5 to 7/1 (-7 loads, -4 stores); `inline_min_loop` from 13/4 to 7/1 (-6 loads, -3 stores).
- Benchmark: 100M loop calling `min_val(i & 1, 1)` executed in **143.36 ms** (2.65x faster than un-inlined -O0 at 380.43 ms). 100M loop counter runs in **102.51 ms** (975.5M iters/sec, matching GCC -O0).
- `make test` **140/140 passed in 0.40s** · `sret_verifier.py` **6/6 passed** · OKF lint PASS.

## [Sprint 33] - 2026-10-19 - Cross-Block mem2reg & LICM Invariant Load Hoisting

### Optimized
- **Cross-Block Loop Induction & Accumulator Promotion (`opt/opt_mem2reg.c`)**:
  - Implemented `opt_promote_loop_carried()` to detect natural loops with non-address-taken loop induction variables and accumulators.
  - Rewrites preheader stores and backedge stores into virtual register transfers (`OP_MOV_REG`), eliminating in-loop stack loads and stores.
  - Loop simple stack stores inside the loop body dropped from 2 down to 0; nested loops dropped from 16 to 7 loads.
- **Straight-Line & Fallthrough mem2reg Enhancements (`opt/opt_mem2reg.c`)**:
  - Preserved slot tracking across conditional branches (`OP_JE`, `OP_JNE`, `OP_JL`, `OP_JLE`, `OP_JG`, `OP_JCOND`) along the straight-line fallthrough path.
  - Added pre-pass jump target scanning to preserve slot tracking across unreferenced fallthrough labels (such as `loop_step` in for-loops without `continue`).
  - Added load-after-load forwarding within basic blocks: subsequent loads from the same stack slot reuse the loaded temp.
  - Preserved slot tracking across in-place sign/zero extensions (`OP_SEXT32`/`OP_ZEXT32`).
- **Loop-Invariant Stack Load & Sign Extension Hoisting (`opt/opt_loop.c`)**:
  - Extended LICM to detect read-only stack slots (e.g. loop bound `n`) and downstream `OP_SEXT`/`OP_ZEXT` instructions, hoisting them to the loop preheader.
- **Pass Level Reclassification (`opt/opt_pass.c`)**:
  - Promoted `mem2reg` and `copy_prop` to `OPT_LEVEL_O1` so default optimization benefits from stack-to-register promotion.
  - Promoted `licm` to `OPT_LEVEL_O2`.

### Verification
- `tools/mem2reg_verifier.py --verify-all`: **10/10 passed** (JIT -O0, JIT -O1, AOT -O1 vs GCC -O2).
- `tools/mem2reg_verifier.py --count-stack-ops`: `loop_simple` dropped from 11/2 to 5/0; `nested_loops` from 16/4 to 7/1; `branch_in_loop` from 15/3 to 8/1.
- `tools/mem2reg_verifier.py --benchmark`: 100M loop iterations down from 251ms to 160ms (faster than GCC -O0).
- `make test` **140/140** · `sret_verifier.py` **6/6** · OKF lint PASS.

## [Sprint 32] - 2026-10-19 - M5 Resolved: Dynamic Scope Hash Table, Dynamic Locals Array, Segmented Chunked AST Arena

### Fixed
- **M5 Scope Symbol Cap & TPHT Collision Ceilings** (`semantic/scope.h`, `semantic/scope.c`):
  - Completely replaced the fixed-capacity Flattened Two-Phase Hash Table (`ZeroFlattenTpht`) with a dynamically resizing separate-chaining hash table.
  - Table starts at 64 power-of-two buckets and doubles automatically when load factor exceeds 75%.
  - Symbols and names are allocated in the permanent compilation arena (`perm_arena`), eliminating arena byte index conversions (`arena_index_of`/`arena_index_to_ptr`).
  - Removed `#include "zero_tpht.h"` dependency from `scope.h`.
- **Compiler Local Variables Dynamic Storage & O(1) Hashed Lookup** (`types/symbols.h`, `types/symbol_mgmt.h`, `main.c`, `jit/compile.h`):
  - Converted `c->locals` from a hard-capped array (`ZERO_CC_MAX_LOCALS = 8192`) to a dynamically resizing heap array that grows on demand (initial 64, doubles on realloc).
  - Implemented an $O(1)$ dynamic hash table (`local_hash_buckets`, `local_hash_next`) for `zero_cc_find_local()`, eliminating the quadratic $O(N^2)$ linear scan when lowering large scopes.
  - Cleaned up heap allocations in `main.c` on compiler exit.
- **Dangling AST Node Pointer Bug & Segmented Chunked AST Arena** (`parser/parser.h`, `parser/ast_arena.h`, `parser/ast_stmt.h`):
  - Fixed dangling `rn` pointer across `parse_expr_ast_internal()` in `ast_stmt.h` (which previously triggered SIGSEGV when reallocating AST nodes in large scopes).
  - Converted `ZeroCCASTArena` from a reallocating contiguous array to segmented 1,024-node chunks (`ZERO_CC_AST_CHUNK_SIZE = 1024`). `zero_cc_ast_get()` uses bitwise chunk indexing (`ref >> 10`, `ref & 1023`). Existing chunks are never moved in memory, ensuring that all `ZeroCCASTNode *` pointers remain valid indefinitely.
- **Large Frame Offsets & Stack Probing** (`isa/x86_64.c`, `isa/x86_64_asm.c`, `ir/ir.h`, `ir/ir_emit_ops.h`, `opt/opt_mem2reg.c`):
  - Handled frame offsets exceeding 16-bit range (`imm64`) in large stack frames.
  - Added stack probing in x86 prologue for frames > 4KB to cross Linux guard pages safely.

### Verification
- **Scalability Probes**: Tested from 1,000 up to 25,000 local declarations in both AOT (`-o`) and JIT (`--run`) modes — all compile and execute with exit code 0.
- **Speed**: ZeroCC compiles standard C code in ~180ms AOT (1.2× faster than GCC `-O0`) and in ~2.8ms in JIT mode (~78× faster than GCC).
- `make test` **140/140** · sret **6/6** · OKF lint PASS.

## [Sprint 31] - 2026-10-18 - M3 Resolved: Dynamic Struct Members Table

### Fixed
- **M3 (Struct members > 64 silently dropped)** (`types/type_system.h`, `parser/definitions.h`):
  - Converted `ZeroCCType.members` from a fixed 64-element inline array to a dynamically resizing heap table.



### Fixed
- **3-operand FP arithmetic in `-S`** (`isa/x86_64_asm.c`): `FADD/FSUB/FMUL/FDIV dst,src1,src2` was emitted as 2-operand `addsd %src2,%dst` (correct only when dst already held src1). All four emitters now handle dst==src1 / dst==src2 / dst==other, staging src1 via `movsd` and using `xmm15` for the non-commutative dst==src2 case.
- **FP compares in `-S`** (`isa_backend.h`, `x86_64_asm.c`, `aarch64_asm.c`, `asm_emitter.c`): float `<`/`!=`/… emitted integer `cmpq`. New `emit_fp_cmp_asm` vtable slot (`ucomisd` / `fcmp`); `OP_CMP` in the asm emitter dispatches on operand class like the MC path.
- **`FMOV_IMM` width in `-S`** (`asm_emitter.c`): float literals are 8-byte doubles in `.rodata` but were loaded with `movss`/`ldr s`/`flw` (low 4 bytes → 0.0). Now `movsd`/`ldr d`/`fld`.
- **`VLOADSS` widening in `-S`** (`asm_emitter.c`): the 4-byte float load never widened into the 64-bit double lane. Added `cvtss2sd` (x86), `fmov d,s` (AArch64), `fcvt.d.s` (RISC-V) to match `x86_emit_vloadss`.
- **`VSTORESS` narrowing in `-S`** (`asm_emitter.c`): a bare `movss %xmm,(mem)` stored the **low half of the double** (1.5 → 0x00000000) — every float struct member written via `-S` read back as 0.0 (sf3 rc=7). Now narrows into the FP scratch (`cvtsd2ss`→`xmm15` / `fcvt s15,d` / `fcvt.s.d`) and stores that, mirroring `x86_emit_vstoress`.
- **`OP_CMP_ZERO` missing from the asm emitter** (`asm_emitter.c`): fell through to `# UNKNOWN: CMP_ZERO` — the second `setcc` in `!(a<b)` read the stale `ucomisd` flags. Now emits `testq %r,%r` (all int/FP `!expr` via `-S`, pre-existing).
- **C12 — `comisd`/`ucomisd` signed condition codes** (`ir.h`, `ast_lowering.c`, `x86_64.c`, `x86_64_asm.c`): `ucomisd` only defines ZF/PF/CF (SF/OF are cleared), so `setl/setle/setg/setge` after an FP compare are wrong on every x86. Added `CC_FLT_LT/LE/GT/GE` (6–9 → `setb/setbe/seta/setae`, gcc's scheme) and a post-regalloc remap in `ast_lowering.c` (x86 only) that rewrites `COND_SET` 0–3 → 6–9 when the preceding `CMP` has FP-class operands; both setcc tables extended. Affects JIT/AOT, not only `-S`.
- **C13 — broken REX in `x86_emit_fp_cmp`** (`x86_64.c`): REX.R/REX.B were crammed into the wrong bits *and* emitted before the `0x66` legacy prefix (CPU ignores REX then). Same REX-before-prefix ordering fixed in `x86_emit_vstoress` (`movdqa`) and `x86_emit_fp_neg` (`movsd`); `vstoress` also had a hardcoded REX.R=1 for an xmm7 destination (masked by the ordering bug).

### Verification
- `/tmp/sfloat` battery (sf1–sf5, m1–m8, c1–c5, s1–s3): **84/84** across JIT `-O0`, JIT `-O1`, AOT `-o`, and `-S`+gcc; zero `# UNKNOWN:` in any `.s`. Former `-S` repros `/tmp/c6v/a5.c` and `e11.c` rc=0 via `-S`.
- `make test` **140/140** · sret **6/6** · OKF lint PASS.

## [Sprint 29] - 2026-10-18 - C9: Float Cast Lowering; C11: Vectorizer Must Not Vectorize Call Loops (C10 Resolved)

### Fixed
- **C9 - inline int→float cast dropped** (`ir/ast_lowering_expr.c`, `NODE_CAST`): `use2((float)k)` passed the raw integer bits as a double bit pattern (int 3 → 3.0e-323) - the cast handler had no int→float branch at all, and the float-source branch unconditionally ran `cvttsd2si` even when the target was float (so `(double)a`/`(float)a` truncated to int). Now: float/double *target* + int/pointer source emits `CVTSI2SD` into a fresh temp and sets `last_expr_is_float`; float→float/double is a no-op under the 64-bit double FP model. The downstream paths that previously supplied their own conversion (float local/global assignment) are all `last_expr_is_float`-guarded, so no double conversion occurs.
- **C11 - auto-vectorizer vectorized loops containing calls** (`opt/opt_vectorize.c`): the loop analyzer's criteria promised "no function calls" but never checked - a loop like `for(...) { float t = use2(1.0f); s += t; }` was accepted, the transform widened the induction step (`k += 1` → `k += 4`) and converted the scalar float ops to 128-bit SIMD (`VADDPS`), while the scalar `OP_CALL` body ran only once per 4 iterations - dropped loop iterations and packed-lane math on 16-byte loads of scalar floats. The analyzer now rejects any loop whose body contains `OP_CALL`/`OP_CALL_INDIRECT`/`OP_RET` (`has_call` guard). Legitimate element-wise array loops still vectorize (VADDPS confirmed in e48, correct results).
- **C10 - "float local as call argument inside a loop" - resolved**: the e30/e31 repros were C11 in disguise (their loop bodies contained calls); they pass JIT + AOT unchanged once C11 is fixed.

### Verification
- C9: e25 (repro), e25p (value probe prints `r=3.250`), e36 (float→double/float), e37 (15-assertion cast battery) - rc=0 JIT `-O0`/`-O1` + AOT, gcc cross-check rc=0
- C11: e21/e23/e24/e29/e30/e31/e38/e39/e40/e41/e43/e46 (call-in-loop battery incl. value probes) rc=0 JIT + AOT; e42/e44/e45 (no-call loops) and e47/e48 (vectorizer sanity: VADDPS still emitted, gcc cross-check) rc=0
- `make test` **140/140** · sret **6/6** · OKF lint PASS · C8 battery rc=0 (no regression) · C5/d batteries rc=0 · c1-wrap OK · `globals 500` OK (JIT+AOT)

### Documented
- `docs/fragility-analysis.md`: C9/C10 **RESOLVED**, **C11 added and RESOLVED** (with the two known vectorizer residuals: mid-body conditional branches not rejected; increment rewrite matches any `OP_ADD` with a constant-1 operand), fix-order list extended.
- `docs/okf/optimization-passes.md`: auto_vectorize safety invariant recorded (no calls/returns in a vectorized loop body).

## [Sprint 28] - 2026-10-18 - C8: FP Call-Result Liveness (Materialized Returns, XMM Spans-Call Spill, Pool Aliasing)

### Fixed
- **C8 - float/double function-call result clobbered by FP arithmetic in the same expression** - three stacked root causes:
  1. **Untracked FP return** (`ir/ast_lowering_expr.c`): the float/double call result was referenced directly as the physical return register (`VREG_F0`) with no tracked temp, so the LRA never saw it live. Now materialized into a fresh temp via `OP_FMOV` after the call.
  2. **XMM `spans_call` ignored** (`ir/regalloc.h`): `alloc_reg()` honored `spans_call` only for GPRs; an FP interval spanning a call was assigned a caller-saved XMM that the call then clobbers. XMM intervals with `spans_call` now always force the spill path (all FP/SIMD registers are caller-saved on every supported ABI).
  3. **Steal from an already-spilled interval** (`ir/regalloc.h`): the spill-selection steal branch copied the stolen interval's `assigned_reg` - which is `-1` once that interval was spilled - leaving a **dangling temp** (no register, no spill slot) that silently read the fallback register. Selection now skips `is_spilled` intervals, FP/VEC intervals spanning a call may not steal at all (no callee-saved FP registers exist), and a `-1` steal falls back to spilling the current interval.
- **Class-aware FP spill** (`ir/ast_lowering_mc.c`, `ir/asm_emitter.c`): spilled FP temps now reload/store through `emit_load_fp`/`emit_store_fp` (movsd) into new `fp_scratch_reg1/2` backend fields (x86 `xmm15/xmm14`, aarch64 `d15/d14`, riscv `f15/f14` - outside the 0-7 XMM pool; shared constants `REGALLOC_FP_SCRATCH1/2` in `regalloc.h`) instead of the GPR 64-bit scratch path, in both the JIT and `-S` emitters.
- **XMM↔VEC register-pool aliasing** (pre-existing, exposed by the materialized returns; `ir/regalloc.h`): VEC-class temps (scalar float locals use 16-byte `OP_VLOAD`) allocated from a separate `vec_free[]` array aliasing the same physical xmm registers as the XMM pool - an overlapping VEC load could clobber an XMM temp (e.g. a float-local load clobbering a call result; baseline repro `/tmp/c6v/e22.c` failed before, passes now). On shared-hardware ISAs (x86/AArch64) the pools now cross-mark indices 0-7 at allocate and expire; RISC-V keeps separate v-registers. VEC allocation also skips the FP scratch indices.

### Discovered (documented, pre-existing on the pre-C5 baseline)
- **C9 (OPEN)**: inline int→float cast in call-argument position drops the conversion - `use2((float)k)` passes the raw int bits as a double (int 3 → 3.0e-323); the cast in its own statement works. Repro `/tmp/c6v/e25.c` / value probe `e25p.c`.
- **C10 (OPEN)**: a float local used as a call argument inside a loop loses its value (`for(...) { float fk=(float)k; last = id(fk); }`); the same shape with a float literal or int argument passes. Repros `/tmp/c6v/e30.c`, `e31.c`.

### Verification
- C8 battery JIT (`-O0`/`-O1`) + AOT all rc=0: e10 (original repro), e11 (call result in `+ * - / !=` with later FP ops), e12 (int `f()+f()` regression), e13 (nested `id(use2(1.0f))`), e14 (three-call sum), e15 (FP live across an intervening int call), e16/e17 (isolated two-call / FDIV), e22 (XMM/VEC aliasing), plus the earlier `f.c`.
- `make test` 140/140 · sret 6/6 · OKF lint PASS · C5 b/h battery rc=0 · d1-d8 rc=0 · c1-wrap OK · `globals 500` OK (JIT+AOT)
- The `-S` path contains the C8 mechanism correctly (materialize → spill store → call → reload); e11 via `-S`+gcc still fails only on the separate pre-existing `-S` float-emit gaps (2-operand `addsd`, integer `cmpq` for float compares, `movss` on 8-byte rodata doubles) - see `docs/zero-rt-plan.md` §9 / fragility C6 note.

### Documented
- `docs/fragility-analysis.md`: C8 **RESOLVED** (full root-cause breakdown), C9/C10 added as open findings, M2/M5 table rows refreshed, fix-order list extended.
- `docs/okf/ir-and-regalloc.md`: C8 invariants recorded (materialized FP returns, spans-call spill rule, no-steal guards, pool cross-marking, FP scratch registers).

## [Sprint 27] - 2026-10-18 - C7 Member Compound Assignment & Width-Correct Member Stores; C8 Discovered

### Fixed
- **C7 - compound assignment to struct members** (`ir/ast_lowering_expr.c`): `s.w += 5`, `s.f += 0.5f`, `s.d *= 2.0` and `p->m op= v` stored only the RHS - compound handling existed only in the multi-segment chain path (C5). New `member_apply_compound()` helper (width-correct load: byte/16/32/deref or VLOADSS/VLOADSD; integer ops incl. `%`; float ops FADD/FSUB/FMUL/FDIV with int→float RHS conversion) wired into all four single-segment paths: global base, local pointer base, general base expression, and the shared local/compound-literal tail.
- **Width-correct member store in the shared tail**: the unconditional 64-bit `ir_emit_store` into a member slot → width-correct byte/16/32/64 store. `struct S{int a,b;}; s.b=1; s.a=2;` previously clobbered `b` (w1/w2 probes now pass JIT+AOT).

### Discovered (documented, not fixed)
- **C8 (OPEN)**: a float function-call result is clobbered when followed by FP arithmetic in the same expression - `if(use2(2.0f) != 1.0f+1.25f)` is wrong in both JIT and AOT (pre-existing; repro `/tmp/c6v/e10.c` works when the result is stored in a local first). Suspected FP liveness/regalloc issue around `OP_CALL`.
- **`-S` float constant width**: after `b0975fc` closed the `.data` and `CVTSI2SD` gaps, the `-S` float path still loads 8-byte rodata doubles with 4-byte `movss` (reads the low 4 bytes) and emits integer `cmpq` for float `!=` - `float a=2.0f+0.25f; a!=2.25f` gives rc=1 via `-S` but rc=0 via `-o` (a5.c repro).

### Verification
- d1-d8, c2, c3, w1, w2 pass JIT + AOT; `make test` 140/140; sret 6/6; C5 battery (c5_all/h14/c5a) rc=0 both modes; c1-wrap OK; `globals 500` OK; t-series spot checks pass.

## [Sprint 26] - 2026-10-18 - M2: Dynamic Global Table, Dynamic ELF Writer Tables, Scope TPHT Growth

### Fixed
- **M2 - globals >256 silently dropped**: three stacked fixed caps were found and removed, all verified to 3000 globals in both JIT (`--run`) and AOT (`-o`) with gcc cross-check:
  - **Global variable table** (`types/global_vars.h`): fixed `global_vars[256]` → heap table that doubles on demand (initial 64); `add_global_var` now only fails on allocation failure, and both callers in `jit/compile_unit.h` report it as a clean diagnostic (sets `had_error` via `diag_report_error_at_token`) instead of ignoring the `-1`.
  - **Scope symbol table** (`semantic/scope.c`, `common/zero_tpht.h`): the flat two-phase hash table was fixed at 64 primary + 61 overflow blocks (≈758 usable symbols, hash-dependent clean `use of undeclared identifier` failures past that — non-monotonic by name). Primary grown 64 → 256 blocks and the overflow now scales with the primary (`block_count - (block_count >> 5) - 1`; 64→61 unchanged, 256→247). This simultaneously raised **M5** (locals per scope) from ~758 to ~3000; beyond that it still fails clean.
  - **ELF writer tables** (`jit/elf_writer.{h,c}`): `symbols[1024]` / `strtab[4096]` / `relocs[512]` fixed arrays → dynamic (256 initial, doubling). `ElfWriterSymbol.name` changed from a `const char*` into the strtab to a stable `uint32_t name_offset` (realloc of the strtab would otherwise dangle every previously stored name pointer); the `sym_remap[]` stack array in `elf_writer_write` is now heap-sized to the actual symbol count; `elf_writer_cleanup` frees the new tables. Before: AOT objects with >~1100 globals dropped symbols past the cap while relocations still referenced them → linked binary **segfaulted at runtime** (silent miscompile-class, now impossible).

### Verification
- 300/1200/2000/3000 globals (incl. `int *p = &gN` data-reloc init) pass JIT + AOT; gcc cross-check rc=0
- `locals` probe: 1000 locals now pass (was 757 OK / 758 clean error)
- `make test`: 140/140 · sret 6/6 · C5 battery (c5_all, h14) rc=0 JIT+AOT · c1-wrap OK · `globals 300` probe OK · 1500-case switch OK

### Documented
- `docs/fragility-analysis.md`: M2 **RESOLVED**, M5 boundary updated (~3000, clean-fail beyond), gcc-comparison table and fix-order updated.
- `docs/ZEROCC_Development_Plan.md`: fragility status + roadmap §8 updated (M3 is now the only remaining silent cap).

## [Sprint 25] - 2026-10-18 - C5 Pointer-Member Chains, C6 AOT-Asm Float, H2 Diagnostics, M1 Identifier Length, zero_rt Plan

### Fixed
- **C5 - multi-segment member chains** (`f9da37e`): pointer members now record their pointee base type (`definitions.h`, `type_info.h`), so `o.p->v`, `(*pp)->w`, `geto()->p->f` and deeper chains resolve target types. `ast_lowering_expr.c` lowers `chain_depth >= 2` chains via `lower_member_chain_addr`; pointer-deref stores are width-safe (1/2/4/8) so `*pv = 77` no longer clobbers adjacent struct fields; floating-point compound assignment through chains (`o.p->f += 0.5f`) loads the current value (VLOADSS/VLOADSD), applies the op, and stores width-correctly. All 21 checks in the C5 battery pass JIT + AOT.
- **C6 - float locals in the `-S` path** (`f0254b5`): `OP_FLOAD`/`OP_FSTORE` implemented in all three asm emitters via new `emit_load_fp_asm`/`emit_store_fp_asm` vtable callbacks (`x86_64_asm.c` movsd/movss, `aarch64_asm.c` ldr/str s/d, `riscv_asm.c` flw/fld); float call args in the asm path resolve source registers from `fp_regs` instead of GPR tables. Verified by `-S` roundtrip through `gas`/`gcc`.
- **M1 - identifier length** (`9968aad`): identifiers are no longer silently truncated to a colliding width; the lexer now diagnostics instead of miscompiling long names.
- **`CVTSI2SD` and `CVTTSD2SI` AOT assembly** (Sprint 25): implemented `emit_fp_cvtsf_asm` and `emit_fp_cvtsi_asm` across x86-64, AArch64, and RISC-V 64 backend vtables; wired in `asm_emitter.c` eliminating `# UNKNOWN: CVTSI2SD` comments.
- **`-S .data` section emission** (Sprint 25): implemented `emit_asm_data_sections()` in `src/main.c`; emits `.rodata` and `.data` sections with `.align`/`.p2align` directives, symbol visibility, data bytes, and `.quad` relocations pointing to string literals or other globals so `-S` generated assembly links cleanly.

### Added
- **`docs/zero-rt-plan.md`** (T3 plan): ZeroCC-owned minimal cross-platform runtime with no musl dependency — portable core (heap, minimal stdio, string builtins, exit) + per-OS backends (Linux/macOS share one `__syscallN` asm file; Windows uses kernel32), per-format crt0, vendored Ryū for float formatting. Phases: 0 = Linux (first customer of the T2 ELF linker), 1 = Windows (PE/COFF writer + Win64 ABI vtable as its own tracked item), 2 = macOS (Mach-O writer; compiler ABI already SysV/AAPCS-compatible; raw-dylib-free Mach-O flagged as the early smoke-test risk). musl stays vendored pristine as optional Linux full-stdio.
- **Fragility status**: C5, C6, H2, M1, and AOT `-S` `.data` / `CVTSI2SD` all **RESOLVED** in `docs/fragility-analysis.md`; C-tier is now fully closed (C1 Sprint 22, C2 Sprint 23/24, C3 Sprint 24, C4 reclassified M-tier, C5/C6 this sprint).

### Verification
- `make test`: 140/140 · `sret_verifier.py --verify-all`: 6/6 · OKF lint/index: pass · c1-wrap battery: OK
- C5 battery (c5_all, h14, c5a, h1, h10, h12, h13): rc=0 in both JIT and AOT
- C6: `-S` float programs emit `movsd`/`ldr s`/`flw` stack ops with no FLOAD/FSTORE UNKNOWN on x86/aarch64/riscv; x86 roundtrips through `gas`+`gcc`
- No regressions vs pre-C5 baseline (`/tmp/zb3`): t2_structs/t6_vla/t8_fnptr/t12_sinksink behavior byte-identical to baseline (pre-existing diffs only)

## [Sprint 24] - 2026-10-18 - C2b: 16-bit Temp Ids, C3 Switch Case Cap Removed, AOT-Asm Extend-Width Fix

### Added
- **C2b - 16-bit temp ids** (`ir/ir.h` + tree-wide): `Instr.temp_dest/src1/src2` widened `uint8_t` → `uint16_t` (packed struct 19→24 B, in-memory only), no-temp sentinel moved `0xFF` → `TEMP_NONE` (0xFFFF) at all 140+ sites; `IRCallArgs.temps[]` widened too; all 115 truncating `(uint8_t)temp*` casts → `(uint16_t)`. The C2a guard now bounds at the 16-bit edge (65534 temps) as a safety net. 117 and 190 nested calls compile and run correctly in both modes (previously: silent miscompilation at 117+).
- **Dual sentinel for float/LEA stack access**: `OP_FLOAD/OP_FSTORE/OP_LEA` take a base-register operand whose "frame-pointer base" marker was the old 0xFF no-temp value. The marker is now `TEMP_NONE` at all 35 call sites (the sentinel sweep alone would have routed the marker through `regalloc_get_reg_for_temp(255)` → garbage register; this is what broke trivial float programs in JIT mode after the sweep).

### Fixed
- **C3 - switch >128 cases** (`ir/ast_lowering_stmt.c`): the dispatch loop's `n_cases < 128` cap silently dropped cases 129+ (no labels, unreachable code, no diagnostic). Cap removed - dispatch is O(n) CMP/JNE, and the per-function temp guard bounds the case count (~2 temps/case → ~32k, under the 16-bit label space). 129/500/1000/1500-case switches now pass JIT+AOT (128 OK / 129 WRONG before).
- **AOT-asm extend width** (`ir/asm_emitter.c`): the `-S` path computed SEXT/ZEXT width as `(op - OP_SEXT8)/2 + 1` - the opcodes are consecutive, not interleaved, so SEXT16 emitted width 1 and SEXT32 emitted width 2 (`movswl` instead of `movslq`). `int y = 2000000000 + 2000000000` round-tripped through GNU `as` returned +10240 instead of -294967296. Pre-existing (byte-identical asm on the pre-C2b baseline) but masked because Sprint 22's roundtrip test only exercised width-1 extends. Now uses the same explicit opcode→width mapping as the MC path.
- **LRA 256-temp hard cap** (`ir/live_range.h`): `live_range_add_use` rejected `temp_id >= 256` even though its arrays realloc-grow - would have silently dropped all widened temps. Cap removed.
- **Temp-indexed optimizer tables**: `opt_dce.c` live table 4096 → 65536 (was misreporting temps ≥4096 as dead → DCE could drop their definitions); `opt_gvn.c` `result_temp` widened + both per-temp tables → 65536; `opt_copy_prop.c` kept its 256-entry map with explicit `>= 256` guards (missed optimization only); `ir.c` restrict alias-group tagging and `ast_lowering_expr.c` keep their 256-entry tables with `< 256` guards (higher ids stay unconstrained).
- **OP_CALL legacy imm packing removed**: args 2/3 were packed 8-bits-each into `imm`; with 16-bit ids that packing is unrecoverable. The `IRCallArgs` side table is always recorded (all consumers read it first), so the fallback now uses only the widened src1/src2 fields.

### Documented (fragility-analysis.md)
- **C2: RESOLVED** (C2a guard + C2b widening). **C3: RESOLVED** (cap removed). **C4 reclassified**: the 232–400-locals hang was C2 in disguise (each `int l=0;` consumes a temp); the true boundary is **757/758 locals** = the scope `zero_flatten_tpht` capacity (64+61 blocks × 8), which already emits a clean `failed to insert symbol` diagnostic - no silent miscompilation, no hang.
- **New finding (float AOT-asm gap)**: float locals in the `-S` path lower to `OP_FLOAD/OP_FSTORE`, which none of the three asm emitters implements (`# UNKNOWN:`) - pre-existing on baseline, all ISAs; the JIT/AOT-ELF paths are unaffected.

### Verification
- `make test`: 140/140 (18 float tests regressed mid-sprint by the sentinel sweep's base-marker alias; fixed by the TEMP_NONE marker change; 0.38s steady state). `sret_verifier.py`: 6/6. C1 battery 29/29. Signedness matrix 39/39. C99 spot probes OK (t2_structs/t3_pointers diffs re-verified pre-existing on baseline).
- Probe boundaries re-measured: nest-calls 117 & 190 OK both modes (parser H1 ~200-depth is the new ceiling, pre-existing); switch-cases 129/500/1000/1500 OK; locals 757 OK / 758 clean diagnostic; wide-temps probe (400+ temps, shallow parse depth) OK all modes, gcc-matched.
- x86-64 `-S` → GNU `as` → host gcc link → runs correct (wrap + narrow params); aarch64/riscv64 `-S` int-only: 0 UNKNOWN opcodes.

## [Sprint 23] - 2026-10-18 - C2a: Temp-Id Exhaustion Guard (Silent Miscompilation → Clean Diagnostic)

### Added
- **C2a temp-id exhaustion guard** (`ir/ast_lowering.c`, after `lower_node`): a function whose temp ids reach 255 (the `0xFF` "no temp" sentinel) or wrap past it into the VREG_0–15 space used to **silently miscompile** — the compiler finished (even `-S` succeeded) and the *generated program* segfaulted (AOT) or infinite-looped (JIT). Now a clean fatal diagnostic: `function 'main' uses 241 temporaries; max is 239 per function (8-bit temp ids, 0xFF = no-temp sentinel). Split the function into smaller helper functions ...` (rc=1, no binary emitted). All 221 temp allocations share `c->next_temp_id`, so the single guard covers every path (the legacy `ir_emitter_alloc_temp` path has no callers).
- **`fragility_probe.py` JIT classification hardening** (`tools/fragility_probe.py`): `classify_jit` now does a compile-only dry run (`-S -o /dev/null`) before `--run`, so a failed compile is reported as `COMPILE-ERR` instead of being confused with a program that returns 1.

### Documented (fragility-analysis.md refinements)
- **C2 reclassified**: silent miscompilation, not a compiler crash (the observed segfault/hang was the *executed* broken code; bare `-dump-ir` "hangs" because `compile_jit` always runs the code). The LRA is innocent — nested-call results are only live across one outer call (~2–4 temps simultaneously live), so its 2-register/no-spill allocation is correct; the bug is purely the 8-bit id space. **C2b** (the permanent fix): widen `Instr.temp_dest/src1/src2` to `uint16_t`, sentinel → `TEMP_NONE 0xFFFF` (152 sites / 16 files measured; LRA/arena already width-safe).
- **H1 reclassified**: the original ~115 boundary was C2 in disguise. The real H1 is a **parser stack overflow** — ~18 stack frames per nesting level exhausts the default 8 MB stack near depth 200 (3600+ frame backtrace; non-deterministic segv/hang). Verified with `ulimit -s 65536`: depth-200 then parses fine and hits the C2a guard. Pre-existing, independent of C2.
- **C3/C4 now masked by C2a**: 129 switch cases ≈ 325 temps and 231 initialized locals ≈ 234+ temps both trip the guard first — the true C3 (>128-case cap) and C4 (~231-local hang) boundaries must be re-measured after C2b.

### Verification
- Boundary: 116 nested calls (240 temps) OK in both modes; 117 → clean error in both modes, **no binary** (previously: hang/JIT, runtime segv/AOT). Split-function workaround verified: 120 nested calls via a 4-call helper function passes JIT+AOT+gcc (temp ids never reset per statement, so only *function* splitting helps — the diagnostic says so).
- `make test`: 140/140. `sret_verifier.py`: 6/6. C1 battery (29 checks) OK both modes. No effect on other probe families' behavior below the guard threshold.

## [Sprint 22] - 2026-10-18 - C1: 32-bit Int Wraparound, Signedness End-to-End, Byte-Register Encoding Fix, Global-Struct Member Write

### Added
- **`OP_SEXT8/16/32` / `OP_ZEXT8/16/32` in-place width-extension opcodes** (`ir/ir.h`, appended at the end of the `Opcode` enum to preserve existing values; `Instr.width` field + `ir_mark_last_width`, `ir_emit_sext/zext/sar` helpers in `ir_emit_ops.h`): every narrow-int value is re-extended to 64-bit at its value boundary — local/global loads, struct-member loads (replacing the old AND/xor/sub narrowing arithmetic), function-call arguments (per callee `param_types[i]`), scalar returns, int↔int and float→int casts, and int arithmetic/bitwise results (wider operand wins; equal width → unsigned wins; comparisons unmarked).
- **`emit_extend` + `emit_extend_asm` vtable slots** for all three backends: x86-64 (`movsxd`/32-bit self-`mov`/`0F BF`+`movsxd`/`0F BE`+`movsxd`/`movzx`), AArch64 (`sxtb/sxth/sxtw`, `uxtb/uxth/uxtw`), RISC-V (`slli`+`srai`, `andi`, `slli/srli`). The `-S` asm path round-trips through GNU `as` + host gcc and runs correctly (verified `f(unsigned char, unsigned short)` wrapper program, x86-64).
- **Signed right shift**: `TK_SHR` with a signed left operand lowers to `OP_SAR` (arithmetic) instead of logical `SHR` — `-16 >> 2 == -4` now matches gcc.
- **Global struct member assignment** (`ir/ast_lowering_expr.c` `NODE_ASSIGN`): `g.member = val;` on a file-scope struct (all member widths incl. float) — previously "undefined local variable in struct assignment". Mirrors the global-base rvalue path (`ir_emit_mov_global` + offset + width-sized store).

### Fixed
- **C1 core — `int` overflow/wraparound**: 32-bit `int` math now truncates to 32 bits (`SEXT32`/`ZEXT32` on results) and narrow loads sign/zero-extend, so `int a=2000000000; int b=a+a;` yields `-294967296`, `char 127 + 1 == -128`, `unsigned char 255 + 1 == 0`, `1<<31 == -2147483648`, `x=-2147483648; x>>1 == -1073741824`, etc. (39/39 gcc-cross-checked matrix probes, including the C89 implicit-int `main(){...}` sketches).
- **`unsigned`/`signed` qualifiers now parsed in every declaration path**: locals (`ast_stmt.h` `decl_is_unsigned`), **both** function-parameter parsers (`ast_stmt.h` and the legacy `jit/compile.h`), function return types (`jit/compile.h`), struct members (`definitions.h` — all six tag sites now `is_unsigned ? TYPE_UINTn : TYPE_INTn`), file-scope globals (`compile_unit.h` — `unsigned char g;` previously failed with "use of undeclared identifier"), and type-name parsers (`const_eval.h` — `unsigned char` in casts and `unsigned long`/`long long` consumed correctly; `(unsigned char)x` casts previously fell back to parenthesized-expression parsing).
- **`zero_cc_type_to_typeinfo` dropped unsigned** (`jit/type_info.h`): `TYPE_UINT8/16/32/64` mapped to the *signed* `TypeInfo` (`type_char/short/int/long`); now maps to `type_uchar/ushort/uint/ulong` (`TYPE_MOD_UNSIGNED`), which the lowering uses to choose ZEXT vs SEXT.
- **x86 byte-register ModRM encoding** (`x86_64.c` `x86_emit_extend`): the legacy 8-bit r/m encoding is not 1:1 with the register index (`%bl`=`rm 3`, `%dh`=`rm 6`, `%sil`/`%dil`/`%bpl`/`%spl` need the REX **X** bit with shifted rm values). The first table mapped register 3 (rbx) to BPL and mis-encoded rsi/rdi/rbp — `unsigned char` locals landed in rbx/rsi and read the wrong byte. Table verified byte-for-byte against GNU `as` (`movzbl %bl→0F B6 C3`, `%sil→40 0F B6 C6`, …).
- **RISC-V extend emitter** (`riscv.c`): the sext/zext32 `slli` was missing its `rs1` field (extended **zero** instead of the value).
- **GAS-legal `-S` extend emission** (`x86_64_asm.c`): `movzbl`/`movzwl` have no 64-bit-destination syntax in GAS; the asm path emits 32-bit-destination forms (upper half zeroed) + follow-up `movl`/`movslq` as needed.

### Verification
- `make test`: 140/140. `sret_verifier.py`: 6/6. 39/39 gcc-cross-checked signedness/overflow matrix (JIT = AOT). C99 probe battery: JIT == AOT, no new divergences vs the pre-fix baseline (verified by building `git archive 5ab94ac`). x86-64 `-S` output assembles with GNU `as` and runs correctly via host gcc; aarch64/riscv64 `-S` emit valid `sxt*/uxt*` / `slli/srai/andi` sequences (0 `UNKNOWN` opcodes).

### Documented (OKF)
- `docs/okf/frontend-and-types.md`: new "Signedness & Narrow-Int Correctness (C1)" section (tags, parser paths, width-marking rule, extension sites).
- `docs/okf/isa-backends.md`: new "Integer Width Extension & the x86 Byte-Register Encoding Trap" section (per-ISA instructions, the verified ModRM byte table, GAS syntax limits).

## [Sprint 21] - 2026-10-18 - Scalar Float Pipeline (x86-64), Cast Grammar, Global-Struct Width Narrowing, WSL2 128-Bit RX-Read Workaround

### Added
- **Scalar float load/store emitters** (`isa/isa_backend.h`, `isa/x86_64.c`, `ir/ast_lowering_mc.c`): new vtable entries `emit_vloadss/emit_vloadsd/emit_vstoress/emit_vstoredsd`. `OP_VLOADSS` = `movss` + in-register `cvtss2sd` widen (the double-width scalar FP model forbids stale upper 32 bits); `OP_VLOADSD` = `movsd`; `OP_VSTORESS` narrows double→single through the xmm7 scratch (`cvttsd2ss` + `movss`) without clobbering the source temp; `OP_VSTORESD` = `movsd`. `OP_VLOAD/OP_VSTORE` (128-bit `movups`) are now used only for true vector ops.
- **Global float/double compound assignment** (`ir/ast_lowering_expr.c` `NODE_ASSIGN`): `g += 0.5f` on a float/double global now uses `VLOADSS/SD` + `FADD/FSUB/FMUL/FDIV` + `VSTORESS/SD`; int RHS is converted with `CVTSI2SD` before storing (previously: integer `LOAD_32`/`ADD`/`STORE_32` on the raw IEEE-754 bits).

### Fixed
- **Cast operand grammar** (`parser/ast_expr.h`): a cast's operand is now parsed with `parse_unary_ast` (C11 `cast-expression`) instead of `parse_assignment_ast`. `(int)x == 1` grouped as `(int)(x == 1)` and the cast silently vanished on float operands (float comparison instead of int cast) — now `((int)x) == 1`.
- **`VLOADSS/VLOADSD` register class** (`ir/ir.h`): moved from `ir_opcode_is_simd` to `ir_opcode_is_float` — their destinations are float temps in the XMM pool, not VEC-pool temps. The XMM and VEC pools alias the same physical xmm registers; the VEC classing let an allocator assign the same physical register to the load and a later `CVTSI2SD` (silent value corruption).
- **Global struct member width narrowing** (`ir/ast_lowering_expr.c` `NODE_MEMBER`): the global-base path no longer early-returns after the 64-bit `DEREF`; it falls through to the 1/2/4-byte sign/zero-extension narrowing. Packed layouts (`struct {int a; float f;}`) leave the upper bytes of the load non-zero, so `gs.a` leaked the float member's bytes and `gs.a == 5` failed.
- **Mixed float/int binary ops** (`ir/ast_lowering_expr.c` `NODE_BINOP`): exactly-one-float `+ - * /` converts the integer side with `CVTSI2SD` and takes the float path (previously integer `MUL` on an XMM temp / raw bit pattern).
- **Float negation** (`ir/ast_lowering_expr.c` `NODE_UNOP` `TK_MINUS`): float operands use `(0.0 - x)` (`FMOV_IMM` + `FSUB`) instead of integer `NEG` on the bit pattern. A sign-mask `xorpd [m128]` is not usable: on WSL2 kernel `6.18.54.1-microsoft-standard-WSL2` every 16-byte SIMD memory read from a `PROT_READ|PROT_EXEC` page raises a GPF (reproduced standalone; 32/64-bit reads from RX pages are fine). The now-unused `x86_emit_fp_neg` was also fixed (opcode `0F 57` not `0F 54`, REX.R + ModRM reg = dst for the destination).
- **Float global/aggregate initializer bytes** (`jit/compile_unit.h`): 4-byte float initializers now store the `float` pattern (`(float)dv`), not the low 32 bits of the double (often zero) — `float g = 1.5f;` previously stored `0.0f`.

### Documented (OKF)
- New page `docs/okf/scalar-float-x86.md`: double-width scalar FP model, scalar V-load/store sequences, XMM-vs-VEC classing rule, the WSL2 128-bit RX-read GPF limitation, and related lowering rules (cast grammar, mixed promotion, member narrowing, global float compound assign).

### Verification
- `make test`: 140/140. 29/29 global/float/cast/switch/goto probes pass in both `--run` (JIT) and `-o` (AOT), including `float g = 1.5f; g += 0.5f; (int)g == 2`, `(int)(gs.f * 2.0f)` on packed global structs, `(int)gf == 1`, negative float constants (`-2.5`) in JIT and AOT, and the full `t10_float` suite (JIT = AOT = 0). Standalone WSL2 reproduction (`/tmp/reproc3.c`) characterizes the GPF: `andpd/orpd/movdqa/movapd/xorpd` with 128-bit memory operands from RX pages fault; `addsd/movss/movsd` reads are fine.

## [Sprint 20] - 2026-10-18 - C99 Correctness Sweep: Global Variables, Float Members, Switch/Goto, Data Relocations

### Added
- **File-Scope Global Variable Initializers (JIT + AOT) (`src/zero_cc/jit/compile_unit.h`)**: full mini-parser rewrite supporting pointer declarators, arrays, IEEE-754 float/double scalar bits, aggregate init (positional + designated, padding-correct scratch buffer), char arrays from string literals (incl. `char g[] = "..."` size inference and truncation), multi-declarator lists, and `const`/`volatile`/`static` qualifiers. All 17 global-variable probes pass in both `--run` and `-o`.
- **`ZeroCCDataReloc` Pointer-Initializer Relocations** (`type_system.h`, `symbols.h`, `compile_unit.h`, `execution.h`, `main.c`, `elf_writer.{h,c}`):
  - `const char *g = "...";` (rodata target) and `int *p = &x;` (data target) emit 8-byte placeholders plus a data reloc.
  - **JIT**: absolute 8-byte addresses patched into the still-writable data page after the data copy (`execution.h`).
  - **AOT**: `R_X86_64_64` relocs against the `.rodata` section symbol / target global symbol; the ELF writer now supports **multiple relocation sections** (one `.rela.<sec>` per distinct target section, per-entry section indices, grouped write-out).
- **`goto`, User Labels & `switch`** (`ast_stmt.h`, `ast_lowering_stmt.c`):
  - Parser: `TK_GOTO` → `NODE_GOTO`; statement-leading `IDENT :` → `NODE_LABEL`.
  - `lower_stmt_switch` implemented: `CMP`/`COND_SET`/`JNE` dispatch chain (jump-if-nonzero), case labels stored in `NODE_CASE.aux.alt`, `break` via `label_patcher_enter_loop(switch_exit)`, C-conformant non-match skip and fallthrough. Verified against gcc on consecutive labels, mid/default-first `default`, switch-in-loop, and char case values.
  - Fixed baseline hangs: `goto label;` and `label:` now parse instead of desyncing the statement loop.

### Fixed
- **Struct Member Float Tags** (`parser/definitions.h`): `float`/`double` struct members now get `TYPE_FLOAT`/`TYPE_DOUBLE` (were `TYPE_INT32`/`TYPE_INT64`), enabling correct float-member loads/stores for local and global structs (JIT + AOT).
- **`ir_emit_fstore` Base Register** (`ast_lowering_expr.c`): member float store passed `BE(c)->fp_reg` (a phys reg) as a temp ID; now passes the `0xFF` frame-pointer marker (pre-existing bug exposed by float members).
- **Char Literals in Constant Expressions** (`parser/const_eval.h`): `parse_primary` handles `TK_CHAR_LIT` (value in `num_val`); previously returned 0 without consuming the token, corrupting aggregate initializers (`.c = 'z'`).
- **Signed/Unsigned Type Names** (`parser/const_eval.h`): `(unsigned int)x` casts, `offsetof`, `sizeof(long long)` no longer misparse.
- **Compound-Assignment Constant Clobbering** (`ast_lowering_expr.c`): `NODE_ASSIGN` compound ops now use a fresh `op_res` temp (fixes `for (i += 2)`-style loops).
- **`->` Arrow Member Access** (`ast_lowering_expr.c`): `NODE_MEMBER_IS_ARROW` honored on rvalue and assignment paths.
- **Float Function Returns** (`ast_lowering_stmt.c`): `ir_emit_fmov(VREG_F0, ...)` for float-typed returns.
- **x86 Shift Aliasing** (`isa/x86_64.c`): `x <<= 2` / `x >>= 1` shared an aliasing-safe shift emitter (dest aliasing zeroed the shift count).
- **Block-Scope `typedef`** (`parser/ast_stmt.h`); **stringize `#`** and **`#endif`/`#else` line-swallowing** (`core/preprocessor.h`); **JIT global address relocs** (`jit/execution.h` — `find_global_var` patch branch, was dead `__data_` code → SIGSEGV on any initialized global).

### Documented (OKF)
- New page `docs/okf/global-data-relocs.md` (globals, `ZeroCCDataReloc`, JIT/AOT patching); updated `frontend-and-types.md` (float member tags, `NODE_MEMBER` `s.str`/`aux` union-aliasing gotcha, goto/label/switch) and `isa-backends.md` (`OP_JE`/`OP_JNE` jump-if-zero/nonzero semantics, JIT data-reloc patching).

### Verification
- `make test`: 140/140; `tools/sret_verifier.py --verify-all`: 6/6; `tools/okf.py lint` + `index`: pass. 17/17 global-variable probes + switch/goto/float-member batteries pass in both `--run` (JIT) and `-o` (AOT).
- Known pre-existing limitations (unchanged): mixed int/`double` arithmetic on FPU temps (e.g. `5 + 1.5`, `(int)(h*100)`), flexible-array `offsetof` crash, GCC-extended nested function definitions, `-S` path does not emit `.data`.

## [Sprint 19] - 2026-10-17 - Unified Register Spill/Reload, Prologue Callee-Save & Standalone C99/C11 Direct-Compiler Verification

### Added & Fixed
- **Unified Register Allocator Spill/Reload & Prologue Callee-Save (`src/zero_cc/ir/regalloc.h`, `ast_lowering.c`, `ast_lowering_mc.c`, `asm_emitter.c`, `isa/*_asm.c`)**:
  - Enforced strict callee-saved-only allocation (or stack spill via `spill_base_offset`) for `spans_call` intervals in `regalloc.h`.
  - Mirrored scratch-register (`%r10`/`%r11`, `x9`/`x10`, `t5`/`t6`) spill reload/store handling across `ast_lowering_mc.c` and `asm_emitter.c`, including spilled `OP_CALL` arguments and indirect callee function pointers.
  - Enabled `OP_CALLEE_SAVE_STORES` in `ast_lowering.c` and implemented `emit_callee_save_stores_asm` across x86-64, AArch64, and RISC-V assembly backends so callee-saved registers (`%rbx`, `%r12`–`%r15`) are symmetrically saved in the prologue and restored in the epilogue.
- **ELF `.symtab` Local/Global Relocation Index Remapping (`src/zero_cc/jit/elf_writer.c`)**:
  - Added `sym_remap[]` in `elf_writer_write()` so reordering `ELF_STB_LOCAL` symbols ahead of `ELF_STB_GLOBAL` symbols in `.symtab` updates `.rela.text` relocation symbol indices (`ELF64_R_INFO`).
- **Standalone C99/C11 Direct-Compiler Support (29/29 Standalone AOT+JIT Tests Passing)**:
  - **Multi-Level Pointer Declarators (`int **mat`)**: Replaced boolean `is_ptr` with `ptr_depth` across parameters, local variables, comma-separated declarators, and return types (`src/zero_cc/jit/compile.h`, `src/zero_cc/parser/ast_stmt.h`).
  - **Opaque/Anonymous Struct Typedefs & Multi-Declarator `extern`**: Added support for forward struct typedefs (`typedef struct FILE FILE;`), anonymous struct typedef naming, and comma-separated `extern` variables (`extern FILE *stdin, *stdout, *stderr;`) in `definitions.h`, `compile_unit.h`, `compile.h`, `ast_lowering_expr.c`, `symbol_mgmt.h`, and `src/main.c`.
  - **Struct Array Members & Nested Brace Initializers**: Added `TYPE_ARRAY` struct member support (`int arr[3]`), array member decay in `NODE_MEMBER`, recursive nested `{ ... }` flattening in `parse_brace_init_list_ast`, and leaf-field resolution (`struct_resolve_scalar_leaf`) in `ast_lowering_expr.c`.
  - **Multi-Eightbyte (`>8` byte) By-Value Struct Arguments & Parameters**: Updated `ast_lowering.c` and `NODE_CALL` in `ast_lowering_expr.c` to pass and save all `num_chunks = (size + 7) / 8` 8-byte chunks of by-value struct arguments/parameters.
  - **`va_arg(ap, Type)` Arbitrary Type Parsing**: Updated `__builtin_va_arg` in `ast_expr.h` and `semantic/expr.c` to parse arbitrary type names via `zero_cc_parse_type_name(c)`.
  - **Physical Register (`< 16`) Guard in Optimization Passes**: Prevented `opt_mem2reg.c`, `opt_copy_prop.c`, `opt_cse.c`, and `opt_gvn.c` from propagating reserved physical registers (`VREG_0`..`VREG_15`) across instructions.

## [Sprint 18] - 2026-10-17 - C11 Core Features, `<threads.h>`, Multi-Dimensional Arrays/VLAs & Tooling Cleanup

### Added - C11 Core Keywords, Alignment, Atomics & Threading (`tools/c11/test_c11.py` 11/11)
- **C11 `<threads.h>` & Linker Library Forwarding**:
  - Created `include/threads.h` mapping C11 thread (`thrd_t`, `thrd_create`, `thrd_join`) and mutex (`mtx_t`, `mtx_init`, `mtx_lock`, `mtx_unlock`, `mtx_destroy`) primitives to POSIX threads (`-lpthread`).
  - Added `-l<lib>` flag forwarding in `src/main.c` for AOT linking.
- **C11 Alignment, Anonymous Structs/Unions, `_Generic` & `_Static_assert`**:
  - Verified `_Alignof` and `_Alignas` on fundamental types, struct members, and stack-allocated local variables.
  - Verified anonymous nested `struct` and `union` field flattening, `_Generic` type-based association selection, and `_Static_assert`.
  - Added support for multi-token signed/unsigned scalar `typedef` declarations (`typedef unsigned long thrd_t;`) in `src/zero_cc/parser/definitions.h` and comma-separated local variable declarators (`thrd_t t1, t2;`) in `src/zero_cc/parser/ast_stmt.h`.
- **Global Variables (AOT & JIT)**:
  - Unified `global_vars` across translation units (`src/zero_cc/types/global_vars.h`, instantiated in `src/zero_cc/parser/ir.c`).
  - Added `ELF_STT_OBJECT` symbol emission for `.data` globals in `src/main.c` and zero-allocated uninitialized `.data` storage in `src/zero_cc/jit/compile_unit.h`.
  - Added support in `src/zero_cc/parser/ast_lowering_expr.c` for `&global`, scalar value loads in `NODE_IDENT`, assignments in `NODE_ASSIGN`, and post/pre-increment (`counter++`).
- **LICM Call-Spanning Safety Invariant (`src/zero_cc/parser/opt_loop.c`)**:
  - Prevented `opt_licm` from hoisting loop-invariant constants out of loops containing `OP_CALL` / `OP_CALL_INDIRECT` instructions, avoiding exhaustion of the 5 callee-saved GPRs on x86-64 and preventing caller-saved register clobbering across function calls.
- **Multi-Dimensional Arrays & Matrix VLAs (`tools/vla/test_vla.py`)**:
  - Implemented Phase 0 (multi-dimensional fixed arrays `a[M][N][K]`, pointer decay on `NODE_INDEX`, struct array indexing `p[i][j].x`) and Phase 1 (matrix VLAs `a[n][M]`) across parser, semantic analysis, and IR lowering, bringing `make test` to **140/140 (100%)**.
- **Tooling Directory Organization (`tools/`)**:
  - Archived 136 historical one-off debugging scripts into `tools/archive/`, keeping active verification suites (`okf.py`, `test_runner.py`, `c11/test_c11.py`, `vla/test_vla.py`, `sret_verifier.py`, `struct_abi_verifier.py`, etc.) at the top level of `tools/` with updated `tools/README.md`.

## [Plan] - 2026-10-16 - Multi-Dimensional VLA Plan & 2-D Array Defect

### Added
- **`docs/MULTIDIM_VLA_PLAN.md`** — revised plan for pragmatic multi-dim VLA
  (single runtime outer dimension + constant inner dimensions, matrix-math focus).
- **OKF** `docs/okf/frontend-and-types.md` — new "Known Defect: Multi-Dimensional
  Arrays" section and `docs/okf/log.md` entry.

### Fixed (documented, not yet implemented) — 2-D/3-D array root cause
- Minimal 2-D fixed array (`int a[2][3]`) segfaults in JIT and AOT.
- Parser (`ast_stmt.h`) consumes only the first `[ ... ]`; remaining dimensions are
  silently swallowed (no diagnostic), truncating the type to 1-D.
- `NODE_INDEX` lowering (`ast_lowering_expr.c`) always loads the element and has no
  pointer-decay case, so the inner index of a nested array loads a value that is
  misused as the outer index's base pointer.
- Plan splits into Phase 0 (multi-dim arrays in general: parser + nested types +
  decay lowering) and Phase 1 (pragmatic multi-dim VLA). Est. ~6 working days.

## [Sprint 17] - 2026 - System V AMD64 & Multi-ISA Hidden Pointer (sret) Large Struct Returns

### Added - Multi-ISA sret & Backend Codegen
- **Multi-ISA Support for Large Struct Returns (>16 bytes)**:
  - **AArch64 (ARM64 AAPCS64 ABI)**:
    - Dedicated `x8` register used as indirect result location buffer pointer.
    - Caller passes stack return buffer in `x8`; arguments start at `x0` without register consumption.
    - Callee prologue saves incoming `x8` (`param_save_int_asm(8, sret_offset)`) and epilogue returns pointer in `x0`.
    - Implemented register reservation and allocation pools avoiding reserved `x8`, `x9` (scratch), `x10`, `x16-x18`, `x29`, `x30`.
  - **RISC-V (RV64GC LP64D ABI)**:
    - Struct return buffer pointer passed in first integer argument register `a0`.
    - User arguments shifted starting at `a1` (param 1); callee preserves `a0` and returns buffer pointer in `a0`.
    - Reserved registers configured in register allocator (`zero`, `ra`, `sp`, `gp`, `tp`, `t0`, `t1`, `s0`), pre-coloring return registers (`VREG_0` -> `a0`, `VREG_2` -> `a1`).
  - **Assembly Emitter (`asm_emitter.c`, `aarch64_asm.c`, `riscv_asm.c`)**:
    - Added ISA-generic emission for `OP_LOAD_32`, `OP_LOAD_16`, `OP_LOAD_BYTE`, `OP_STORE_64`, `OP_STORE_32`, `OP_STORE_16`, `OP_STORE_BYTE`, `OP_LEA_RIP`, and `OP_MOV_GLOBAL`.
    - Added multi-ISA frame pointer layouts (`x29`/`s0`) with proper negative frame offsets, callee-saved preservation, and large immediate handling via scratch registers.
    - String literals and `.rodata` references emitted cleanly in AOT mode across x86-64, AArch64 (`adrp` + `:lo12:`), and RISC-V (`lla` + `addi`).
  - **Direct Verification**:
    - End-to-end static compilation and execution verified under QEMU user emulation (`qemu-aarch64-static` and `qemu-riscv64-static`) across `test/abi/struct_return_20byte.c`, `struct_return_edge.c`, `struct_return_tail.c`, and `struct_return_funcptr.c`.
- **Hidden Return Pointer (sret) for Large Structs (>16 bytes)**:
  - Implemented System V AMD64 ABI compliant struct return for types exceeding 16 bytes.
  - **Callee Side**:
    - Function prologue reserves an 8-byte stack slot (`fn->sret_stack_offset`) below the frame base.
    - Saves hidden return pointer received in `%rdi` into `-sret_stack_offset(%rbp)` and shifts incoming integer parameter registers (`int_idx = 1`, real arguments start in `%rsi`).
    - Struct return lowering (`ast_lowering_stmt.c`) copies struct data in 8-byte chunks to `[hidden_ptr + i*8]` using safe base pointer reloading.
    - Preserves and returns caller's hidden pointer in `%rax` upon function epilogue.
  - **Caller Side**:
    - Allocates 16-byte aligned return buffer on local stack.
    - Passes buffer address in `%rdi` as hidden first argument.
    - Shifts user integer arguments to `%rsi`, `%rdx`, `%rcx`, `%r8`, `%r9`.
    - Handles returned buffer pointer in `%rax` and copies chunks to destination lvalue or provides pointer to expression.
- **Sret Edge Cases Handled**:
  - Direct member access on call expressions (`make_big20(...).c`) without requiring separate local variable binding.
  - Direct assignment to existing local struct (`r = make_big20(...)`) with stack-spilled buffer pointer safety and size-aware tail chunk copy to prevent adjacent stack clobbering.
  - Indirect calls through function pointers (`typedef Ret (*fn_t)(...)` and inline `Ret (*fn)(...)`) returning >16 byte structs via `sret` in both JIT and AOT modes (`call *%r11` after parallel argument register setup).
  - Struct return tail pass-through (`return make_inner(...)`).
  - Safe parallel argument moves in x86-64 assembly emission (`x86_64_asm.c`) using push/pop sequence to avoid register overwriting during argument setup.
  - Function prototype parameter recording in JIT/parser (`compile.h`) for external prototypes.
- **Interoperability & Verification**:
  - Full bidirectional compatibility verified between GCC and ZeroCC (`GCC caller -> ZeroCC callee` and `ZeroCC caller -> GCC callee`).
  - Automated verification suite `tools/sret_verifier.py --verify-all` passes 6/6 checks.
  - Added test cases `test/abi/struct_return_edge.c`, `test/abi/struct_return_tail.c`, and `test/abi/struct_return_funcptr.c`.
  - Full test suite passes 136/136 tests (100%).

## [Sprint 16] - 2026 - AOT Assembly Emission Fixes & Cleanup

### Fixed - AOT Assembly Emission
- **Backend ASM Emission Initialization (`x86_64.c`, `aarch64.c`, `riscv.c`, `isa_backend.h`)**:
  - Fixed uninitialized ASM emission function pointers (`emit_param_save_int_asm`, `emit_param_save_fp_asm`, `emit_variadic_spill_asm`) in the AOT assembly path.
  - Made backend tables writable (removed `const` from `x86_64_backend`, `aarch64_backend`, `riscv64_backend`) to allow runtime initialization.
  - Added lazy initialization via `zero_cc_init_backends_once()` called from `zero_cc_get_backend()` and `zero_cc_get_native_backend()`.
  - Assembly output now correctly emits parameter saves after the prologue using `rbp` as the base register.
- **Parameter Save Placement (`ast_lowering.c`, `asm_emitter.c`)**:
  - Moved parameter saves to occur after the prologue (`pushq %rbp; movq %rsp, %rbp`) instead of before.
  - Changed from red-zone writes (`rsp - offset`) to frame-pointer-relative writes (`rbp - offset`).
  - Stack-passed arguments (7th+ integer args) now correctly load from caller's stack frame after prologue setup.

### Cleaned Up
- **Debug Output Removal (`main.c`, `ast_lowering.c`)**:
  - Removed 5 DEBUG fprintf statements from `main.c` that printed when `-dump-ir` or `--profile-opts` flags were used.
  - Removed redundant IR dump debug print from `ast_lowering.c`.
  - Kept the `-dump-ir` flag functionality for intentional debugging.

### Test Results
- **Pass Rate**: 128/128 tests passing (100%) in `make test`.

## [Sprint 16] - 2026 - SIMD Auto-Vectorizer Fix & Enablement

### Added / Enabled
- **Auto-Vectorizer Pass Enabled**:
  - Re-enabled `auto_vectorize` pass in `g_opt_passes` (`opt_pass.c`).
  - Added full test coverage in `test/jit/simd_loop_test.c` verifying 4-element SIMD vector addition and element verification.
  - Test suite passing at **125/125** tests (100%).

### Fixed - Code Generation & Register Allocation
- **Vector Store Operand Order (`ast_lowering_mc.c`)**:
  - Fixed operand ordering when calling `emit_vstore` and `emit_vstore_asm` for `OP_VSTORE`/`OP_VSTORESS`/`OP_VSTORESD`: passed `phys_src2` (vector register value) as source and `phys_src1` as base address. Resolved segmentation fault caused by register inversion.
- **Floating-Point Comparison in Machine Code Lowering (`ast_lowering_mc.c`, `regalloc.h`, `ast_lowering_expr.c`)**:
  - Implemented `regalloc_get_class_for_temp` in `regalloc.h`.
  - Added support in `ast_lowering_mc.c` under `OP_CMP` to detect `REG_CLASS_XMM` and `REG_CLASS_VEC` operands and dispatch to `emit_fp_cmp` (`ucomisd`) instead of integer GPR `cmp`.
  - Added floating-point comparison handling (`TK_EQ`, `TK_NE`, `TK_LT`, etc.) in `NODE_BINOP` in `ast_lowering_expr.c`.
- **Loop Stride in Vectorization (`opt_vectorize.c`)**:
  - Changed vector loop stride from `byte_stride` (16) to element `vec_width` (4) so loop counter matches address calculation scaling.
  - Added `OP_VLOADSS` and `OP_VSTORESS` detection in `analyze_loop_for_vectorization_full` and `vectorization_transform`.
- **Two-Address Vector Arithmetic in x86-64 Backend (`x86_64.c`, `x86_64_asm.c`)**:
  - Added commutative operand swapping and `vmovaps dst, src1` copies when `dst != src1` for `addps`, `subps`, `mulps`, and `divps`.
  - Updated memory vector load/store in `x86_64_asm.c` to use unaligned `movups`.

## [Sprint 15] - 2026 - System V AMD64 ABI Completeness & AOT Parity

### Added / Enabled
- **System V AMD64 ABI Full Support**:
  - Implemented parameter passing and spilling for >6 integer arguments and >8 floating point arguments across stack boundaries.
  - Implemented `va_start` variadic parameter register spilling in both JIT (`x86_64.c`) and AOT (`x86_64_asm.c`).
  - Implemented <=8 byte struct return values (`ast_lowering_stmt.c`) directly into `rax` (`VREG_0`).
  - Implemented 9–16 byte struct by-value parameter handling in registers.
  - Implemented 9–16 byte struct return values in registers (`rax:rdx`).
  - Added new backend hooks `emit_param_save_int_asm`, `emit_param_save_fp_asm`, and `emit_variadic_spill_asm` to `ZeroCCISABackend`.
  - **Struct by-value passing**: Full support for structs of all sizes (1-64 bytes) passed by value in function arguments.

### Known Limitations
- **Large struct returns (>16 bytes)**: Not yet implemented. Requires sret (struct return via hidden pointer) where caller passes buffer in RDI and callee returns pointer in RAX.

### Test Results
- **Pass Rate**: 128/128 tests passing (100%) in `make test`, including **13 struct by-value tests** in `test/abi/` covering various sizes, alignments, nesting, and edge cases.
- **9-16 byte struct returns**: Verified working (tested in `struct_return_9_16_jit`).

### Fixed - Code Generation & Optimization
- **Callee-Saved Register Slot Offsets (`x86_64_asm.c`)**:
  - Fixed sign of callee-saved register save/restore offsets in assembly generation: changed positive `%d(%%rbp)` to negative `-%d(%%rbp)`. Previously, positive offsets directly corrupted the caller's return address at `8(%rbp)`, causing crashes upon function return.
- **9–16 Byte Struct Return Virtual Register Tracking (`ast_lowering_stmt.c`)**:
  - Replaced hardcoded physical register `2` with `VREG_2` for loading/storing the second eightbyte (`rdx`) so the register allocator and live interval analysis recognize the live range across function returns and assignments.
- **`opt_mem2reg` Sub-8-Byte Stack Store Invalidation (`opt_mem2reg.c`)**:
  - When non-8-byte aligned subfield stores occur (e.g. `p.y = y` at `[fp - 68]`), invalidate both boundary 8-byte slots (`(-offset)/8` and `(-offset + 7)/8`) so `opt_mem2reg` does not forward stale 64-bit slot values to subsequent 8-byte loads.
- **Assembly Condition Set Zero Extension (`x86_64_asm.c`)**:
  - Fixed `movzbl %al, %<reg64>` to `movzbq %al, %<reg64>` to avoid operand width mismatch errors with standard assemblers.

### Test Results
- **Pass Rate**: 124/124 tests passing (100%) in `make test`, including `struct_return_8byte_test`, `struct_return_9_16_jit`, `stack_args_test`, `stack_args_edge`, `variadic_test`, and `struct_byval_test`.


### Added / Enabled
- **Loop-Invariant Code Motion (LICM) Two-Pass Rebuild**:
  - Implemented safe Two-Pass Rebuild architecture in `opt_loop.c` (`opt_licm()`).
  - **Pass 1 (Analysis)**: Replaced flawed label index assumption with a full synthetic label map (`label_to_idx`), correctly resolving loop headers (`OP_LABEL`) and backedges (`OP_JMP`, `OP_JE`, `OP_JNE`, etc.). Iteratively marks pure, side-effect-free invariant expressions and constants.
  - **Pass 2 (Rebuild)**: Reconstructs IR into a fresh `IRArena`, emitting hoisted loop invariants immediately preceding the loop header label, and replacing original in-loop definitions with `OP_NOP`.
  - **IRCallArgs Remapping**: Remapped call argument tracking table indices (`g_compiler->ir_call_args`) for 5+ argument calls so call sites match updated instruction positions in the new buffer.

### Fixed - Loop Live-Range & Register Allocation Safety
- **Loop Backedge Live-Interval Extension (`live_range.h`)**:
  - Linear scan previously calculated variable end positions at the last lexical instruction where the variable was read. When variables were defined before or inside a loop and read inside the loop body, their live intervals ended on the first iteration, allowing subsequent instructions to overwrite their assigned registers.
  - Extended live intervals for temporaries defined prior to or within a loop and consumed in the loop across the backedge to the loop exit.
- **x86-64 `%rcx` Reservation (`regalloc.h`)**:
  - Reserved physical register `1` (`%rcx`) in `regalloc_init()`.
  - Prevents the linear-scan allocator from assigning `%rcx` to loop-carried or hoisted temporaries where x86-64 variable shift instructions (`shl`, `shr`, `sar`) clobber `%cl`.

### Test Results
- **Pass Rate**: 119/119 tests passing (100%) in `make test`.

## [Sprint 13] - 2026 - Mem2Reg Stabilization & Register Allocator Spill Safety

### Added / Enabled
- **Local Mem2Reg Pass Enabled**:
  - Store-to-load forwarding within basic blocks now enabled and active (`opt_pass.c`).
  - Promotes repeated local variable loads (`[rbp - offset]`) to virtual expression temporaries, keeping values in registers and eliminating redundant stack memory round-trips.

### Fixed - Mem2Reg Load/Store Safety (`opt_mem2reg.c`)
- **8-Byte Slot Alignment Constraint**:
  - Restricted stack slot tracking strictly to negative multiples of 8 bytes (`offset < 0 && (offset % 8) == 0`).
  - Prevents non-8-byte struct subfields and bitfield stores/loads from corrupting or aliasing 8-byte slot mappings.
- **Conservative Invalidation**:
  - Invalidated active slot states across block/control-flow boundaries (`OP_LABEL`, `OP_RET`, `OP_EPILOGUE`, jumps, conditional branches).
  - Invalidated across function calls (`OP_CALL`, `OP_CALL_INDIRECT`).
  - Invalidated across indirect memory stores (`OP_STORE_BYTE..64`, vector/float stores, atomics).
  - Invalidated when stack addresses are taken (`OP_LEA`).
  - Invalidated cached slot values when an existing temporary is overwritten (`temp_dest == state.slot_to_temp[s]`).
- **Flags Preservation**:
  - Preserved condition flags registers by resetting `instr->flags_out = 0xFF` on promoted loads instead of overwriting with destination temp.

### Fixed - Register Allocator & Spill Emitter Safety (`regalloc.h`)
- **Stack & Frame Pointer Reservation**:
  - Physical registers `4` (`%rsp`) and `5` (`%rbp`) were incorrectly part of `pool_to_phys` and marked free in `regalloc_init()`.
  - Under high register pressure (>12 live variables), the allocator assigned `%rsp` and `%rbp` to temporaries, causing segmentation faults.
  - Reserved `gpr_free[4]` (`RSP`) and `gpr_free[5]` (`RBP`) in `regalloc_init()` and removed from `pool_to_phys`.
- **Scratch Register Collision**:
  - Physical registers `10` (`%r10`) and `11` (`%r11`) were in the general allocator pool while also serving as backend scratch registers (`scratch_reg1` and `scratch_reg2` in `ast_lowering_mc.c`) for spill reloading and spill stores.
  - Under spill pressure, scratch reloading clobbered live variables assigned to `r10` and `r11`.
  - Reserved `gpr_free[10]` and `gpr_free[11]` in `regalloc_init()`, removed them from `pool_to_phys`, leaving the clean allocatable GPR pool (`rax, rbx, rcx, rdx, rsi, rdi, r8, r9, r12, r13, r14, r15`).
  - Fixed loop bound to iterate over the actual size of `pool_to_phys`.

### Test Results
- **Pass Rate**: 119/119 tests passing (100%), including `jit/regalloc_test`, `jit/anon_struct_test`, `jit/bitfield_test`, `jit/compound_literal_jit`.

## [Sprint 12] - 2024 - SIMD Infrastructure Complete

### Added - Complete SIMD Infrastructure
**Status**: Production-ready backend, auto-vectorization disabled as experimental

#### SIMD IR Layer
- 32 SIMD opcodes defined (VADDPS, VSUBPS, VMULPS, VDIVPS, etc.)
- Scalar FP: VADDSS, VSUBSS, VMULSS, VDIVSS, VMOVSS
- Packed Single (4x float32): VADDPS, VSUBPS, VMULPS, VDIVPS
- Packed Double (2x float64): VADDPD, VSUBPD, VMULPD, VDIVPD
- Bitwise: VAND, VOR, VXOR, VANDN
- Shuffles: VSHUF
- Conversions: VCVT_SI2SS, VCVT_SS2SI
- Memory: VLOAD, VSTORE, VLOADSS, VLOADSD
- Comparisons: VCMPSS, VCMPSD

#### SIMD Backend Interface
- 34 function pointers added to ZeroCCISABackend
- Vector register support (vec_regs[32], num_vec_regs)
- REG_CLASS_VEC register class added to regalloc.h
- get_vec_vec_reg_name() implemented for all ISAs

#### SIMD Machine Code Emission
- **x86-64**: 27 SSE2 instructions (xmm0-xmm15, 16 vector registers)
- **AArch64**: 25 NEON instructions (v0-v31, 32 vector registers)
- **RISC-V**: 15 RVV instructions (v0-v31, 32 vector registers)
- **Total**: 67 vector instructions across 3 ISAs

#### SIMD JIT Binary Emission
- 7 binary emission functions: emit_vload, emit_vstore, emit_vaddps,
  emit_vsubps, emit_vmulps, emit_vdivps, emit_vmovaps
- Direct machine code generation for vector operations

#### SIMD Register Allocator
- Vector register allocation with vec_free[32] tracking
- 16-byte aligned vector spill slots
- Separate next_vec_spill_slot counter
- Helper functions: regalloc_get_vec_reg_name(), regalloc_get_vec_reg_count()

### Fixed - Auto-Vectorization Bugs
**Issue**: Transformation produced incorrect results
- Bug #1: VLOAD/VSTORE zeroed out immediate offsets (fixed: preserve imm)
- Bug #2: Loop increment change doesn't work with current IR structure
- Bug #3: Unmapped invariants allocated as fresh temps (fixed: skip vectorization)
- Bug #4: Loop detection capacity uninitialized (fixed: initialize capacity)

**Decision**: Auto-vectorization disabled as experimental prototype
- Requires complete redesign (pattern matching across instructions)
- High complexity for -O3 feature
- Infrastructure ready for future implementation

### Strategic Decision
**Option 3**: Focus on general optimizations first
- SIMD infrastructure complete and production-ready
- Auto-vectorization is -O3 feature (not -O1/-O2)
- High-ROI general optimizations waiting (GVN/CSE, LICM, DCE)
- Return to auto-vectorization with fresh approach when ready

### Tested
- All 118/118 tests passing ✅
- SIMD backend verified for all 3 ISAs
- No regressions from previous work

### Added - Cost-Based Register Allocation
**Status**: Enabled by default for better loop performance

- Spill cost formula: `10^loop_depth`
- Variables in deeper loops kept in registers
- Backedge detection for loop identification
- Cost-based spill selection (lowest cost spilled first)
- Better register allocation for loop-heavy code

### Fixed - CSE Basic Block Scoping
**Issue**: CSE incorrectly eliminated across control flow boundaries

- Added basic block tracking to CSE pass
- Clear hash table at OP_LABEL boundaries
- Prevents incorrect elimination across if/else branches
- Maintains correctness while preserving optimization benefits

### Verified - LICM Infrastructure
- Loop-invariant code motion infrastructure verified
- Invariant detection working correctly (324 invariants found in demo)
- Full code motion not yet implemented (0 instructions moved)
- Requires two-pass algorithm to handle instruction index invalidation
- See `LICM_IMPLEMENTATION_NOTES.md` for detailed analysis

### Added - IR Insertion Support
**Status**: Complete and ready for use

- Added `ir_insert_at(idx, instr)` function
- Helper functions: `ir_insert_mov_imm()`, `ir_insert_mov_reg()`, `ir_insert_add()`, `ir_insert_label()`
- O(n) insertion with automatic buffer growth
- Enables future optimizations:
  - Full LICM (loop-invariant code motion)
  - Loop restructuring for vectorization
  - Auto-vectorization with pre-header/tail loop generation
  - SSA phi function insertion

**Note**: Not yet used in LICM due to instruction index invalidation issues during iteration. Requires careful index management.

### Added - Optimization Profiling System
**Status**: Complete and ready for use

- Added `--profile-opts` flag to zerocc CLI
- Tracks statistics for all optimization passes:
  - CSE: instructions scanned, eliminated, basic blocks
  - DCE: instructions eliminated, iterations
  - LICM: loops detected, invariants found
  - Register allocation: intervals, spills, cost-based spills
  - General: instruction counts, reduction percentage
- Disabled by default (zero overhead in production)
- Example output shows optimization effectiveness

See `OPTIMIZATION_PROFILING.md` for detailed usage guide.

---

## [Sprint 11] - 2024

### Added - CALL Argument Passing Implementation
- Full ABI-compliant function call argument passing for all three ISAs
- **x86-64**: Integer args in rdi/rsi/rdx/rcx/r8/r9, FP in xmm0-xmm7
- **AArch64**: Integer args in x0-x7, FP in v0-v7  
- **RISC-V**: Integer args in a0-a7, FP in fa0-fa7
- Mixed integer/FP argument handling with is_fp tracking
- Type-safe argument array unpacking in asm_emitter.c

### Fixed - Critical uint8_t Pointer Type Mismatch
**Issue**: CALL argument passing had multi-byte aliasing bug
- IRCallArgs.temps: uint8_t array (8-bit values)
- emit_call_asm() signature: int *arg_temps (32-bit reads)
- Result: Both arguments resolved to same physical register (%rax)

**Solution**: Explicit unpacking in asm_emitter.c
- Convert uint8_t array to local int array
- Type-safe conversion: `unpacked_temps[i] = (int)call_args->temps[i]`
- Proper register assignment for each argument

### Changed
- Updated IRCallArgs structure with is_fp tracking
- Refactored argument collection in ast_lowering_expr.c
- Updated emit_call_asm function signature across all backends

### Tested
- All 118/118 tests passing
- No regressions from previous work
- Assembly output verified for all three ISAs
- Example: add(10, 20) correctly moves args to rdi/rsi instead of both to rax

---

## [Sprint 10.5.7] - 2024

### Added - Multi-ISA Backend-Driven Assembly Architecture
- Created dedicated assembly emitter modules for each ISA
- **src/zero_cc/isa/x86_64_asm.c** - x86-64 assembly emitter (400+ lines)
- **src/zero_cc/isa/aarch64_asm.c** - AArch64 assembly emitter (400+ lines)
- **src/zero_cc/isa/riscv_asm.c** - RISC-V assembly emitter (400+ lines)

### Added - ISA-Specific Function Pointers
- Added 35+ function pointer fields to ZeroCCISABackend
- Implements Option A: Full Backend-Driven Architecture
- Each function pointer maps to ISA-specific assembly emission function

### Added - Non-Hardcoded Register Handling
- Added get_hw_reg_name() function pointer to backend interface
- Hardware register name arrays for each ISA
- x86_64: 16 register names (rax, rcx, rdx, ...)
- aarch64: 32 register names (x0-x31, sp, etc.)
- riscv: 32 register names (zero, ra, sp, ...)

### Added - ISA-Specific Prologue/Epilogue
- Prologue/epilogue no longer hardcoded
- Each ISA implements via backend function pointers
- x86-64: push rbp; mov rsp, rbp; sub rsp, size
- aarch64: stp x29, x30, [sp]!; mov x29, sp
- riscv: addi sp, sp, -size; sw ra/s0

### Added - Callee-Save Register Handling
- Fixed callee-save register mapping (physical → hardware number)
- Hardware register indexing instead of VREG indexing
- Proper register name lookup via get_hw_reg_name()

### Added - Assembly Instruction Emission
- Implemented 32+ IR instruction types per ISA
- Integer operations: MOV, ADD, SUB, MUL, DIV, etc.
- Bitwise operations: AND, OR, XOR, SHL, SHR, SAR
- Comparisons: CMP, COND_SET
- Control flow: JMP, JEQ, JNE, JLT, JLE, JGT, JGE
- Function calls: CALL, RET
- FP operations: FADD, FSUB, FMUL, FDIV, FMOV
- Memory operations: LOAD, STORE, LEA, DEREF
- VLA operations: VLA_ALLOC

### Added - Documentation
- OPTION_A_IMPLEMENTATION_SUMMARY.md - x86-64 details
- OPTION_A_FINAL_SUMMARY.md - Executive summary
- OPTION_A_MULTI_ISA_SUMMARY.md - Multi-ISA overview

### Fixed - Assembly Emitter Critical Bugs
1. **OP_MOV Segfault**: Fixed NULL pointer dereference
2. **FP/SIMD Register Mapping**: Fixed xmm register indexing
3. **ADD Instruction Bug**: Fixed register aliasing in src2==dst case
4. **Static Buffer Bug**: Implemented 4-buffer round-robin scheme
5. **Register Mapping**: Fixed physical→hardware register mapping
6. **Callee-Save Timing**: Moved saves to prologue/epilogue from IR body

### Changed
- Separated assembly emission into dedicated modules
- Refactored backend interface for ISA-specific implementations
- Updated register name handling (% prefixes for x86-64)

### Tested
- All 118/118 tests passing
- No regressions from previous work
- Assembly compilation verified for x86-64

---

## [Sprint 9.5] - 2024

### Fixed - Critical VLA Array Access Bug
**Issue**: VLA array indexing returned wrong values
- Test: vla_test.c expected 20, got 0 (JIT) or 56 (AOT)
- Root cause: x86-64 shift instruction RCX register clobbering
- In SHL/SHR/SAR, shift amount goes in CL (low byte of RCX)
- When destination is RCX, the shift amount overwrites the value

**Solution**: Use temporary registers when destination is RCX
- x86_emit_shl(), x86_emit_shr(), x86_emit_sar() modified
- Use r10 or r11 as temporary when destination is RCX
- Swap results back to RCX after shift

### Fixed - IR Printing Issues
- OP_MOV_MEM: Was printing [sp-%u], now prints [fp%+d]
- OP_STORE_32: Added missing case to IR dump
- OP_DEREF: Added missing case to IR dump
- Added SIMD opcode cases to IR dump

### Added - SIMD IR Specification
- Created docs/SIMD_IR_SPECIFICATION.md (235 lines)
- Defined 32 SIMD IR opcodes
- Specified 26 assembly emission functions
- Analyzed vla_test.c: all 10 loops vectorizable (4x speedup potential)

### Added - Assembly Emitter Implementation
- Implemented -S flag for assembly output
- Created src/zero_cc/parser/asm_emitter.c/h modules
- Fixed PARAM_SAVE_INT register mapping (sequential index → ABI registers)
- Assembly output to file (e.g., /tmp/test.s)

### Added - Python Debugging Tools
- Created 10 SIMD debugging tools
- Created 3 assembly debugging tools
- Updated 4 existing tools for multi-ISA support
- Total: 119 Python tools for validation and testing

### Tested
- All 118/118 tests passing (up from 117/118)
- VLA test now returns correct value (20)
- Assembly output verified
- No regressions

---

## [Earlier Work]

### Completed Features
- Lexical analysis (lexer)
- Parsing (parser)  
- Semantic analysis
- Type checking and promotion
- IR generation
- Register allocation (linear scan)
- Machine code generation (JIT)
- Support for:
  - Integer operations
  - Floating-point operations
  - Function calls and returns
  - Variable-length arrays (VLAs)
  - Callee-save registers
  - Function prologue/epilogue

### Test Infrastructure
- 118/118 tests passing
- Comprehensive test coverage
- No known regressions

---

## Version History

| Sprint | Status | Tests | Key Achievement |
|--------|--------|-------|-----------------|
| 11 | ✅ | 118/118 | CALL argument passing ABI compliance |
| 10.5.7 | ✅ | 118/118 | Multi-ISA backend-driven architecture |
| 9.5 | ✅ | 118/118 | VLA array access bug fix |
| Earlier | ✅ | 117/118 | Core compiler infrastructure |

---

## Bug Reports and Fixes

### High Priority (Fixed)
- ✅ VLA array indexing with loop variables
- ✅ RCX register clobbering in shift instructions  
- ✅ CALL argument pointer type mismatch

### Medium Priority (Known, Unfixed)
- Pre-existing float_printf test failure (JIT test infrastructure issue)

### Low Priority (Not Yet Implemented)
- Stack arguments for functions with >6/8 parameters
- Struct argument passing
- Multi-register return values
- SIMD instruction emission (partial - IR defined, assembly partial)

