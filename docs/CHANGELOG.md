# ZeroCC Changelog

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
  - Implemented 9–16 byte struct by-value parameter and return value handling in registers (`rax` / `rdx`).
  - Added new backend hooks `emit_param_save_int_asm`, `emit_param_save_fp_asm`, and `emit_variadic_spill_asm` to `ZeroCCISABackend`.
  - **Struct by-value passing**: Full support for structs of all sizes (1-64 bytes) passed by value in function arguments.
  - **Struct by-value returning**: Support for structs up to 16 bytes returned in registers (RAX:RDX).

### Test Results
- **Pass Rate**: 128/128 tests passing (100%) in `make test`, including **13 struct by-value tests** in `test/abi/` covering various sizes, alignments, nesting, and edge cases.

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

