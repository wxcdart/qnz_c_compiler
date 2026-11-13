---
type: Concept
title: RV32 64-bit Integer (long long) Support Roadmap
description: Four-phase architectural roadmap for 64-bit integer arithmetic, ABI calling conventions, libgcc div/rem, and sanitizer integration on 32-bit RISC-V (rv32).
resource: backend/rv32-i64-roadmap
tags: [riscv32, rv32, i64, long-long, gpr-pair, abi, roadmap, sanitizer]
timestamp: 2026-11-13T05:32:37Z
generated: antigravity-agent
verified: engineering-lead
status: active
---

# RV32 64-bit Integer (`long long`) Support Roadmap

> **Status (Sprint 170 - 174):** **ALL PHASES (1, 2, 3, and 4) are COMPLETE**.
> - **Phase 1 (Core 64-bit Arithmetic)**: **COMPLETE** (Sprint 170/171). Core `OP_*64` opcodes, frontend expression/decl/assign/unop/cast lowering, raw binary and `-S` assembly emitters, borrow condition fix in SUB64, and comprehensive test suite (`test/jit/rv32_i64.c`) verified in both raw and text paths under `qemu-riscv32-static`.
> - **Phase 2 (64-bit ABI & I/O Calling Conventions)**: **COMPLETE** (Sprint 172). RISC-V ILP32 psABI even-odd pair passing (`a0:a1`..`a6:a7`), 8-byte stack alignment, `(a0, a1)` returns via `OP_MK64`/`OP_HI64`, callee scalar reception, variadic `printf %lld`, and strict ILP32 `long` (32-bit) vs `long long` (64-bit) scoping. Verified via `test/jit/rv32_abi.c` across all `-O0..-O3` levels in raw and `-S` paths under `qemu-riscv32-static`.
> - **Phase 3 (64-bit Div/Rem via libgcc)**: **COMPLETE** (Sprint 173). 64-bit division/remainder (`/`, `%`, `/=`, `%=`) dispatching `OP_DIV64`, `OP_MOD64`, `OP_UDIV64`, `OP_UMOD64` to `__divdi3`, `__moddi3`, `__udivdi3`, `__umoddi3` runtime helpers in libgcc. Root-caused and resolved GPR64 stack spill slot positive/negative displacement offset bug in `ast_lowering_mc.c` and `asm_emitter.c` (`-off + 4` vs `-off - 4`) and GPR64 thief handover in `regalloc.h`. Verified in both raw and asm paths under `qemu-riscv32-static`.
> - **Phase 4 (Sanitizer Width-8 Checks)**: **COMPLETE** (Sprint 174). Lifted the rv32 width-8 `OP_OVF_TRAP` skip in `zero_san`, enabling signed integer overflow (`ADD64`, `SUB64`, `MUL64`), div-by-zero, rem-by-zero, and shift bounds checks on 64-bit integers with pair register allocation and spill reloading in both raw and text assembly emitters. Fully integrated into `tools/verify/san_sweep.py` (`san_trap_cross_wide.c` on `riscv32`).

---

## 1. Overview & Architectural Motivation

On 32-bit RISC-V (`--target riscv32`, ILP32 ABI), machine registers are 32 bits wide (`XLEN = 32`). While 64-bit target architectures (x86-64, AArch64, RV64) manipulate C99 `long long` (`int64_t` / `uint64_t`) in single 64-bit general-purpose registers, RV32 requires representing each 64-bit integer as a **`(lo, hi)` 32-bit register pair**.

Historically, ZeroCC lacked 64-bit integer lowering on RV32: 64-bit operations were truncated to 32 bits or treated as 32-bit operations, causing severe divergence from GCC (documented in OKF [sanitizer-roadmap.md](./sanitizer-roadmap.md) §4d).

### Architectural Invariants & Scope Rules
1. **Target-Confined Lowering**: The pair representation and `OP_*64` IR opcodes exist **only** for `--target riscv32`. On 64-bit ISAs, 64-bit operations remain standard single-register instructions (`OP_ADD`, `OP_SUB`, etc.).
2. **Phase Independence**: The 4 phases are partitioned so each phase is verifiable on its own under QEMU:
   - **Phase 1** tests value representation and arithmetic purely through global and local variables, returning results via `main`'s 32-bit exit code without touching function calling conventions.
   - **Phase 2** introduces the inter-procedural calling ABI.
   - **Phase 3** integrates external runtime helpers for operations RV32 cannot perform in instructions.
   - **Phase 4** extends the safety instrumentation (`zero_san`) to the validated 64-bit representation.

