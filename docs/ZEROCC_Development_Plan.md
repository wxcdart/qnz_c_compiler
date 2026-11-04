# ZeroCC Development Plan

## Project Overview

ZeroCC is a multi-ISA C99/C11 compiler targeting x86-64, AArch64, and RISC-V with both an in-memory `W^X` JIT (`--run`) and AOT assembly/ELF backends (`-S`, `-c`, `-o`).

- **Knowledge Graph Navigator**: [`docs/okf/index.md`](./okf/index.md)
- **Full Changelog**: [`docs/CHANGELOG.md`](./CHANGELOG.md)

---

## Completed Sprints

### Sprint 57: GCC `c-torture` Conformance Gate & High-Leverage Correctness Fixes ✅
- **Achievement**: Established a ratcheting differential conformance gate against the vendored GCC `gcc.c-torture` corpus (`third_party/vendor/c-torture/`, 3,940 tests from `gcc-mirror` commit `442eb35f`) ahead of the Windows PE/COFF + Win64 ABI vtable rework, and fixed five high-leverage compiler bugs uncovered by the initial sweep.
  - **Differential Harness (`tools/torture_run.py`, `make test-torture`)**: Sweeps `execute/` tests across 8 configurations (`-O0..-O3` × `{JIT, AOT}`) against a cached `gcc -O1 -w -lm` reference binary, and `compile/` tests at `-O0`/`-O2`. Pre-skips DejaGnu target/GNU-extension directives (`untyped_assembly`, `label_values`, `indirect_jumps`, `trampolines`, `int128`, `return_address`, `dg-bogus`, `-ffast-math`/`-mfpmath=`, `%p` stdout). Avoids C11 pre-filtering; instead, triages only `REJECT`s with `gcc -std=c11/-c99 -pedantic-errors -fsyntax-only` to separate C89/GNU extensions (`SKIP-C89`) from real C99/C11 gaps (`REJECT`). Ratchets `test/torture/baseline.json` (`PASS: 1714`, `REJECT: 954`, `SKIP-C89: 699`, `MISCOMPILE: 218`, `NO-REF: 190`, `SKIP-TARGET:*: 144`, `TIMEOUT: 10`).
  - **Bug Fixes Uncovered by the Sweep (`execute/` `MISCOMPILE` 252 → 218)**:
    1. **Struct/Union `enum` Bitfields & Progress Guards** (`definitions.h`, `ast_expr.h`, `compile_unit.h`): Parsed `: const_expr` bitfield widths on `struct`/`union`/`enum` member declarations (`enum Lisp_Type type: 3L;` in `compile/20000224-1.c`), generalized bitfield widths to `zero_cc_eval_const_expr(c)`, and added progress/delimiter recovery in struct bodies and brace-initializer loops.
    2. **C99 §5.1.2.2.3 Implicit `return 0;` in `main`** (`ast_lowering.c`): Emitted `ir_emit_mov_imm(VREG_0, 0)` before `epilogue_label` in `main`, fixing ~20 execute tests that fell off the end of `main`.
    3. **Inlined Function Return Integer Width Extension** (`lowering_state.h`, `ast_lowering_expr.c`, `ast_lowering_stmt.c`): Recorded `inlining.return_type` and applied `zero_cc_emit_int_extend` on inlined `return` statements (`execute/pr108064.c`, `execute/pr44828.c`).
    4. **Bitfield Brace-Initializer Packing (Local AST + Global `.data`)** (`type_funcs.h`, `init_common.h`, `ast_expr.h`, `compile_unit.h`, `ast_lowering_expr.c`): Added `ZC_INIT_BF_STAMP(off, sz, boff, bw)` and `zc_object_resolve_scalar_leaf_bf` so bitfields in local compound literals/brace initializers and file-scope `.data` initializers mask and shift into their storage unit (`execute/pr71700.c`).
    5. **C99 Short-Circuit `&&` / `||` Lowering + Optimizer Multi-Def Guards** (`ast_lowering_expr.c`, `opt_loop.c`, `opt_constant_fold.c`): Replaced eager `&&`/`||` evaluation with branching short-circuit evaluation (`execute/20000314-1.c`), and hardened `instr_is_invariant` (`opt_loop.c`) and `opt_fold_constants` (`opt_constant_fold.c`) to require single-definition destination temps (`def_dest_count == 1` / `g_def_count[temp] == 1`).
- **Gates**: `make test-torture` baseline recorded (`PASS: 1714`, `MISCOMPILE: 218`, `REJECT: 954`, `TIMEOUT: 10`), `make test` **156/156**, `make test-opt` **117/117**, `sret` **6/6**, diag baseline OK, style **0 errors**, okf lint PASS. OKF: `docs/okf/c-torture-gate.md`.

