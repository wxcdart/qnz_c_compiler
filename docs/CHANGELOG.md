# ZeroCC Changelog

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

