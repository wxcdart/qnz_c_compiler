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
  - Standard JUnit XML output mode (`--junit <file>`) for CI/CD integration.
  - Category filtering (`--category <cat>`) and per-test execution support.
- **Enhanced Makefile Targets**:
  - `make test` runs the unified runner across all 108 tests.
  - `make test-unit`, `make test-semantic`, `make test-abi`, `make test-jit` for focused testing.
  - `make test-tap` and `make test-junit` for automation pipelines.
  - Automatic directory creation (`@mkdir -p $(dir $@)`) for all test binaries.

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