### Sprint 58: AArch64 IEEE-754 FP Compare Fix & c-torture Burn-Down ✅
- **Achievement**: Fixed IEEE-754 NaN handling in AArch64 floating-point comparisons and improved the GCC `c-torture` conformance baseline by 3 tests.
  - **AArch64 FP Compare**: Changed `aarch64_emit_fp_cmp` from `fcmp` (quiet compare) to `fcmpe` (signaling compare) and updated condition codes to GCC's IEEE-754 correct ones: LT=`mi`, LE=`ls`, GT=`gt`, GE=`ge` (JIT `aarch64.c`, ASM `aarch64_asm.c`). This fixes NaN handling in all FP comparisons (`lt`/`le`/`gt`/`ge`/`eq`/`ne` return IEEE-correct results for NaN operands).
  - **ASM Emitter FP Flag Tracking**: Modified `asm_emitter.c` to set `c->lowering.cmp_is_fp` during `OP_CMP` for ASM emission path.
  - **`isfinite` Builtin Fix**: Changed implementation from `(x == 0 || 2*x != x) && x == x` to `(x * 0 == 0) && (x == x)` which works correctly with `ucomisd` + `setnp` for both infinity and NaN cases.
  - **Torture Baseline**: Improved from 809 bad to **806 bad** (3 fewer failures) — primarily IEEE cluster tests in `execute/ieee/` now pass on AArch64.
  - **Verification**: All test suites pass (`make test` 156/156, `make test-opt` 117/117, `make test-isa-qemu` 44/44, `sret_verifier` 6/6, `diag_battery` baseline OK, `style_check` 0 errors, `okf.py lint` PASS, `torture_run` gate OK).
  - **Differential Harness (`tools/torture_run.py`, `make test-torture`)**: Sweeps `execute/` tests across 8 configurations (`-O0..-O3` × `{JIT, AOT}`) against a cached `gcc -O1 -w -lm` reference binary, and `compile/` tests at `-O0`/`-O2`. Pre-skips DejaGnu target/GNU-extension directives (`untyped_assembly`, `label_values`, `indirect_jumps`, `trampolines`, `int128`, `return_address`, `dg-bogus`, `-ffast-math`/`-mfpmath=`, `%p` stdout). Avoids C11 pre-filtering; instead, triages only `REJECT`s with `gcc -std=c11/-c99 -pedantic-errors -fsyntax-only` to separate C89/GNU extensions (`SKIP-C89`) from real C99/C11 gaps (`REJECT`). Ratchets `test/torture/baseline.json` (`PASS: 1714`, `REJECT: 954`, `SKIP-C89: 699`, `MISCOMPILE: 218`, `NO-REF: 190`, `SKIP-TARGET:*: 144`, `TIMEOUT: 10`).
  - **Bug Fixes Uncovered by the Sweep (`execute/` `MISCOMPILE` 252 → 218)**:
    1. **Struct/Union `enum` Bitfields & Progress Guards** (`definitions.h`, `ast_expr.h`, `compile_unit.h`): Parsed `: const_expr` bitfield widths on `struct`/`union`/`enum` member declarations (`enum Lisp_Type type: 3L;` in `compile/20000224-1.c`), generalized bitfield widths to `zero_cc_eval_const_expr(c)`, and added progress/delimiter recovery in struct bodies and brace-initializer loops.
    2. **C99 §5.1.2.2.3 Implicit `return 0;` in `main`** (`ast_lowering.c`): Emitted `ir_emit_mov_imm(VREG_0, 0)` before `epilogue_label` in `main`, fixing ~20 execute tests that fell off the end of `main`.
    3. **Inlined Function Return Integer Width Extension** (`lowering_state.h`, `ast_lowering_expr.c`, `ast_lowering_stmt.c`): Recorded `inlining.return_type` and applied `zero_cc_emit_int_extend` on inlined `return` statements (`execute/pr108064.c`, `execute/pr44828.c`).
    4. **Bitfield Brace-Initializer Packing (Local AST + Global `.data`)** (`type_funcs.h`, `init_common.h`, `ast_expr.h`, `compile_unit.h`, `ast_lowering_expr.c`): Added `ZC_INIT_BF_STAMP(off, sz, boff, bw)` and `zc_object_resolve_scalar_leaf_bf` so bitfields in local compound literals/brace initializers and file-scope `.data` initializers mask and shift into their storage unit (`execute/pr71700.c`).
    5. **C99 Short-Circuit `&&` / `||` Lowering + Optimizer Multi-Def Guards** (`ast_lowering_expr.c`, `opt_loop.c`, `opt_constant_fold.c`): Replaced eager `&&`/`||` evaluation with branching short-circuit evaluation (`execute/20000314-1.c`), and hardened `instr_is_invariant` (`opt_loop.c`) and `opt_fold_constants` (`opt_constant_fold.c`) to require single-definition destination temps (`def_dest_count == 1` / `g_def_count[temp] == 1`).
- **Gates**: `make test-torture` baseline recorded (`PASS: 1714`, `MISCOMPILE: 218`, `REJECT: 954`, `TIMEOUT: 10`), `make test` **156/156**, `make test-opt` **117/117**, `sret` **6/6**, diag baseline OK, style **0 errors**, okf lint PASS. OKF: `docs/okf/c-torture-gate.md`.