---

## 2. Phase 1 — Core 64-bit Arithmetic (Complete)

Phase 1 establishes the virtual register representation, frontend lowering, and machine code emitters for 64-bit integer operations on local and global variables.

### IR Representation & Opcodes
ZeroCC introduces dedicated 64-bit IR opcodes targeting `REG_CLASS_GPR64`:
- **Arithmetic**: `OP_ADD64`, `OP_SUB64`, `OP_MUL64`, `OP_NEG64`
- **Bitwise & Logic**: `OP_AND64`, `OP_OR64`, `OP_XOR64`, `OP_NOT64`
- **Shifts**: `OP_SHL64` (logical left), `OP_SHR64` (logical right), `OP_SAR64` (arithmetic right)
- **Comparisons**: `OP_CMP64` with predicate materialization (`EQ`, `NE`, `LT`, `LE`, `GT`, `GE`)
- **Data Movement & Conversions**: `OP_MOV64`, `OP_MOV64_IMM`, `OP_LOAD64`, `OP_STORE64`, `OP_ZEXT32_64`, `OP_SEXT32_64`, `OP_TRUNC64_32`
- **Reserved Slots (Phase 3)**: `OP_DIV64`, `OP_MOD64`, `OP_UDIV64`, `OP_UMOD64`

### Register Allocation & Pair Model
- **Register Class**: `REG_CLASS_GPR64` virtual registers map to adjacent physical register pairs allocated via `gpr128_alloc_pair` (`s1..s11`).
- **Temporary Pool**: `TR_POOL_COUNT` in `opt_temp_renumber.c` is expanded to `8 * 256` to prevent virtual register pool exhaustion.
- **Scratch Invariants**:
  - `t0` (5) and `t5` (30) serve as short-lived scratch for carry and shift operations.
  - `t2` (7) is unallocatable on RV32, reserved for carry propagation in `ADD64` and cross-term arithmetic in `MUL64`.

### Emitter Logic (Raw MC & `-S` Assembly)
- **ADD64 / SUB64**:
  - `sum_lo = alo + blo`; carry is `sltu t2, sum_lo, alo`. `dhi = ahi + bhi + carry`.
  - `sub_lo = alo - blo`; borrow is `sltu t0, alo, blo`. `dhi = ahi - bhi - borrow`.
- **MUL64**:
  - Low 64 bits of $A \times B$: `dlo = alo * blo` (`mul`), `cross = alo * bhi + ahi * blo`, `dhi = mulhu(alo, blo) + cross`.
- **Shifts (SHL64, SHR64, SAR64)**:
  - Multi-way branch handling shift counts `cnt < 32`, `32 <= cnt < 64`, and `cnt >= 64`.
  - Operands and cross-terms (`slo << (32 - cnt)` or `slo >> (32 - cnt)`) are read before destination registers are clobbered to avoid aliasing corruption.
- **Memory Access**:
  - `STORE64`: stores `lo` at `addr`, `hi` at `addr + 4`.
  - `LOAD64`: loads `lo` from `addr`, `hi` from `addr + 4` (with ordering guarded against destination register overlap).
- **Constants (`MOV64_IMM`)**:
  - Materializes the 32-bit low half into `dlo` and the 32-bit high half into `dhi`.

### Phase 1 Verification Scope
- Verified without ABI dependencies using tests where 64-bit variables are manipulated as locals or globals, and assertions return a 32-bit integer status code in `main`.
- Swept across both raw binary (`ZC_RAW_AOT=1`) and text assembly (`-S`) paths under `qemu-riscv32-static`.

---

## 3. Phase 2 — 64-bit ABI & I/O Calling Conventions (Complete)

Phase 2 enables 64-bit values to cross function boundaries and interact with C library I/O, conforming to the standard RISC-V ILP32 psABI.

