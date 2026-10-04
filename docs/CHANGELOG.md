# Changelog

## [Unreleased] - Multi-ISA Backend & C11 Support

### Goal: Self-Hosting Compiler

ZeroCC is designed to eventually **self-host** (compile itself). Architecture supports:

- **Modular design**: Separate source files, not single-header
- **Multi-ISA**: x86-64, aarch64, riscv64 backends
- **C11 conformance**: TLS, Atomic, Alignment, Generic selection
- **Cross-platform**: Linux, Windows, macOS, baremetal

### Progress

### Added

#### Sprint 8.5 — Architectural Hardening & Infrastructure Refactoring
- **Unified Prologue & Epilogue Pipeline (Task 60)**:
  - Eliminated the split between `compile.h` and `ast_lowering.h`: early prologue and early parameter register saves in `compile.h` removed.
  - Deferred prologue emission until post-regalloc: `OP_PROLOGUE` is emitted as a placeholder during IR lowering, and its `imm64` (frame size) is patched after register allocation with exact total size (`locals + spills + callee_saves + 16_byte_alignment`).
  - Added IR instructions `OP_PARAM_SAVE_INT`, `OP_PARAM_SAVE_FP`, and `OP_VARIADIC_SPILL` to uniformly represent incoming ABI argument and variadic register spills in IR before instruction selection.
  - Fixed variadic register spill offset calculation so `x86_emit_variadic_spill` uses the exact base offset following fixed parameters without corrupting `current_stack_frame`.
  - Added `-MMD -MP` automatic header dependency generation to the `Makefile` to prevent translation unit header desynchronization.
- **Global State Encapsulation / Reentrancy (Task 61)**:
  - Created `LoweringState` struct to encapsulate all per-function lowering state (`ir_ctx`, `epilogue_label`).
  - Created `LabelPatcherState` struct to encapsulate all label/patching state (`loop_stack`, `user_labels`).
  - Replaced all global variables with encapsulated state in `ZeroCCCompiler`:
    - `g_ir_emitter_ctx` → `c->lowering.ir_ctx`
    - `g_ir_epilogue_label` → `c->lowering.epilogue_label`
    - `g_loop_label_stack` → `c->label_patcher.loop_stack`
    - `g_loop_label_depth` → `c->label_patcher.loop_depth`
    - `g_user_labels` → `c->label_patcher.user_labels`
    - `g_user_label_count` → `c->label_patcher.user_label_count`
  - Enabled reentrant compilation: multiple `ZeroCCCompiler` instances can now compile concurrently without state collision.
  - Removed ~70KB of dead `IREmitterContext` code (unused break/continue patch arrays, label tables, goto patch lists).
- **Unified IR Label & Branch Patching (Task 62)**:
  - Created new `label_patcher.h` with unified API for loop control and label management.
  - Moved loop control state (`loop_stack`, `loop_depth`) from `LoweringState` to `LabelPatcherState`.
  - Moved user label state (`user_labels`, `user_label_count`) from `LoweringState` to `LabelPatcherState`.
  - Updated all AST lowering code to use `label_patcher_*` API:
    - `label_patcher_enter_loop()` / `label_patcher_exit_loop()` for loop scoping
    - `label_patcher_get_break_label()` / `label_patcher_get_continue_label()` for break/continue
    - `label_patcher_find_or_create_user_label()` for goto labels
  - Removed dead code from `IREmitterContext`:
    - `break_patch_pos[16][64]`, `break_patch_count[16]`
    - `continue_patch_pos[16][64]`, `continue_patch_count[16]`
    - `loop_start_pos[16]`, `label_table[256]`, `label_count`
    - `goto_patch_list[256]`, `goto_patch_count[256]`, `goto_patch_list_count`
    - `epilogue_patches[32]`, `epilogue_patch_count`
  - Simplified `ir_emitter.h` to only contain temp variable tracking functions.
  - Net result: **-128 lines** of code (165 removed, 37 added).

#### Sprint 5.8 — Designated Initializers, VLA Validation & Compound Literal Hardening ✅
- **Zero-Initialization of Omitted Elements (C99 §6.7.8)**:
  - Compound literals and brace-enclosed variable initializers now zero-initialize the entire object (struct/array) before applying designated and positional stores.
  - Fixes uninitialized gaps in `int arr[5] = { [2] = 7 }` → `arr[0]`, `arr[1]`, `arr[3]`, `arr[4]` now correctly zero.
  - Implemented in `ast_lowering_expr.h` `NODE_COMPOUND_LIT` lowering via chunked zero stores (8/4/2/1-byte).