### Sprint 56: General Expression Indirect Calls, Non-Ident Struct-by-Value Marshaling & Virtual C11 Headers ✅
- **Achievement**: Closed the remaining C11 calling-convention and standard-header gaps — function-pointer calls through general expressions, by-value struct arguments larger than one register from non-identifier expressions, and the three C11 standard headers missing from the virtual-header set.
  - **General Expression Indirect Calls** (`ast_lowering_expr.c`): `NODE_CALL` lowering no longer requires an identifier callee; `a[i]()`, `p->cb()`, `(*fp)()` lower to the same indirect-call machinery as named function pointers (callee expression evaluated, spilled, reloaded as the `is_fp = 2` argument, `OP_CALL` with `fn_name == NULL`). Return type (sret / 9–16-byte struct return / float result) comes from the semantic `TypeInfo` on the call node. `TK_STAR` on a pointer-to-function returns the pointer value without a memory load (C99 6.5.3.2 function lvalue), making `(*fp)(args)` callable.
  - **Non-Identifier By-Value Struct Args (`> 8`B)** (`ast_lowering_expr.c`): the `NODE_CALL` arg loop passes every 8-byte chunk of a by-value struct/union argument in consecutive GP registers. Chunk source by expression shape: struct locals read from the local's stack slot; compound literals and struct-returning calls contribute chunk 1 from the ABI secondary value register (`%rdx`/`VREG_2` on x86-64, `%x1`/`a1` = `VREG_1` on AArch64/RISC-V64 — matching the struct-return convention); `> 16`B by-value arguments pass the pointer. 9–16-byte compound literals now load chunk 1 into the per-ISA secondary register (was hardcoded `VREG_2`).
  - **Virtual C11 Headers** (`preprocessor.h`): `<stdatomic.h>` (`atomic_*` types as `_Atomic T`, `ATOMIC_VAR_INIT`, `atomic_*` macros over the `__atomic_*` builtins; mirrors `include/stdatomic.h`), `<stdnoreturn.h>` (`noreturn` → native `_Noreturn`; new `include/stdnoreturn.h` reference copy), and `<threads.h>` (`thrd_t`/`mtx_t` over pthread; mirrors `include/threads.h`).
  - **Dead Code Sweep** (`emit_helpers.h`): removed six unused stubs superseded by the IR/machine-emitter pipeline (`zero_cc_emit_prologue/epilogue/mov_imm64/movss_rodata/movsd_rodata/lea_rax_rodata`); the `zero_cc_emit_u8/u16/u32/u64` primitives remain for the ISA backends.
- **Gates**: `test/jit/declarator_stack.c`, `test/jit/struct_byval_test.c`, `test/jit/atomic.c`, `test/jit/noreturn.c` pass cleanly (JIT + AOT); `make test` **156/156**, `make test-opt` **117/117**, `make test-isa-qemu` **44/44**, `make test-linker` **21/21**, `make test-driver` **37/37**, `sret` **6/6**, diag baseline OK, style **0 errors**, okf lint PASS.

### Sprint 55: `zero_rt` Freestanding Runtime Cross-Platform Parity: x86-64, AArch64, RISC-V64 ✅
- **Achievement**: Brought ZeroCC's freestanding runtime `zero_rt` to 100% feature and behavioral parity across x86-64, AArch64, and RISC-V 64-bit, enabling seamless self-hosted linking (`--zerocc-link`) with no host libc dependencies.
  - **Cross-Platform Auto-Linking & Build Targets**: Added `build/lib/zero_rt_aarch64.a` and `build/lib/zero_rt_riscv64.a` to default `all:` build, compiling `zero_rt/` and vendored Ryū with zerocc itself (`--target aarch64`, `--target riscv64`). Auto-links runtime archive matching `--target` or sniffing `e_machine` from input object files; supports override via `$ZERO_RT_A` / `$ZERO_RT_AARCH64_A` / `$ZERO_RT_RISCV64_A`. Predefined target macros (`__aarch64__`, `__riscv`, `__LP64__`).
  - **AArch64 Codegen & Linker Fixes**: Fixed `R_AARCH64_ABS64`/`ABS32` data relocations falling through into instruction-word rewrites that cleared the lower 32-bits of data pointers (`stdout`, `stderr`, `stdin`). Corrected `R_AARCH64_ADR_PREL_PG_HI21` instruction mask (`0x60FFFFE0`). Delegated `param_save_int` and `param_save_fp` stack offset handling directly to store emitters, eliminating invalid `add x9, xzr, x16` sequences.
  - **RISC-V 64-bit Codegen & psABI Fixes**: Added 64-bit immediate materialization in `riscv64_emit_mov_imm` (`slli 32` + low 32-bit `slli/srli` zero-extend + OR). Inverted branch skip (+8) over 20-bit JAL trampolines for branches exceeding 4KB in large functions (`vsnprintf_core`). Handled out-of-range offsets outside `[-2048, 2047]`. Mapped variadic float arguments to GP registers per psABI in `NODE_VA_ARG`.
  - **Stack & Frame Safety**: `OP_SYSCALL` marks functions as non-leaf, preventing x86-64 red zone stack omission (`alloc_frame_size = 0`) from exposing local variables to stack overwrite during raw syscall push/pop instruction sequences.
  - **Runtime Core & Syscall Layer**: Set `RT_SYS_BRK 214` and `RT_HAS_BRK 1` for AArch64 and RISC-V64 (Linux asm-generic syscall numbers). Fixed `(h->size & RT_FREE_BIT) != 0` truncation bug in `heap.c`. Fixed missing `i++` increments in copy loops of `strcpy`, `strcat`, `strncat`.
- **Gates**: multi-ISA test battery passes identically with `RC=0` across x86-64, AArch64, and RISC-V64 under QEMU; `make test` **156/156**, `make test-opt` **117/117**, `make test-isa-qemu` **44/44**, `make test-linker` **21/21**, `sret` **6/6**, diag baseline OK, style **0 errors**, okf lint PASS.

### Sprint 54: Own Linker (T2) Gains RISC-V64: `--zerocc-link` for `--target riscv64` ✅
- **Achievement**: Extended ZeroCC's own static linker to support **RISC-V64** (`--target riscv64`), closing the last 64-bit target gap in the own linker.
  - **`e_machine` plumbing**: accepted `EM_RISCV` (243) in `elf_reader.{h,c}`.
  - **RISC-V64 relocations**: supported `R_RISCV_64` (2), `R_RISCV_32` (1), `R_RISCV_JAL` (17), `R_RISCV_CALL` (18), `R_RISCV_CALL_PLT` (19), `R_RISCV_PCREL_HI20` (23), `R_RISCV_PCREL_LO12_I` (24), and `R_RISCV_PCREL_LO12_S` (25). Two-pass pairing collects all `HI20` deltas in pass 1, resolving `LO12` relocations referencing local marker symbols at `auipc` offsets in pass 2. Accepted `R_RISCV_NONE`, `R_RISCV_ALIGN`, `R_RISCV_RELAX` as safe no-ops.
  - **Per-ISA `_start` stub**: 20-byte stub `auipc t1, %pcrel_hi(main); addi t1, t1, %pcrel_lo(main); jalr ra, t1, 0; li a7, 93; ecall`. Standard static Linux text base `0x10000`; ELF header flags set to `EF_RISCV_FLOAT_ABI_DOUBLE` (4).
  - **Differential gate**: `tools/linker_tests.py` extended to 21/21, running `--target riscv64 --zerocc-link` binaries under `qemu-riscv64-static` and diffing against `musl-gcc-riscv64 -static` (calls, data relocations, archive member extraction).
