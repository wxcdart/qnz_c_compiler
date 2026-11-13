---
type: Concept
title: RV32 64-bit Integer (long long) Support Roadmap
description: Four-phase architectural roadmap for 64-bit integer arithmetic, ABI calling conventions, libgcc div/rem, and sanitizer integration on 32-bit RISC-V (rv32).
resource: backend/rv32-i64-roadmap
tags: [riscv32, rv32, i64, long-long, gpr-pair, abi, roadmap, sanitizer]
timestamp: 2026-11-13T02:48:26Z
generated: antigravity-agent
verified: engineering-lead
status: active
---

# RV32 64-bit Integer (`long long`) Support Roadmap

> **Status (Sprint 170 / 171):** **Phase 1 is COMPLETE (core arithmetic, bitwise logic, unary ops, shifts, compares, 32↔64 casts, conditionals, globals/locals/pointers/arrays/structs landed and verified under QEMU)**.
> - **Phase 1 (Core 64-bit Arithmetic)**: **COMPLETE**. Core `OP_*64` opcodes, frontend expression/decl/assign/unop/cast lowering, raw binary and `-S` assembly emitters, borrow condition fix in SUB64, and comprehensive test suite (`test/jit/rv32_i64.c`) verified in both raw and text paths under `qemu-riscv32-static`.
> - **Phase 2 (64-bit ABI & I/O)**: Planned / Next. RISC-V psABI even-odd pair passing (`a0:a1`..`a6:a7`), 64-bit return values, and variadic `printf %lld`.
> - **Phase 3 (64-bit Div/Rem via libgcc)**: Planned. Dispatching `DIV64`/`MOD64`/`UDIV64`/`UMOD64` to `__*di3` helper calls in the rv32 static-musl libgcc shim.
> - **Phase 4 (Sanitizer Width-8 Checks)**: Planned. Lifting the rv32 width-8 `OP_OVF_TRAP` skip in `zero_san`, enabling signed integer overflow and shift bounds checks on 64-bit integers.

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

## 3. Phase 2 — 64-bit ABI & I/O Calling Conventions

Phase 2 enables 64-bit values to cross function boundaries and interact with C library I/O.

### RISC-V psABI Calling Convention
According to the RISC-V ILP32 psABI:
1. **Argument Passing**:
   - 64-bit integer arguments (`long long`, `uint64_t`) are passed in **aligned even-odd register pairs** (`a0:a1`, `a2:a3`, `a4:a5`, `a6:a7`).
   - If an argument begins at an odd register (e.g. `a1` after an `int` in `a0`), that odd register is skipped, and the 64-bit value aligns to the next even register (`a2:a3`).
   - If register pairs are exhausted, arguments spill to the stack aligned to 8 bytes.
2. **Return Values**:
   - 64-bit integer return values are returned in `(a0, a1)`, where `a0` holds the low 32 bits and `a1` holds the high 32 bits.
3. **Variadic Arguments & `printf %lld`**:
   - Variadic functions (such as `printf`) format 64-bit values by unpacking 8-byte aligned pairs from the varargs buffer.
   - Variadic argument lowering on RV32 must align 64-bit arguments to 8-byte boundaries on the argument save area.

---

## 4. Phase 3 — 64-bit Division & Modulo via libgcc

Because RV32 has no native 64-bit hardware divide instructions (the `M` extension provides only 32-bit `div`/`rem`), 64-bit division and modulo must be lowered as runtime helper calls.

### Integration with libgcc Helpers
- **Opcodes**: `OP_DIV64`, `OP_MOD64`, `OP_UDIV64`, `OP_UMOD64`.
- **Runtime Symbols**:
  - Signed division: `__divdi3(int64_t a, int64_t b)`
  - Signed remainder: `__moddi3(int64_t a, int64_t b)`
  - Unsigned division: `__udivdi3(uint64_t a, uint64_t b)`
  - Unsigned remainder: `__umoddi3(uint64_t a, uint64_t b)`
- **Toolchain Shim**:
  - The static-musl cross toolchain wrapper (`tools/toolchain/setup_musl_cross.sh`) provides `libgcc.a` defining `__*di3`.
  - ZeroCC's own `zero_rt/core/int128.c` avoids redefining these symbols on RV32 to maintain a single provider on the system link line (see OKF [zero-rt.md](./zero-rt.md) and [sanitizer-roadmap.md](./sanitizer-roadmap.md)).

---

## 5. Phase 4 — Target Sanitizer (`zero_san`) Width-8 Checks

Phase 4 folds RV32 64-bit integer operations into the compiler's target-side undefined behavior checks (`-fsanitize=undefined` / `-fsanitize-trap`).

### Scope of Instrumentation
1. **Signed Overflow Traps (`OP_OVF_TRAP`)**:
   - Currently, `ast_lowering.c` skips width-8 overflow checks when compiling for RV32 (`ISA_RISCV32 && w == 8`).
   - Phase 4 removes this bypass and emits multiword overflow validation:
     - **ADD64**: detects signed overflow when both operands have the same sign and the result sign differs:
       $(A_{hi} \oplus Res_{hi}) \land (B_{hi} \oplus Res_{hi}) < 0$.
     - **SUB64**: detects signed overflow when operands have opposite signs and result sign differs:
       $(Res_{hi} \oplus A_{hi}) \land (A_{hi} \oplus B_{hi}) < 0$.
     - **MUL64**: checks whether the high 64 bits of the 128-bit full product are an arithmetic sign-extension of the low 64-bit result.
2. **Shift-Count Exponent Checks**:
   - Ensures shift counts for 64-bit shifts satisfy $0 \le count < 64$.
   - Validates that negative shift counts or counts $\ge 64$ trap via `__zerocsan_report` with `ZC_OVF_OP_SHBIG` or `ZC_OVF_OP_SHNEG`.

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