### RISC-V ILP32 psABI Calling Convention
According to the RISC-V ILP32 psABI:
1. **Argument Passing**:
   - 64-bit integer arguments (`long long`, `uint64_t`) are passed in **aligned even-odd register pairs** (`a0:a1`, `a2:a3`, `a4:a5`, `a6:a7`).
   - If an argument begins at an odd register (e.g. `a1` after an `int` in `a0`), that odd register is skipped, and the 64-bit value aligns to the next even register (`a2:a3`).
   - If fewer than 2 argument registers remain, the 64-bit argument does not split between registers and stack; it spills entirely to the stack aligned to 8 bytes (`stack_idx = (stack_idx + 1) & ~1`).
2. **Return Values**:
   - 64-bit integer return values are returned in `(a0, a1)`, where `a0` holds the low 32 bits and `a1` holds the high 32 bits.
   - In ZeroCC, `a0` maps to `VREG_0` and `a1` maps to `VREG_2` (pre-colored to physical register 11 on RISC-V).
   - A dedicated IR opcode `OP_MK64` (`ir_emit_mk64(c, dst, lo, hi)`) pairs `(VREG_0, VREG_2)` into a `REG_CLASS_GPR64` virtual register in caller scope.
   - For 64-bit returns in callee functions, `ir_emit_mov_reg(c, VREG_0, lo)` and `ir_emit_mov_reg(c, VREG_2, hi)` place the respective words into the return registers.
3. **Callee Parameter Reception**:
   - Parameter promotion (`can_promote_params`) is disabled for scalar 64-bit arguments on RV32 (`sym->type.tag == TYPE_INT64 || sym->type.tag == TYPE_UINT64`).
   - Even-odd register pairs are saved to the frame stack: `sym->stack_offset` holds `lo` and `sym->stack_offset - 4` holds `hi`.
   - Spilled arguments are loaded from caller stack offsets `16 + 4 * stack_idx` with 8-byte alignment preserved.
4. **Variadic Arguments & `printf %lld`**:
   - Variadic functions unpack 64-bit values aligned to 8 bytes from the argument buffer.
   - Lowering preserves 64-bit argument tags so callers forward both halves into aligned register pairs or 8-byte stack slots.
5. **ILP32 Data Model Alignment (`long` vs `long long`)**:
   - On 32-bit RISC-V (ILP32), `long` and `unsigned long` are 32 bits (4 bytes), while `long long` and `unsigned long long` are 64 bits (8 bytes).
   - Scoped type resolution in `registry.h`, `type_info.h`, `type_parser.h`, `definitions.h`, `decl_local.c`, `decl_struct.c`, `zero_cc_declarator_stack.h`, and `ast_lowering_binop.c` to ensure single `long` maps to `TYPE_INT32`/`TYPE_UINT32` on RV32, preventing ABI parameter misalignment with libc `printf("%ld", ...)` or `long` parameters.
6. **ASM Emitter Multiplier Fix**:
   - `riscv_emit_mul64_asm` in `src/zero_cc/isa/riscv_asm.c` was corrected to emit `mulhu` (unsigned high multiply) rather than `mulh` for `P0.hi = alo * blo`, matching the raw emitter in `riscv64.c`.

### Phase 2 Verification
- Verified with `test/jit/rv32_abi.c` across 5 test suites (direct 64-bit params/returns, even-odd register alignment, stack spilling, variadic `printf %lld`, and intermixed 32/64-bit arguments).
- Tested across all four optimization levels (`-O0..-O3`) in both raw binary and `-S` assembly paths under `qemu-riscv32-static`.
- Verified cross-ISA gates: `isa_qemu_diff.py` (63/63 PASS across all 4 target ISAs), `san_sweep.py` (644/644 PASS), `make test` (229/229 PASS), `make test-opt` (128/128 PASS).

---

## 4. Phase 3 — 64-bit Division & Modulo via libgcc (Complete)

Because RV32 has no native 64-bit hardware divide instructions (the `M` extension provides only 32-bit `div`/`rem`), 64-bit division and modulo are lowered as runtime helper calls.

### Integration with libgcc Helpers
- **Opcodes**: `OP_DIV64`, `OP_MOD64`, `OP_UDIV64`, `OP_UMOD64`.
- **Runtime Symbols**:
  - Signed division: `__divdi3(int64_t a, int64_t b)`
  - Signed remainder: `__moddi3(int64_t a, int64_t b)`
  - Unsigned division: `__udivdi3(uint64_t a, uint64_t b)`
  - Unsigned remainder: `__umoddi3(uint64_t a, uint64_t b)`