- **Gates**: `make test-linker` **21/21**, `make test` **156/156**, `make test-opt` **117/117**, `make test-isa-qemu` **44/44**, `sret` 6/6, diag baseline OK, style 0 errors, okf lint PASS.

### Sprint 53: Own Linker (T2) Gains AArch64: `--zerocc-link` for `--target aarch64` ✅
- **Achievement**: Extended ZeroCC's own static linker from x86-64-only to **x86-64 + AArch64**, closing the documented next step and unblocking the zero_rt critical path on a second ISA.
  - **`e_machine` plumbing**: `elf_reader.{h,c}` records `e_machine` and accepts `EM_X86_64` (62) / `EM_AARCH64` (183); requires homogeneous machine architecture across all inputs.
  - **AArch64 relocations**: supported `R_AARCH64_ABS64` (257), `R_AARCH64_ABS32` (256), `R_AARCH64_ADR_PREL_PG_HI21` (275), `R_AARCH64_ADD_ABS_LO12_NC` (277), `R_AARCH64_CALL26` (283) with instruction field patching in place. Derived empirically verified `ADRP` imm21 bitfield layout.
  - **Per-ISA `_start` stub**: 12-byte + nop stub `mov x8,#94; bl <target>; svc #0` (`bl` ensures LR is set). Standard static text base `0x400000`.
  - **Differential gate**: `tools/linker_tests.py` extended to 18/18, running `--target aarch64 --zerocc-link` under `qemu-aarch64-static` vs `musl-gcc-aarch64 -static`.
- **Gates**: `make test-linker` **18/18**, `make test` **156/156**, `make test-opt` **117/117**, `make test-isa-qemu` **44/44**, `make test-driver` **37/37**, `sret` 6/6, diag baseline OK, style 0 errors, okf lint PASS.

### Sprint 52: Build-System Compatibility Flag Pass (`-U`, `-include`, `-isystem`, accept-and-ignore) ✅
- **Achievement**: Stock Makefile/CMake invocations (`-g -Wall -O2 -fPIC ...`) accepted with standard GCC semantics.
  - **Accept-and-ignore**: `-g*` (DWARF), `-W*` (warning controls), `-f*`, `-m*`, `-std=c89..gnu23`, `-pthread`, `-pipe`, `-static`, `-shared`, `-pie`, `-no-pie`, `-x c|assembly|none`. Flagged with reasons on stderr under `-v`.
  - **Optimization aliases**: `-O`→1, `-Os`→2, `-Oz`/`-Ofast`→3.
  - **`-U<macro>`**: command-line undefine, interleaved in command-line order with `-D`.
  - **`-include <file>`**: processed as if `#include "file"` appeared as first line of source, resolving through normal and virtual include machinery.
  - **`-isystem <dir>`**: include search directory flag.
- **Gates**: `make test` **156/156**, `make test-opt` **117/117**, `make test-driver` **37/37**, diag baseline OK, style 0 errors, okf lint PASS.

### Sprint 51: `-E` Preprocess-Only Mode (gcc-Parity Driver Front Door) ✅
- **Achievement**: Added `-E` driver flag to preprocess each input (includes, macros, line markers) to stdout or `-o` file without compilation.
  - Precedence over `-c`/`-S`/`--run`. Multiple inputs with `-o` cleanly diagnosed as error.
  - gcc-parity dependency defaults under `-E`: default `-MMD` target/dep file derived from input name (`main.c` → `main.d`, target `main.o`), not output file.
- **Gates**: `make test` **156/156**, `make test-opt` **117/117**, `make test-driver` **31/31**, diag baseline OK, style 0 errors, okf lint PASS.

### Sprint 50: Toolchain T0: Makefile Dependency Generation (`-MMD` / `-MF` / `-MT`) ✅
- **Achievement**: GNU-Make-compatible dependency generation in the zerocc driver, allowing `make`-based build systems to track header changes.
  - Preprocessor dependency tracking via `zero_cc_cpp_add_dependency()` and `zero_cc_cpp_write_dep_file()`, formatting rules with 78-column line wrapping.
  - Driver flags `-MMD`, `-MF <file>`, `-MT <target>` wired through single-file (`-c`/`-S`) and combined link paths.
  - Clean error handling (missing includes suppress `.d` generation and emit diagnostics).
- **Gates**: `make test` **156/156**, `make test-opt` **117/117**, `make test-driver` **27/27**, diag baseline OK, style 0 errors, okf lint PASS.