- **VLA Initializer Rejection (C99 §6.7.8 p3)**:
  - Variable-length arrays with initializers now emit a constraint violation error: `"variable length array '...' cannot be initialized"`.
  - Implemented in `ast_stmt.h` during declaration parsing.
- **Array Designator Bounds Checking**:
  - Designated array initializers `[idx] = val` now validate `idx < array_len` for fixed-size arrays at parse time.
  - Errors with: `"array initializer index N exceeds array bound M"`.
  - Implemented in `ast_expr.h` `parse_brace_init_list_ast`.
- **Flexible Array Member Initialization Rejection (C99 §6.7.8 p21)**:
  - Initialization of flexible array members (`.flex = ...` or `[idx] = ...` on trailing zero-length array) now rejected at both parse time and lowering time.
  - Errors with: `"initialization of flexible array member '...'"`.
  - Implemented in `ast_expr.h` `parse_brace_init_list_ast` and `ast_lowering_expr.h` `NODE_COMPOUND_LIT` lowering.
- **Parser Synchronization Enhancement**:
  - Added `TK_STATIC_ASSERT` to panic-mode synchronization stop list in `zero_cc_synchronize_ast()`.
  - Prevents cascading parse errors after `_Static_assert`.
- **Code Quality Cleanups**:
  - Replaced magic `0x8000` flag with named constant `NODE_FLAG_DESIGNATED` (bit 15) in `parser.h`.
  - All usages updated in `ast_expr.h`, `ast_lowering_expr.h`, `ast_stmt.h`.

#### Sprint 5.6 — Register Allocation & Calling Convention Hardening ✅
- **Callee-Saved Register Preservation (`rbx`, `r12`–`r15`)**:
  - Added `used_callee_saved_gpr_mask` tracking to `RegAllocator` in `regalloc.h`.
  - Added `CALLEE_SAVE_FRAME_RESERVE` (48 bytes) top-of-frame reservation in `compile.h` to prevent local variable collisions.
  - Implemented `emit_callee_save_stores` and `emit_callee_save_loads` in `ast_lowering.h` using dedicated stack frame slots.
  - Callee-saved registers assigned during linear-scan allocation are preserved across function calls per the AMD64 ABI.
- **OP_CALL Calling Convention Hardening & Multi-Argument Support**:
  - Implemented `IRCallArgs` side table in `ir.h` and `ir.c` allowing up to 16 integer arguments per call site (overcoming packed `Instr` field limitations).
  - Implemented full AMD64 ABI argument passing in `emit_ir_to_machine_code`:
    - Arguments 1–6 pass in `rdi`, `rsi`, `rdx`, `rcx`, `r8`, `r9`.
    - Arguments 7+ are pushed in reverse order to the stack with 16-byte alignment and cleaned up post-call via `emit_stack_adjust`.
  - Updated callee function prologue in `compile.h` to load stack-passed arguments (`[rbp + 16 + 8*k]`) into parameter local slots.
- **Nested Call Clobbering Resolution**:
  - Fixed pre-existing caller-saved register clobbering across nested calls during argument evaluation:
    - Proactively spills evaluated integer argument temporaries to dedicated stack slots immediately upon evaluation.
    - Reloads all arguments into fresh virtual temporaries immediately before `OP_CALL`, guaranteeing no intervening call clobbers arguments.
- **Struct Return Register Protection (9–16 bytes)**:
  - Preserved direct `VREG_0` return for functions returning 9–16 byte structs/unions, ensuring both lower 8 bytes (RAX) and upper 8 bytes (RDX) remain untouched until stored into target local slots.