- **Calling Convention & Argument Staging**:
  - `riscv32_emit_64div_call` (raw) and `riscv32_emit_64div_asm` (assembly): stages arguments `(a_lo, a_hi, b_lo, b_hi)` into `(a0..a3)` using reserved scratch registers `t0, t2, t5` to prevent clobbering when arguments overlap with `a0..a3`. Emits `jal` with `R_RISCV_CALL_PLT` relocation (or `call` in text assembly) and moves `(a0, a1)` to destination pair `(dlo, dhi)`.
- **Compiler Pipeline Integration**:
  - `live_range.h`: records `OP_DIV64`, `OP_MOD64`, `OP_UDIV64`, `OP_UMOD64` as function calls to spill live caller-saved registers across division sites.
  - `opt_gvn.c`: treats division operations as memory barriers in `opt_restrict_memory_forward`.
  - `main.c`: own-linker driver automatically extracts the RV32 libgcc shim (`libgcc.a`) from the musl cross-toolchain prefix on `--target riscv32`.
- **Regalloc & Spill Slot Displacement Bugfixes (Crucial)**:
  1. **Linear Scan Steal Handover (`regalloc.h`)**: Fixed register steal handover for `REG_CLASS_GPR64` intervals: previously only `REG_CLASS_GPR128` transferred `assigned_reg_hi`, leaving a spilled `GPR64` thief with `assigned_reg_hi = -1`.
  2. **GPR64 Frame Spill Slot Addressing (`ast_lowering_mc.c`, `asm_emitter.c`)**:
     - `off = spill_base_offset + (spill_slot + 1) * 8` defines the bottom of the slot in frame space (`-off`).
     - Previously, the high half was addressed at `zero_cc_frame_disp(c, -off - 4)`, which subtracted 4 from the bottom of the slot, placing the high half 4 bytes below the allocated stack frame (`-4(sp)`).
     - When `__udivdi3` was called, its own prologue clobbered `-4(sp)`, corrupting any spilled 64-bit value's high half across the division call.
     - Corrected to `zero_cc_frame_disp(c, -off + 4)` across all reload and store paths.
  3. **64-bit Shift Count Spill (`ast_lowering_mc.c`, `asm_emitter.c`)**:
     - For 64-bit shifts (`OP_SHL64`, `OP_SHR64`, `OP_SAR64`), `src2` is a 32-bit shift count. Prevented `phys_src2 = g64_s2lo` from overwriting the reloaded scalar shift count.

### Phase 3 Verification
- Tested comprehensive division and remainder suite in `test/jit/rv32_i64.c` (Section 11) covering signed/unsigned `/` and `%`, negative divisors/dividends, edge cases (`/ 1`, `% 1`, `x / x`, `x % x`), maximum 64-bit integers (`0xFFFFFFFFFFFFFFFFULL / 1000ULL`), and compound assignments (`/=`, `%=`).
- Swept across all optimization levels (`-O0..-O3`) in both raw binary and `-S` assembly modes under `qemu-riscv32-static`: **100% PASS**.

---

## 5. Phase 4 — Target Sanitizer (`zero_san`) Width-8 Checks (Complete)

Phase 4 folds RV32 64-bit integer operations into the compiler's target-side undefined behavior checks (`-fsanitize=undefined` / `-fsanitize-trap`), completing the 4-phase roadmap.

### Scope of Instrumentation & Implementation Details
1. **Lifting the Bypass**:
   - Removed the `c->isa == ISA_RISCV32 && w == 8` bypass from `zc_emit_ovf_trap` in `ast_lowering_binop.c`.
   - Wired 64-bit overflow and UB checks into `zc_lower_binop64` and 64-bit compound assignment lowering in `ast_lowering_assign.c` for `+`, `-`, `*`, `/`, `%`, `<<`, `>>`.
2. **Backend Architecture (`emit_ovf_trap64`)**:
   - Added `emit_ovf_trap64` and `emit_ovf_trap64_asm` to `ZeroCCISABackend` (`(c, rlo, rhi, alo, ahi, blo, bhi, width, op)`).
   - Implemented `riscv32_emit_ovf_trap64` in `riscv64.c` (raw MC) and `riscv32_emit_ovf_trap64_asm` in `riscv_asm.c` (text assembly).