### Sprint 49: ABI-Correct Variadic `va_list`, Unsigned `DIV`/`MOD`/`CMP` Codegen, `zero_rt` `printf` Closeout & 256-Stack-Arg Call Fix ✅
- **Achievement**:
  - **Dual-stream `va_list` (`{gp, gp_end, fp, fp_end, ovf}`)**: replaced the single-pointer `va_list` with a 5-pointer struct across `<stdarg.h>`, prologue register-save area (x86-64, AArch64, RISC-V), and `NODE_VA_START`/`NODE_VA_ARG`, supporting `va_list` forwarding across multiple function calls (`printf` → `rt_stream_printf` → `vsnprintf_core`). Expanded `stack_meta_temp`/`stack_meta_fp`/`stack_pos_list` to `IR_CALL_MAX_ARGS` (`256`) so 32–128+ parameter functions (`test/jit/many_params.c`) pass all stack arguments cleanly across `-O0..-O3`.
  - **Unsigned integer `/`, `%`, and `<`/`<=`/`>`/`>=` codegen**: `zero_cc_binop_is_unsigned` (`ast_lowering_expr.c`) marks unsigned `OP_DIV`/`OP_MOD` (`xor %edx,%edx; divq` on x86-64, `udiv` on AArch64, `divu`/`remu` on RISC-V) and emits unsigned condition codes (`6..9` → `setb`/`setbe`/`seta`/`setae` on x86-64, `lo`/`ls`/`hi`/`hs` on AArch64, `sltu` on RISC-V).
  - **`zero_rt` `printf` Phase 0 closeout**: removed duplicate `fmt++` in `vsnprintf_core`, fixed `ryu_parse` scientific exponent extraction and `d2exp_buffered` precision for `%e`/`%g`. `--zerocc-link` `printf` now matches `gcc`/glibc on `%llx`/`%llu`/`%llo` for values $\ge 2^{63}$ and `%f`/`%e`/`%g` for doubles $\ge 10^{15}$.
- **Gates**: `make test` **156/156**, `make test-opt` **117/117**, `make test-isa-qemu` **44/44**, sret **6/6**, diag **16/16**, style **0 errors**, okf lint clean.

### Sprint 48: Dynamic Arena Slices for Function Parameters (C99/C11 127+ Limit) & Caller Dynamic-SP Spill-Offset Fix ✅
- **Achievement**: `ZeroCCFunction` parameters moved from fixed inline arrays to dynamically arena-allocated slices (dedicated `c->fn_arena`); `sizeof(ZeroCCFunction)` shrank ~8.3 KB → 400 bytes (~95% memory savings across `c->functions[]`). Parameter cap expanded to `ZERO_CC_MAX_PARAMS 256` / `IR_CALL_MAX_ARGS 256` — comfortably beyond the C99/C11 127-parameter translation limit (ISO/IEC 9899 §5.2.4.1) — with a clean diagnostic when exceeded. Multi-TU merge deep-copies param slices.
- **Dynamic-SP Spill-Offset Fix**: with `omit_frame_pointer` active, callers that push stack arguments shift `%rsp`; spilled argument temps were reloaded at the *static* frame displacement, reading wrong stack locations on calls with 11+ params. New `zero_cc_frame_disp_at(c, off, extra_sp)` tracks the running SP shift during call/syscall argument marshaling in all four ISAs (JIT + asm paths).
- **Verification**: n=1..130 parameter functions diffed against host GCC in JIT and AOT; `test/jit/many_params.c` (11/16/32/128 params); clean error at 257 params. Gates: `make test` **156/156**, `make test-opt` **117/117**, `make test-isa-qemu` **44/44**, sret 6/6, diag baseline OK, style 0 errors, okf lint clean.

### Sprint 47: FP-Argument Load Ordering in `OP_CALL` (7+ Integer Args) ✅
- **Achievement**: Fixed a pre-existing miscompile where any call with 7+ integer args corrupted every float argument: the integer setup pushes stack args (shifting `%rsp`) *before* the FP-arg loads read their spill slots, so the frame-relative offsets were stale and adjacent bytes landed in `%xmm0..7`. Fix: emit FP-arg loads before the stack-arg pushes in the raw-byte MC path (`ast_lowering_mc.c`), matching the `-S` path's order (the block is shared, so the invariant holds for all ISAs). Regression: `test/jit/varargs_float.c`.
- **Gates**: test 155/155, test-opt 116/116, test-isa-qemu 44/44, diag baseline OK, style 0 errors, okf lint clean. Ordering invariant documented in `docs/okf/abi-sysv-x86_64.md` §1.