- **Real Stack Spill Slot Lowering (Phase 3)**:
  - Added `regalloc_get_spill_slot_for_temp` helper in `regalloc.h` to identify spilled virtual temporaries and their assigned slot index.
  - Dynamically reserved spill stack space in `zero_cc_lower_ast` (`c->current_stack_frame += c->regalloc.next_spill_slot * 8`) and computed 8-byte aligned `spill_base_offset`.
  - Implemented automatic operand reload in `emit_ir_to_machine_code`: spilled `temp_src1` reloads into scratch register `r10`, and spilled `temp_src2` reloads into scratch register `r11` (`mov reg, [rbp - (spill_base_offset + (slot + 1) * 8)]`), avoiding register collision on two-address ALU operations.
  - Implemented automatic destination store in `emit_ir_to_machine_code`: spilled `temp_dest` writes through scratch register `r10` and emits `mov [rbp - (spill_base_offset + (slot + 1) * 8)], r10` at instruction completion.
  - Added spilled argument reloading for `OP_CALL` stack and register passing.
  - Added Tests 9 and 10 to `test/jit/regalloc_test.c` asserting correctness under extreme register pressure (18 simultaneously live variables and spilled call-spanning arithmetic).
- **Modularization of `ast_lowering.h`**:
  - Decomposed the 2,300+ line monolithic `ast_lowering.h` into dedicated, modular headers:
    - [`ast_lowering_stmt.h`](file:///home/ubuntu/dev/qnz_c_compiler/src/zero_cc/parser/ast_lowering_stmt.h) (360 lines): Statement AST node lowering (`lower_stmt_*`, block/loop/control flow) and `lower_node` dispatcher.
    - [`ast_lowering_expr.h`](file:///home/ubuntu/dev/qnz_c_compiler/src/zero_cc/parser/ast_lowering_expr.h) (1,385 lines): Expression AST node lowering (`lower_expr`) into IR instructions.
    - [`ast_lowering_mc.h`](file:///home/ubuntu/dev/qnz_c_compiler/src/zero_cc/parser/ast_lowering_mc.h) (452 lines): IR-to-machine-code emission (`emit_ir_to_machine_code`) with operand reloading, register dispatch, and destination stack spilling.
    - [`ast_lowering.h`](file:///home/ubuntu/dev/qnz_c_compiler/src/zero_cc/parser/ast_lowering.h) (178 lines): Clean coordinator header containing state management, callee-saved preservation helpers, and `zero_cc_lower_ast()`.
- **Comprehensive Test Suite**:
  - Added `test/jit/regalloc_test.c` covering callee-save preservation across calls, 10+ live values under high register pressure, recursive calls, 6-register argument calls, stack-passed arguments (7th and 8th), multi-level nested call expressions, and true stack spill/reload lowering.
  - **Test Suite Status**: 112/112 tests passing cleanly under both `make test` and `make asan` with 0 memory errors or leaks.

- **Named `enum` as a Distinct Type-Tag**:
  - Implemented `TYPE_ENUM` tag in the type system with 4-byte size and 4-byte alignment.
  - Added compiler enum registry `enums` with `zero_cc_register_enum` and `zero_cc_find_enum`.
  - Added full support for `enum Tag { ... };` definitions at both top-level and block scope.
  - Supported `enum Tag var;`, `enum Tag *ptr;`, and typedef aliases `typedef enum Tag Alias;`.
  - Added support for `enum Tag` in function parameter declarations and return type signatures.
  - Added support for `enum Tag` members inside struct and union declarations.
  - Supported compile-time `sizeof(enum Tag) == 4` and `_Alignof(enum Tag) == 4`.
  - Folded enum constant identifiers directly to `NODE_INT` in AST expressions and added immediate value lowering in `ast_lowering.h`.
  - Added comprehensive test suite in `test/jit/enum_tag_jit.c`.
- **Compound Literals `(T){...}`**:
  - Implemented `NODE_COMPOUND_LIT` parsing and AST lowering for scalar and aggregate types.
  - Added support for array decay to pointer, direct subscripting `(int[]){1,2}[0]`, address-of `&(int){42}`, and designated struct initializers `(struct S){.x = 10}`.
  - Expanded comprehensive test suite in `test/jit/compound_literal_jit.c`.
- **Comma Operator `,`**:
  - Implemented `NODE_COMMA` with lowest operator precedence and left-associativity.
  - Lowered sequentially in AST IR lowering (evaluates LHS, evaluates RHS, yields RHS).
  - Added comprehensive test suite in `test/jit/comma_jit.c`.
- **Test Suite Status**: 111/111 tests passing cleanly under `make test` and `make asan` with 0 memory errors or leaks.

#### Sprint 5.5 — Task 40: Architecture Cleanup & Legacy Pipeline Elimination ✅
- **Eliminated Legacy Single-Pass Pipeline**: Fully removed the deprecated single-pass expression and statement parsers that performed ad-hoc x86-64 machine code emission (~3,700 lines eliminated).
- **Unified Pipeline Architecture**: ZeroCC now strictly operates on a clean **AST → Semantic Analysis → IR → Backend Emission** path for all statements, expressions, and functions.
- **Pure Utility Extraction (`const_eval.h`)**:
  - Extracted compile-time integer constant evaluation (`zero_cc_eval_const_expr`, recursive descent arithmetic / logic evaluators).
  - Extracted type inference and member lookup (`zero_cc_type_of_*`, `zero_cc_type_find_member`).
  - Extracted type name parsing for casts and operators (`zero_cc_parse_type_name`, `zero_cc_parse_type_name_at_pos`).
  - Extracted compile-time `sizeof` and `_Alignof` folding (`eval_sizeof`, `eval_alignof`).
- **Static Assertion Extraction (`static_assert.h`)**:
  - Extracted `zero_cc_stmt_static_assert` from legacy control flow handlers into a dedicated, non-emitting header.
- **Removed Deprecated Headers & Sources**:
  - Deleted `src/zero_cc/expressions.h` and `src/zero_cc/expressions/{primary,unary,binary,ternary,visitor}.h`.
  - Deleted `src/zero_cc/statements.h`, `src/zero_cc/statements.c`, and `src/zero_cc/statements/{control,variables}.h`.
  - Purged obsolete `#include` references from `include/zero_cc.h` and `src/zero_cc/zero_cc.h`.
- **Build System & Test Suite Cleanup**:
  - Updated `Makefile` to remove `statements.o` and legacy expression header dependencies.
  - Purged 8 scratch/stepping-stone debug test files (`test/flexible_array_debug*.c`) and 39 orphaned test binaries.
  - Clean build with zero warnings under `-Wall -Wextra` across all 108 test suites.

#### Sprint 5.5 — Task 41: Test Suite Restructuring & Harness Modernization ✅
- **Categorized Test Directory Structure**: Migrated flat 108 tests into structured domain-specific categories:
  - `test/unit/` (12 tests): Lexer, preprocessor, hash table, IR opcodes, basic data structures.
  - `test/semantic/` (11 tests): Type checking, scope resolution, declarations, promotions, static assert validation, const/volatile.
  - `test/abi/` (26 tests): Calling conventions, System V AMD64 argument registers, struct pass-by-value, struct returns, SSE float passing, variadics.
  - `test/jit/` (59 tests): End-to-end JIT execution across control flow, VLAs, atomics, bitfields, compound literals, floating-point arithmetic, strings, TLS.
- **Unified Test Runner (`tools/test_runner.py`)**:
  - Millisecond-precision per-test timing and pass/fail tracking.
  - Colorized terminal output with category badges and failure details (stdout/stderr captures).
  - Test Anything Protocol (TAP v13) output mode (`--tap`).
  - Category filtering (`--category <cat>`) and per-test execution support.
- **Enhanced Makefile Targets**:
  - `make test` runs the unified runner across all 108 tests.
  - `make test-unit`, `make test-semantic`, `make test-abi`, `make test-jit` for focused testing.
  - `make test-tap` for automation pipelines.
  - Automatic directory creation (`@mkdir -p $(dir $@)`) for all test binaries.

#### Sprint 5.5 — Task 42: Memory Leak & Sanitizer Audit ✅
- **Complete ASan + UBSan + LSan Cleanliness**: All test suites execute with zero leaks and zero memory errors under AddressSanitizer and UndefinedBehaviorSanitizer.
- **Resource Lifecycle Fixes**:
  - Fixed preprocessor macro expansion loop leaks in `zero_cc_cpp_destroy` and freed preprocessor structures in `zero_cc_preprocess_ext`.
  - Added `regalloc_destroy` to reclaim register interval and clobber structures.
  - Reclaimed per-function AST arenas immediately upon compilation completion.
  - Implemented thread-local type pool (`zero_cc_type_alloc_base` / `zero_cc_type_pool_free`) to prevent base type leaks during recursive type construction.
  - Fixed stack-use-after-return in semantic binary operator promotions for decayed pointer types.
  - Cleaned up string buffer and compiler context allocations across test drivers.
- **Dedicated Sanitizer & Valgrind Targets**:
  - Added `make asan`, `make test-asan`, `make valgrind`, and `make test-valgrind` targets.
  - Verified 100% clean passes under Valgrind 3.22.0 (0 errors, 0 leaks across all test suites).

#### Sprint 5.5 — Task 43: JIT W^X Memory Hardening ✅
- **Strict Write-XOR-Execute Enforcement**:
  - Eliminated dangerous combined RWX memory mappings in JIT execution engine (`execution.h`).
  - Implemented page-aligned memory partitioning between executable code (`.text` + `.rodata`) and mutable data (`.data` / globals).
  - Code pages transition strictly from `PROT_READ | PROT_WRITE` during emission to `PROT_READ | PROT_EXEC` (no write) before execution via `mprotect`.
  - Mutable data pages remain `PROT_READ | PROT_WRITE` (never executable).
- **Cross-Platform Virtual Memory Adapter (`mmap_adapter.h`)**:
  - Dynamically queries system page size (`sysconf(_SC_PAGESIZE)` on POSIX, `GetSystemInfo` on Windows).
  - Clean abstraction for `zero_mmap_alloc_rw`, `zero_mmap_make_rx`, `zero_mmap_make_ro`, and `zero_mmap_free`.
  - Native Windows support using `VirtualAlloc`, `VirtualProtect`, and `VirtualFree`.
- **W^X Invariant Verification Test (`test/jit/jit_wx_test.c`)**:
  - Verifies that writes to RX code pages reliably fault (`SIGSEGV` / `SIGBUS`).
  - Verifies JIT compilation and execution in W^X-hardened memory.
  - 109/109 tests passing cleanly across `make test`, `make asan`, and `make valgrind`.

#### Sprint 5.5 — Task 44: Diagnostic & Header Sanitization ✅
- **Header Layout Unification**:
  - Eliminated duplicate `src/zero_cc/zero_cc.h`.
  - Established `include/zero_cc.h` as the single canonical public entry point for ZeroCC with full architecture documentation.
  - Eliminated redundant `src/zero_cc/statements/` directory; moved `definitions.h` and `static_assert.h` into `src/zero_cc/parser/`.
  - Eliminated orphaned `src/zero_cc/arch/` directory; consolidated `arm64_emitter.h` into `src/zero_cc/isa/`.
  - Moved loose root files into their respective subsystems: `keywords.inc` into `src/zero_cc/lexer/` and `abi_classify.h` into `src/zero_cc/parser/`.
  - Removed duplicate `ZeroCCCompiler` forward declaration in `src/zero_cc/parser.h`.
  - Clean include dependency graph verified under `-Wall -Wextra`.
- **Standardized Error Reporting with Caret Formatting**:
  - Enhanced `DiagEntry` and `DiagBuf` in `diagnostics.h` and `diagnostics.c` to track source filenames (defaulting to `<stdin>`).
  - Standardized diagnostic printing via `diag_print_stream` and `semantic_print_diagnostics` to follow the standard `file:line:col: error: <msg>` format.
  - Implemented column-aligned caret indicator (`^`) positioned directly below the offending token for clear visual diagnostics.
- **Sprint 5.5 Completion**:
  - All 5 tasks of Sprint 5.5 (Tasks 40–44) complete.
  - 109/109 tests passing under `make test`, `make asan`, and `make valgrind`.

#### Subsystem Consolidation (`src/zero_cc/common/`) ✅
- **Directory Structure Simplification**:
  - Consolidated disparate utility, platform, and data structure directories (`utils/`, `data_structures/`, `platform/`, `structures/`) into a single unified `src/zero_cc/common/` directory.
  - Reduced `src/zero_cc/` subdirectories to 8 distinct, well-defined subsystems: `common/`, `core/`, `isa/`, `jit/`, `lexer/`, `parser/`, `semantic/`, `types/`.
- **Relocated Components**:
  - Arena memory allocator: `src/zero_cc/common/arena.{c,h}`.
  - Hash tables & double hashing: `src/zero_cc/common/hash_table.{c,h}`, `zero_tpht.h`, `zero_chained_tpht.h`, `zero_tinyptr.h`.
  - Virtual memory adapter: `src/zero_cc/common/mmap_adapter.h`.
  - POSIX platform shims: `src/zero_cc/common/zero_posix.h`, `posix_error.h`, `posix_fileio.h`, `posix_libc.h`, `posix_mmap.h`, `posix_system.h`.
- **Include Paths & Build Updates**:
  - Updated include paths across compiler subsystems (`parser`, `semantic`, `jit`) and all unit/semantic test suites.
  - Updated `Makefile` object targets (`EXTRA_OBJS`) and header dependency wildcards (`$(wildcard src/zero_cc/common/*.h)`).
  - 109/109 tests passing cleanly with zero memory leaks across `make test`, `make asan`, and `make valgrind`.

#### Out-of-Source Build Architecture (`build/`) ✅
- **Dedicated Build Directory**:
  - Migrated build system to an out-of-source structure isolating all intermediate artifacts to `build/`:
    - `build/obj/` for compiler object files (`.o`).
    - `build/test/` for categorized test binaries (`unit/`, `semantic/`, `abi/`, `jit/`).
  - Purged all legacy orphaned `.o` and `.exe` binaries from repository root and source trees.
  - Atomic cleanup: `make clean` now simply performs `rm -rf $(BUILD_DIR)`.
- **Harness & Runner Modernization**:
  - Removed Java JUnit XML reporting from `tools/test_runner.py` and `Makefile`.
  - Updated `test_runner.py` to auto-discover compiled binaries in `build/test/` with transparent fallback to `test/`.
#### Pure Subsystem Encapsulation (`src/zero_cc/`) ✅
- **Complete Elimination of Loose Files**:
  - Relocated all 12 loose implementation and façade files in `src/zero_cc/` into their matching subsystem directories:
    - `lexer.c`, `lexer.h` → `src/zero_cc/lexer/`
    - `parser.c`, `parser.h` → `src/zero_cc/parser/`
    - `semantic.c`, `semantic.h` → `src/zero_cc/semantic/`
    - `core.h` → `src/zero_cc/core/core.h`
    - `types_lexer.h` → `src/zero_cc/types/types.h`
    - `jit_linker.h` → `src/zero_cc/jit/jit_linker.h`
    - `isa_backend.h` → `src/zero_cc/isa/isa_backend.h`
    - `tls_backend.c`, `zero_cc_backends.h` → `src/zero_cc/jit/` (`tls_backend.c`, `backends.h`)
  - `src/zero_cc/` now contains **zero loose files** and consists strictly of 8 self-contained subsystem directories: `common/`, `core/`, `isa/`, `jit/`, `lexer/`, `parser/`, `semantic/`, `types/`.
- **Public API & Harness Alignment**:
  - Updated `include/zero_cc.h` and all internal include paths to reference clean subsystem paths.
  - All 109 tests compile with 0 warnings and pass cleanly across `make test` and `make asan`.




#### Virtual Register (VREG) System ✅
- ISA-independent virtual registers (VREG_0-15, VREG_F0-F15)
- Backend mapping functions (`get_int_reg()`, `get_fp_reg()`)
- Multi-ISA support (x86-64, aarch64, riscv64)
- RISC-V special remapping (VREG_0 → a0/x10)
- Documentation: `VREG.md`

#### Backend Architecture
- Unified backend interface (`zero_cc_backends.h`)
- TLS backend (`tls_backend`) for `_Thread_local` ✅ Complete
- Atomic backend (`atomic_backend`) for `_Atomic`
- ABI backend (`abi_backend`) for calling conventions
- Pragma backend (`pragma_backend`) for `#pragma` directives
- Modular design for cross-platform support (Linux/Windows/macOS/baremetal)

#### Documentation
- `TLS_IMPLEMENTATION_PLAN.md` - TLS and cross-platform backend plan
- Updated `README.md` with backend architecture overview

#### Variadic Functions & ABI Calling Convention ✅
- Implemented `__builtin_va_start` and `__builtin_va_arg` builtins in preprocessor and parser (`NODE_VA_START`, `NODE_VA_ARG`).
- Added semantic validation and type inference for variadic builtins in `visitor.c` and `expr.c`.
- Emitted System V AMD64 register spill area for variadic parameter registers (`rdi`, `rsi`, `rdx`, `rcx`, `r8`, `r9`).
- Lowered `NODE_VA_START` using hardware `lea -offset(%rbp), %rax` pointing to the parameter spill area.
- Lowered `NODE_VA_ARG` with stack pointer progression and dereferencing.
- Fixed System V AMD64 argument register mappings in `ast_lowering.h` (`rdi=7, rsi=6, rdx=2, rcx=1, r8=8, r9=9`).
- Fixed multi-argument call liveness tracking in `live_range.h` for packed arguments in `OP_CALL`.
- Fixed external function indirect calls in `x86_64.c` with 64-bit `movabs` (`REX.W + 0xBB`) and dynamic relocation recording.
- Synchronized semantic token identifiers (`EXPR_TK_*`) with lexer `ZeroCCTokType`.

#### Floating-Point Calling Conventions & Mixed SSE Arguments ✅
- Implemented System V AMD64 dual-cursor argument placement: integer arguments via GPRs (`rdi`, `rsi`, `rdx`, `rcx`, `r8`, `r9`) and floating-point arguments via SSE registers (`xmm0`–`xmm7`).
- Implemented caller-side float argument temporary spilling and reloading before call emission to protect float arguments against clobbering across argument expression evaluation.

#### Sprint 5 — Task 33: Union Layout & Overlapping Members ✅
- Implemented `TYPE_UNION` support across the type system and symbol tables.
- All union members placed at byte offset 0 (`m->offset = 0`), enabling proper memory overlap across scalar and compound types.
- Union total size calculated as `max(member.size)` padded up to `max(member.align)`.
- Fixed default member alignment calculation in `definitions.h` (`member_align = 0`) to prevent unannotated members from forcing 8-byte alignment.
- Added compile-time constant folding for `TK_SIZEOF` and `TK_ALIGNOF` in `parse_unary_ast` (`ast_expr.h`), resolving AST parser infinite loops.
- Added `STATEMENTS_HEADERS` dependency to `Makefile` for robust incremental builds of `statements.o`.
- Created comprehensive JIT validation suite in `test/union_jit.c`.
- Added vector register count tracking in `%al` before `call` execution (`mov al, <fp_reg_idx>`).
- Implemented callee-side parameter saving for incoming `xmm0`–`xmm7` registers in function prologues with proper ModR/M register encoding.
- Implemented `OP_CVTTSD2SI` opcode in the IR stream for float-to-integer conversion (`NODE_CAST`), ensuring conversions sequence correctly after function calls and IR-lowered expressions.
- Added float return value passing in `xmm0` with caller-side return type inspection.

#### Bug Fixes
- Fixed x86-64 load/store encoding (REX bits, displacement size)
- Fixed subtraction lowering (dst = src1 - src2)
- Fixed stack offset sign convention
- Fixed jump patch position
- Fixed push/pop REX prefix
- Fixed NODE_INDEX lowering: implemented proper array indexing with `OP_LOAD_BYTE` for char element access
- Fixed comparison operators (sete/setne/setl/setle/setg/setge): added zero-extension (movzx) after setcc instructions
- Fixed live range analysis: conditional jumps now properly record temp_src1 as a use
- Fixed phantom `NODE_DECL` semantic failure caused by enum shifts between header and object files
- Fixed duplicate `x86_emit_call` definition in `src/zero_cc/isa/x86_64.c`

### Changed
- Removed unused backend scaffolding (`backends/` directory)
- Moved `zero_cc_set_tls_backend()` to `tls_backend.c`
- Cleaned up unused struct members and declarations

### Technical Details

#### Platform-Specific TLS

| Platform | C Library | TLS Access |
|----------|-----------|------------|
| Linux x86-64 | musl/glibc | `fs` segment |
| Linux aarch64/riscv64 | musl/glibc | `x28`/`tp` register |
| Windows x86-64 | UCRT | `gs` segment |
| macOS x86-64/aarch64 | libc | `gs` segment / `x28` |
| Baremetal | None | Custom TLSBlock |

#### Backend Pattern

All features follow the same pattern:
1. Define backend interface in `zero_cc_backends.h`
2. Add to `ZeroCCCompiler` struct
3. Implement per-ISA (x86_64.c, aarch64.c, riscv.c)
4. Use in lowering: `BE(c)->feature_op(c, ...)`

### Planned Features (C11/C23)

| Feature | Backend | Priority |
|---------|---------|----------|
| `_Atomic` | `atomic_backend` | High |
| `_Alignas` / `_Alignof` | `abi_backend` | High |
| `inline` semantics | `isa_backend` | Medium |
| Complex numbers | `isa_backend` | Medium |
| `#pragma pack` | `pragma_backend` | Low |

---

## [Previous] - Sprint 3: Two-Pass Code Generation

### Added

#### Phase 1: IR Infrastructure ✅
- Two-pass code generation architecture
- 64K instruction buffer (`src/zero_cc/parser/ir.h`)
- 8-byte packed instruction format
- Register allocation integration (linear-scan)
- Optimization level support (-O0, -O1, -O2, -O3)

#### Documentation
- `TWO_PASS_ARCHITECTURE.md` - Complete architecture overview
- `2_pass_plan.md` - Detailed implementation plan
- `2_pass_decision.md` - Final design decisions
- `2_pass_considerations.md` - Edge cases and details
- `2_pass_progress.md` - Current progress status
- `circular_dependency_analysis.md` - Architecture analysis

#### Tests
- `test/ir_test.c` - IR buffer unit tests

### Changed
- Updated `README.md` with two-pass architecture documentation
- Integrated IR buffer into compilation flow (`compile_unit.h`, `compile.h`)

### Technical Details

#### IR Structure (8 bytes packed)
```c
typedef struct __attribute__((packed)) {
    uint8_t  op;        // 0-255 opcodes
    uint8_t  temp_dest; // 0-255 temps (0xFF if none)
    uint8_t  temp_src1; // 0-255 temps (0xFF if none)
    uint8_t  temp_src2; // 0-255 temps (0xFF if none)
    int16_t  imm;       // -32768 to 32767
    uint16_t label_idx; // 0-65535 labels
} Instr;
```

#### Register Classes
- **GPR**: RAX, RCX, RDX, RBX, RSI, RDI, R8-R15 (14 registers)
- **XMM**: XMM0-XMM7 (8 registers for SSE2 float)

### Performance Targets

| Metric | -O0 | -O1 | -O2 | -O3 |
|--------|-----|-----|-----|-----|
| Compile Time | 1x | 1.2x | 3x | 10x |
| Code Speed | 1x | 2-5x | 5-10x | 10-20x |

### Next Steps (Phase 2)
- Define 50 opcodes (MOV, ADD, SUB, etc.)
- Migrate NODE_BINOP to IR
- Implement Pass 2 code emission
- Test with arithmetic expressions

---

## [Previous Versions]

### Sprint 2: AST Lowering and JIT Execution
- Single-pass code generation
- Global variable storage emission
- All 71 tests passing
- Zero compiler warnings

### Sprint 1: Core Compiler Infrastructure
- Parser and lexer
- Type system
- Semantic analysis
- Basic expression evaluation

## [Unreleased] - Virtual Register (VREG) System

### Added

#### New Files
- `src/zero_cc/types/vregs.h` - Virtual register definitions (VREG_0-15, VREG_F0-F15)
- `VREG.md` - Complete VREG system documentation

#### Changes
- `src/zero_cc/isa_backend.h` - Added `get_int_reg()`/`get_fp_reg()` function pointers
- `src/zero_cc/parser/ast_lowering.h` - Updated to use ISA-independent VREGs
- `src/zero_cc/isa/x86_64.c` - VREG mapping + x86 encoding bug fixes
- `src/zero_cc/isa/aarch64.c` - VREG mapping
- `src/zero_cc/isa/riscv.c` - VREG mapping with RISC-V remapping

### Features
- **ISA Independence**: Lowering code is the same for all backends
- **Type Safety**: Separate integer (VREG_0-15) and float (VREG_F0-F15) registers
- **Special Registers**: `BE(c)->fp_reg`/`BE(c)->sp_reg` for frame/stack pointers
- **RISC-V Support**: Special remapping since RISC-V has non-linear register numbering

### Bug Fixes
- Fixed x86-64 load/store encoding (wrong REX bits, wrong displacement size)
- Fixed subtraction lowering (dst = src1 - src2, not src1 - dst + src2)
- Fixed stack offset sign convention (use -offset for [fp - offset])
- Fixed jump patch position (return position of rel32 field, not opcode)
- Fixed push/pop REX prefix (not needed in 64-bit mode)