3. **Multiword Check Algorithms**:
   - **ADD64 / SUB64**: Overflow in signed two's-complement arithmetic depends solely on the operand and result sign bits (bit 31 of high halves):
     - ADD: $(A_{hi} \oplus Res_{hi}) \land (B_{hi} \oplus Res_{hi}) < 0$. Checked via `xor t0, ahi, rhi; xor t2, bhi, rhi; and t0, t0, t2; blt t0, zero, trap`.
     - SUB: $(Res_{hi} \oplus A_{hi}) \land (A_{hi} \oplus B_{hi}) < 0$. Checked via `xor t0, rhi, ahi; xor t2, ahi, bhi; and t0, t0, t2; blt t0, zero, trap`.
   - **MUL64**:
     - Fast-path check: if both operands fit in signed 32-bit integers ($A_{hi} = A_{lo} \gg 31$ and $B_{hi} = B_{lo} \gg 31$), the product never overflows signed 64 bits.
     - Full check: computes higher product words ($P_2, P_3$) using RV32 `mulhu`, `mulhsu`, `mulh`, `mul`, `sltu` into reserved scratch registers `t0, t2, t5`, trapping if $P_2 \ne (Res_{hi} \gg 31)$ or $P_3 \ne (Res_{hi} \gg 31)$.
   - **DIV_ZERO / REM_ZERO**: 64-bit divisor is zero iff $(Div_{lo} \lor Div_{hi}) = 0$. Checked via `or t0, rlo, rhi; bne t0, zero, skip; unimp`.
   - **SHIFT_COUNT**: Shift count is a 32-bit scalar operand ($0 \le count < 64$). Checked via `li t5, 64; bge rlo, zero, L1; unimp; L1: blt rlo, t5, L2; unimp; L2:`.
4. **IR Liveness & Register Allocation Preservation**:
   - In `live_range.h`, updated `build_live_intervals` so `OP_OVF_TRAP` records its operands (`temp_dest`, `temp_src1`, `temp_src2`) as USES (`is_def = false`), ensuring physical registers remain live and un-clobbered across the check without redefining intervals.
   - In `ast_lowering_mc.c` and `asm_emitter.c`, handled spill reloading for `OP_OVF_TRAP`: loads `dhi` into `t0` for arithmetic ops, `(dlo, dhi)` into `(t1, t3)` for div/rem zero ops, and `dlo` into `t0` for shift bounds.
5. **Verification**:
   - Extended `tools/verify/san_sweep.py` cross-trap runner: added `"riscv32"` to `CROSS_WIDE_ISAS`.
   - Fixed runner loop in `san_sweep.py` to test both `raw` and `asm` paths.
   - All tests in `test/san/` and `san_trap_cross_wide.c` pass cleanly across `-O0..-O3` in both raw and text paths under `qemu-riscv32-static`.

---

## 6. Verification Matrix Across Phases

| Phase | Features | Verification Harness & Test Files | Target Linkage |
| :--- | :--- | :--- | :--- |
| **Phase 1** | Pair arithmetic, shifts, logic, unops, casts, loads/stores, constants, condition codes | `test/jit/rv32_i64.c` (10 test categories: arith, logic, unops, shifts, signed/unsigned cmp, casts, branches/loops, pointers/arrays/structs) | Standalone ELF & `-S` text link, QEMU RV32 static |
| **Phase 2** | Even-odd psABI pairs, function calls, `printf %lld` | Multi-function call tests, `test/jit/int128_args.c` adaptation | Static musl cross link, QEMU |
| **Phase 3** | `__divdi3`, `__moddi3`, `__udivdi3`, `__umoddi3` | 64-bit division battery (corner cases: `LLONG_MIN / -1`, div-by-zero) | Static musl cross link with libgcc shim |
| **Phase 4** | Width-8 signed overflow & shift traps | `test/san/san_trap_*.c` enabled at width 8 on RV32 | `make test-san` under QEMU |

---

## Related Concepts
- Return to [Root Index](./index.md)
- See [Multi-ISA Backends (x86-64, AArch64, RISC-V)](./isa-backends.md)
- See [Target-Side Sanitizer (zero_san) Roadmap](./sanitizer-roadmap.md)
- See [Zero_RT & Freestanding Runtime](./zero-rt.md)