### Sprint 46: Static TLS (`_Thread_local`) End-to-End + Function-Declarator/Void-Parameter Fixes ✅
- **Achievement**: C11 `_Thread_local` for file-scope statics in **all four modes** (JIT `--run`, AOT `-c`/`-o` via zerocc's ELF writer, AOT `-S`, gcc-linked objects, dynamic + static) using the x86-64 initial-exec model: new `OP_TLS_LEA` IR op, `movq %fs:0` + `leaq $var@tpoff` with `R_X86_64_TPOFF32`, `.tdata`/`.tbss` packing (type-aligned, zero-padded) in `tls_backend.c`, per-variable TLS symbols + `SHF_TLS` sections in the ELF writer, proper `.tdata`/`.tbss` section emission on the `-S` path. Also: `_Generic` type-compatibility fix (C11 6.5.1.1), C11 `<stdatomic.h>` + bitwise RMW builtins (`__atomic_fetch_or/and/xor`), void-parameter/function-declarator parsing fixes (regression coverage `test/jit/vt1..vt10.c`), file-scope `[[attribute-list]]` skipping (C23).
- **Docs**: new OKF page `docs/okf/tls-static.md`. Gates: test 154/154, test-opt 115/115, test-isa-qemu 44/44, diag baseline OK, style 0 errors, okf lint clean.

### Sprint 45: Unified C99 6.7.8/6.7.9 Aggregate-Initializer Engine ✅
- **Achievement**: Replaced the divergent per-path initializer walkers with one shared C99 6.7.8/6.7.9 engine (`parser/init_common.h`) used by both the AST parser and the file-scope `.data` writer: arbitrary interleaved designator chains (`.a[i].b`), initializer ranges (`.arr[2 ... 5] = { ... }`), elided braces (C99 6.7.9), C99 6.7.9p9 cursor advancement, transparent grouping, and char-array string-fill semantics (C11 6.7.9p13 — fills never exceed the array, NUL only when it fits). Excess aggregate elements warn instead of error (gcc parity). Fixed silent miscompiles: values landing in the wrong field, dropped global designator values, out-of-bounds string fills. `test/jit/designated_init_test.c` extended 7→16 snippet groups, all gcc-diffed.
- **Gates**: test 154/154, test-opt 115/115, test-isa-qemu 44/44, prologue/epilogue + extension verifiers, diag baseline OK, style 0 errors, okf lint clean, C11 11/11.

### Sprint 44: Frame Pointer Elimination on Fixed-Frame Functions (`-fomit-frame-pointer`) ✅
- **Achievement**: Functions with fully static frames no longer set up/tear down the frame pointer. `-fomit-frame-pointer` / `-fno-omit-frame-pointer` flags (default mirrors GCC: omit at `-O1+`, keep at `-O0`). Frame slots are addressed from SP with a constant per-function displacement via shared `zero_cc_frame_base_reg(c)` / `zero_cc_frame_disp(c, off)` accessors across all frame-slot emission sites on all four ISAs; per-ISA FP-less prologues/epilogues (x86 `sub/add rsp`, AArch64 `str x30`/`ldr x30, [sp], #16`, RISC-V `addi sp`); RISC-V keeps FP when `aligned+16 > 2047` (addi immediate range). `tools/prologue_epilogue_verifier.py` extended with omit-FP shape checks + SP-based CSR symmetry.
- **Scope note**: the FP register is only *not set up* — not yet handed to the allocator as a GP register (win is prologue/epilogue cost, not register pressure). Gates: test 153/153, test-opt 114/114, test-isa-qemu 44/44, verifiers green.

### Sprint 43: zero_rt Phase 0, `__zerocc_syscall`, String-Init, Bug-Fix Cluster, GNU Extensions ✅ (multiple entries in CHANGELOG)
- **`__zerocc_syscall` OS-boundary builtin** (`OP_SYSCALL`): SysV syscall register map (`eax`, `rdi/rsi/rdx/r10/r8/r9`), push/pop parallel-move argument setup, call-equivalent clobber/`spans_call` semantics plumbed through live-range/DCE/GVN/mem2reg/LICM; x86-64 only (clean error elsewhere). First OS-boundary primitive for zero_rt.
- **zero_rt Phase 0 — ZeroCC-owned freestanding runtime (Linux x86-64)**: `zero_rt/` (brk-backed first-fit heap, string builtins, atexit/exit via `zeroc_main_wrapper`, unbuffered stdio with vendored Ryū float formatting) **compiled by zerocc itself** (`make all` → `build/lib/zero_rt.a`); `--zerocc-link` auto-links it (`--no-zerocc-rt` opts out); linker `_start` stub routes to `zeroc_main_wrapper`. New OKF page `docs/okf/zero-rt.md`. **Open closeout item**: `printf` output segfaults in some `--zerocc-link` runs (direct `__zerocc_syscall(write, ...)` works — suspected `vsnprintf_core`/static-buffer path in `rt_stream_printf`; see Next Steps §5).
- **String-literal initialization of char arrays (C11 6.7.9p13)**: local/brace/file-scope char-array string init (byte copies, not rodata pointers), unsized-dimension inference, symbol-index invariant for shadowed locals.
- **Pre-existing codegen/semantic bug-fix cluster**: scaled pointer arithmetic (`p++`, `p+n`, `p-q`), non-ident lvalue `++`/`--`, x86 `OP_DIV`/`OP_MOD` dest-aliasing (`8/4 == 0` bug), C block/`for` scope shadowing (latest-wins symbol table), narrow signed/unsigned load extension, auto-vectorizer unit-stride/value-chain hardening (fixed `-O3` miscompiles on `v[2*i]` and int div/mod loops), char-array truncation diagnostic (+ `va_list`-by-value UB fix in diag plumbing).
- **High-ROI GNU/Clang extensions** (`tools/extension_verifier.py` 14/14): `__builtin_offsetof`, case ranges (`case low ... high:`), designated-initializer ranges, statement expressions `({ ... })`; `tools/literal_verifier.py` C-literal spec suite.
- **Gates** (at close): test 149-153/149-153, test-opt 110-114/110-114, asan green, test-linker 15/15, test-driver 21/21, test-isa-qemu 44/44, diag 8/8, style 0 errors.

### Sprint 42: Diagnostics Quality — C99 Constraint Checking, "Did You Mean", & the Diagnostics Battery ✅
- **C99 constraint violations diagnosed** (previously silent miscompiles, JIT + AOT): call argument count vs. prototype (6.7.5.3, incl. the `()`-vs-`(void)` prototype distinction via `TypeInfo.function.is_prototype` / `ZeroCCFunction.has_prototype`; variadic at-least rules), `goto` to undefined label (6.8.4.3, labels pre-collected per function so forward gotos validate), unknown struct/union member (6.5.2.1 — previously defaulted to `int` at offset 0), truncated block (6.8p10 — `expected '}' to end block`). `test/semantic/call_arity_fail.c` + `constraint_violations_fail.c`.
- **"Did you mean" suggestions** for undeclared identifiers (scope-chain candidates) and unknown members (field-table candidates): `diag_levenshtein` (early-exit DP) + `diag_best_match` (distance ≤ 2 and ≤ max(1, len/2), unique-best only). `test/semantic/did_you_mean_test.c`. Side-by-side parity with gcc on the audit cases.
- **Diagnostics battery** (`tools/diag_battery.py` + `test/diag/` corpus, 16 broken-program cases incl. multi-fault files): classifies diagnostics into error categories, differential vs gcc, ratcheting baseline (`test/diag/baseline.json`). Baseline state: category coverage 8/8; multiplicity is the gap (5-fault file diagnosed at 2 errors vs gcc's 5).
- **Multi-error recovery (done, same sprint)**: error budget in `DiagBuf` (default 20); the file-scope loop continues until the budget is spent so one pass reports all faults; `15_multi_error_5.c` reports all five gcc faults; the arity double-report was deduped. The recovery work also exposed and fixed a latent cross-TU ABI bug: `SemContext` was defined in three headers behind a first-include-wins guard (the stale 8-field copy overflowed the caller's stack) — now a single canonical definition in `visitor.h`. See §11 for the full record.
- Also landed this sprint: preprocessor main-file-dir quoted-include resolution (C99 6.10.2p2) + missing-include diagnostics; 262 non-canonical `#include`s normalized to the include-root convention; `docs/coding_style.md` + `tools/style_check.py` (ruff-style C convention checker, 0 errors baseline).
- AST-walker invariant documented: `aux.step`/`aux.alt` only exist on `NODE_FOR`/`NODE_IF`/`NODE_TERNARY` — on other tags they alias the payload union (a 4-char name reads as `aux.step == 4`). Walkers must be tag-aware.

### Sprints 43–96 Summary (2026-10-23 → 2026-11-04) ✅
This section summarizes all sprints after Sprint 42, which drove ZeroCC from a ~80% c-torture pass rate to **0 MISCOMPILE / 0 REJECT / 0 TIMEOUT** (3,266 PASS, 11 SKIP-TARGET:trampolines) and completed the GNU nested-function/trampoline feature set.

- **Sprint 43**: zero_rt Phase 0 (freestanding runtime: brk heap, strings, atexit/exit, stdio with Ryū float formatting) compiled by zerocc itself across x86-64; `__zerocc_syscall` OS-boundary builtin; string-init of char arrays (C11 6.7.9p13); bug-fix cluster (pointer arithmetic, non-ident lvalue `++`/`--`, x86 `DIV`/`MOD` dest aliasing, block scope shadowing, narrow load extension, auto-vectorizer unit-stride/value-chain hardening). 14/14 GNU extensions verified.
- **Sprint 44**: Frame Pointer Elimination on fixed-frame functions (`-fomit-frame-pointer`); selective callee-saved spilling; red zone; inline epilogues; frameless leaf functions with param promotion.
- **Sprint 45**: Unified C99 6.7.8/6.7.9 aggregate-initializer engine (shared between AST parser and file-scope `.data` writer): designated initializers, ranges, elided braces, string fills.
- **Sprint 46**: Static TLS (`_Thread_local`) end-to-end on x86-64 (initial-exec idiom, `.tdata`/`.tbss`, JIT + AOT + `-S`); `include/threads.h` mapped to pthreads; `include/stdatomic.h` & `<stdnoreturn.h>` virtual headers; `_Generic` type-compatibility fix.
- **Sprint 47**: FP call-result liveness — materialized returns via `OP_FMOV`, XMM spans_call forced spill, class-aware FP spill scratch registers, XMM↔VEC pool cross-marking.
- **Sprint 48**: Dynamic arena slices for function parameters (256-param C99/C11 limit); caller dynamic-SP spill-offset fix.
- **Sprint 49**: Dual-stream `va_list` (gp/gp_end/fp/fp_end/ovf); unsigned `DIV`/`MOD`/`CMP` codegen; `zero_rt` `printf` closeout.
- **Sprint 50**: Toolchain T0 `-MMD`/`-MF`/`-MT` dependency generation.
- **Sprint 51**: `-E` preprocess-only mode.
- **Sprint 52**: Build-system compatibility flags (`-U`, `-include`, `-isystem`, accept-and-ignore).
- **Sprint 53**: Own linker gains AArch64 (`EM_AARCH64`, `R_AARCH64_*` relocations, `_start` stub).
- **Sprint 54**: Own linker gains RISC-V64 (`EM_RISCV`, `R_RISCV_*` relocations, `_start` stub).
- **Sprint 55**: zero_rt cross-platform parity (x86-64, AArch64, RISC-V64) with auto-linking under `--zerocc-link`.
- **Sprint 56**: General expression indirect calls (`a[i]()`, `p->cb()`, `(*fp)()`); non-ident struct-by-value marshaling (>8B chunks from `VREG_2`); virtual C11 headers (`<stdatomic.h>`, `<stdnoreturn.h>`, `<threads.h>`).
- **Sprint 57**: c-torture conformance gate established (`tools/torture_run.py`, `test/torture/baseline.json`): PASS 1714, MISCOMPILE 218, REJECT 954, SKIP-C89 699, TIMEOUT 10. Five high-leverage bug fixes (mode attribute, nested designators, text-reloc overflow, VREG_2 protection).
- **Sprint 58**: AArch64 IEEE-754 FP compare fix (`fcmpe` vs `fcmp`); `isfinite` builtin fix; torture baseline improved to 806 bad.
- **Sprint 59–86**: Continuous bug-fix and conformance burn-down: c-torture PASS grew from ~2,000 to 3,162; MISCOMPILE → 0; REJECT → 0; TIMEOUT → 0. Key areas: bitfield brace-initializer packing, short-circuit `&&`/`||`, hex-literal classification (`0x123456789ABCDEFLL`), `-S` opcode gaps, `__builtin_return_address`/`__builtin_frame_address`, empty structs, GNU labels-as-values/computed goto (`OP_LEA_LABEL`/`OP_JMP_INDIRECT`), multi-ISA `OP_SETJMP`/`OP_LONGJMP`, GNU integer complex (`_Complex int`), 16-bit temp-id calibration, `semantics` type-identity invariants, `typeof`/`typeof_unqual`, empty `{}` initializers, `__STDC_NO_COMPLEX__`, `[[attr]]` recognition.
- **Sprint 87–89**: `label_values` unskipped; inline asm triage; static label difference relocs; `OP_SYSCALL` cross-ISA (52/52 QEMU); 221/221 backend vtable parity across all 4 ISAs; c-torture 0 REJECT / 0 MISCOMPILE (3,234 PASS).
- **Sprint 90–93**: GNU nested functions / trampolines (C99 6.9p15): Phase 1 (JIT-only, no capture), Phase 2 (closure capture by-value hidden leading params), Phase 3a (by-reference capture via mirror locals), Phase 3a-2 (runtime VLA param dims + `sizeof`), Phase 3b (real trampoline stubs + pointer-routed capture). 8 trampoline tests unskipped; PASS grew to 3,266.
- **Sprint 94–96**: C11 `_Alignas`/`_Alignof` correctness (file-scope any N, local cap 16 + warning, `sizeof` unchanged); `_Alignas(N>16)` locals via GCC-style runtime SP realign prologue (frame_align mode, `$-N,%rsp`, leave epilogue) on x86-64, AArch64, RISC-V 64/32; `__STDC_NO_COMPLEX__` removal.

### Sprint 36: mem2reg Loop-Promotion Cost — O(loops × 256 × |IR|) → O(loops × |IR|) ✅
- **Status**: committed (`5b240e8`). Phase 1 of the agreed order **2 → 3 → 4 → 1** (mem2reg perf → static-local fix → CFG-enabled opt → T0 `-I`).
- **Problem** (measured in Sprint 35 Phase 4): `opt_promote_loop_carried` rescanned the whole function once **per stack slot (up to 256) per loop (up to 64)** — ~85% of all optimizer time (1088 ms @-O1, 3374 ms @-O2, 4536 ms @-O3 on `test/bench/synth_large.c`).
- **Fix** (`opt/opt_mem2reg.c`): one scan of the function per loop accumulates per-slot stats for ALL slots in a single pass (in-loop loads/stores, first-exit-post loads, bad loads, out-of-loop stores); only slots passing the candidate check get the follow-up init-store/forwarding scans. The existing address-taken pre-audit also records `max_slot_used`, bounding the candidate iteration (slots with no load/store traffic are provably non-candidates). Candidate semantics unchanged.
- **Measured** (median of 5 profile runs, `synth_large.c`): mem2reg 1088→3.2 ms @-O1, 3374→13.8 ms @-O2, 4536→15.9 ms @-O3; total passes-only @-O2 3688→~275 ms (~13×). **Correctness: the `-O1` `-dump-ir` output is byte-identical pre/post** (promotion decisions unchanged); `make test-opt` 105/105, `mem2reg_verifier.py` 12/12, `make test` 141/141.
- **Next hotspot**: GVN (~184-210 ms @-O2/-O3, now the dominant pass — per-block hash-table re-init churn); tracked, not fixed here (GVN was just OOB-fixed in Sprint 35 Phase 4; separate risk budget).
- **Measurement note**: wall times on this host swing ±50% under variable load (load avg up to 1.3 on 4 cores); the per-pass profile (in-process `CLOCK_MONOTONIC`) is the stable metric — always report medians of ≥5 runs.

### Sprint 34: Small Function Inlining (`-O2`/`-O3`) & LICM Multi-Def Safety ⚠️ (landed with open risks)
- **Status**: COMMITTED (`0272a2a`). Inlining risks were audited in Sprint 35 Phase 1 (2026-10-20): FP signatures, body locals, labels/goto and `&param` are now rejected by conservative candidate filters (`jit/compile.h`); see the Phase 1 section and `test/inline_audit/` (17 differential tests in the `opt_sweep.py` corpus).
- **Achievement**: AST-level call-site inlining of small `static`/`inline` leaf functions (≤35 AST nodes, non-variadic, non-recursive, no struct return >8B, no calls in body) with TU-level AST retention/cloning. Fixed a pre-existing LICM bug that hoisted instructions whose destination temp had multiple definitions in the loop.
- **Key Deliverables**: `parser/ast_arena.h` (`zero_cc_ast_clone`/`_node_count`/`_has_call`), `jit/compile.h` (candidate detection), `ir/ast_lowering_expr.c` + `ast_lowering_stmt.c` (param substitution, return redirection), `opt/opt_loop.c` (multi-def guard).
- **Measured**: 100M-iteration helper-call loop 380 ms (`-O0`, no inlining) → 143 ms (`-O2` JIT). Single benchmark; not a general claim.
- **Phase 1 (done)**: conservative candidate filters close the audited inlining gaps (FP signatures, body locals, labels/goto, `&param`).
- **Phase 4 (done)**: measurement (`--profile-opts` per-pass before/after), JIT-vs-GCC claim re-measured and retracted (`docs/okf/perf-measurements.md`), stale docs cleaned. Sprint 35 complete — no open phases.
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

