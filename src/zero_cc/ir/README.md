# IR, AST Lowering, Register Allocation & Code Emission (`src/zero_cc/ir/`)

## Overview

`src/zero_cc/ir/` implements the middle-end Intermediate Representation (IR), AST-to-IR lowering, linear-scan register allocation, and dual-path code emission (in-memory JIT / ELF binary emission and GNU Assembler `.s` text emission):

- **IR Representation & Arenas**: [`ir.h`](./ir.h), [`ir.c`](./ir.c), [`ir_arena.h`](./ir_arena.h), [`ir_emitter.h`](./ir_emitter.h), [`ir_emit_ops.h`](./ir_emit_ops.h)
- **AST-to-IR Lowering**: [`ast_lowering.c`](./ast_lowering.c), [`ast_lowering_stmt.c`](./ast_lowering_stmt.c), [`ast_lowering_expr.c`](./ast_lowering_expr.c), [`abi_classify.h`](./abi_classify.h)
- **Live-Range Analysis & Linear-Scan Register Allocation**: [`live_range.h`](./live_range.h), [`regalloc.h`](./regalloc.h)
- **Code Emission (JIT/ELF Binary & AOT Assembly)**: [`ast_lowering_mc.c`](./ast_lowering_mc.c), [`asm_emitter.c`](./asm_emitter.c), [`machine_emitter.h`](./machine_emitter.h), [`label_patcher.h`](./label_patcher.h)

> **Subsystem Boundaries**:
> - Frontend AST construction lives in [`src/zero_cc/parser/`](../parser/).
> - Middle-end IR optimization passes (`opt_pass.c`, `opt_mem2reg.c`, `opt_loop.c`, `opt_vectorize.c`, etc.) live in [`src/zero_cc/opt/`](../opt/).
> - Target ISA vtable implementations (`x86_64`, `aarch64`, `riscv`) live in [`src/zero_cc/isa/`](../isa/).

---

## Architecture

### Two-Path Code Generation

Both code generation paths consume the same optimized, register-allocated IR buffer:

```
AST (parser/) ──► Semantic Analysis (semantic/) ──► AST Lowering (ir/ast_lowering*.c)
                                                            │
                                                            ▼
                                                   IR Buffer (ir/ir.c)
                                                            │
                                                            ▼
                                                Optimization Passes (opt/)
                                                            │
                                                            ▼
                                       Live Ranges & RegAlloc (ir/live_range.h, regalloc.h)
                                                            │
                                           ┌────────────────┴────────────────┐
                                           ▼                                 ▼
                                  ir/asm_emitter.c                ir/ast_lowering_mc.c
                                  GAS Assembly (.s)            Direct Machine Code (.text)
                                           │                                 │
                                           ▼                                 ├──► JIT Execution (--run)
                                      Host Assembler                         └──► ELF Writer (-c / -o)
```

### File Organization (`src/zero_cc/ir/`)

| File | Purpose |
|------|---------|
| [`ir.h`](./ir.h) / [`ir.c`](./ir.c) | 85-opcode `Instr` representation, `IRCallArgs` side table, and IR arena management |
| [`ir_arena.h`](./ir_arena.h) | Growable `IRArena` and `IRCallArgsArena` data structures |
| [`ir_emitter.h`](./ir_emitter.h) / [`ir_emit_ops.h`](./ir_emit_ops.h) | Typed helper functions (`ir_emit_*`) for appending instructions to the active `IRArena` |
| [`lowering_state.h`](./lowering_state.h) | Per-function lowering context (`ZeroCCLoweringState`: VLA state, loop label stacks, alias groups) |
| [`abi_classify.h`](./abi_classify.h) | System V AMD64 / AAPCS64 / LP64D struct eightbyte classification and `sret` helpers |
| [`ast_lowering.c`](./ast_lowering.c) | Per-function pipeline coordinator (`lower_function`, `zero_cc_lower_ast`) |
| [`ast_lowering_stmt.c`](./ast_lowering_stmt.c) | Statement lowering (`if`/`else`, `while`, `for`, `do`/`while`, `switch`, `goto`, `return`, local `NODE_DECL`, VLAs) |
| [`ast_lowering_expr.c`](./ast_lowering_expr.c) | Expression lowering (arithmetic, pointer/array indexing, structs, compound literals, `va_arg`, `NODE_CALL`, atomics) |
| [`ast_lowering_internal.h`](./ast_lowering_internal.h) / [`ast_lowering_ir.h`](./ast_lowering_ir.h) | Shared declarations across lowering translation units |
| [`live_range.h`](./live_range.h) | Liveness analysis, call-spanning (`spans_call`) detection, and loop backedge live-range extension |
| [`regalloc.h`](./regalloc.h) | Linear-scan register allocator (`RegAllocator`) with callee-saved enforcement and stack spill slot assignment |
| [`ast_lowering_mc.c`](./ast_lowering_mc.c) | IR → binary machine code emitter (`emit_ir_to_mc`) with unified scratch-register spill reload/store |
| [`asm_emitter.h`](./asm_emitter.h) / [`asm_emitter.c`](./asm_emitter.c) | IR → GAS text assembly emitter (`emit_ir_to_asm`) with unified scratch-register spill reload/store |
| [`machine_emitter.h`](./machine_emitter.h) | Low-level byte buffer emission helpers (`me_emit_u8`, `me_emit_u32`, `me_emit_u64`) |
| [`label_patcher.h`](./label_patcher.h) | Two-pass forward/backward branch target offset patching for binary codegen |

### Build Pattern

Each `.c` file in `src/zero_cc/ir/` (`ir.c`, `ast_lowering.c`, `ast_lowering_stmt.c`, `ast_lowering_expr.c`, `ast_lowering_mc.c`, `asm_emitter.c`) is compiled as a standalone `.o` translation unit in [`Makefile`](../../../Makefile) with `-MMD -MP` header dependency tracking.

---

## Register Allocation & Spill Handling (`live_range.h`, `regalloc.h`)

- **Reserved Physical Registers (`temp < 16`)**:
  - `VREG_0` (`0`, `%rax`/`x0`/`a0`) and `VREG_2` (`2`, `%rdx` for 9–16B struct returns) are fixed ABI physical registers.
  - `%rsp` (`4`), `%rbp` (`5`), `%rcx` (`1`, variable shifts), and `%r10`/`%r11` (`10`/`11`, backend scratch registers `scratch_reg1`/`scratch_reg2`) are never allocated to general virtual registers (`temp >= 16`).
- **Call-Spanning Virtual Registers (`spans_call`)**:
  - Any virtual register whose live range crosses `OP_CALL` or `OP_CALL_INDIRECT` is restricted exclusively to callee-saved GPRs (`%rbx`, `%r12`–`%r15` on x86-64; `x19`–`x28` on AArch64; `s1`–`s11` on RISC-V).
  - Function prologues and epilogues symmetrically emit `OP_CALLEE_SAVE_STORES` and `OP_CALLEE_SAVE_LOADS` (`-8(%rbp)`..`-40(%rbp)`).
- **Unified Stack Spill / Reload**:
  - When register pressure exceeds available GPRs (or callee-saved GPRs across a call), `regalloc_allocate()` assigns a stack spill slot at `-(spill_base_offset + spill_slot * 8)`.
  - Both [`ast_lowering_mc.c`](./ast_lowering_mc.c) and [`asm_emitter.c`](./asm_emitter.c) transparently reload spilled source operands into `scratch_reg1` / `scratch_reg2` before instruction execution (including spilled `OP_CALL` arguments and indirect callee pointers) and store spilled destinations afterward.

---

## Supported Opcode Families (`ir.h`)

- **Data Movement**: `OP_MOV`, `OP_MOV_IMM`, `OP_MOV_REG`, `OP_MOV_MEM`, `OP_LEA`, `OP_LEA_RIP`, `OP_MOV_GLOBAL`, `OP_DEREF`, `OP_LOAD_BYTE`, `OP_LOAD_16`, `OP_LOAD_32`, `OP_STORE_BYTE`, `OP_STORE_16`, `OP_STORE_32`, `OP_STORE_64`, `OP_PUSH`, `OP_POP`
- **Integer Arithmetic & Bitwise**: `OP_ADD`, `OP_SUB`, `OP_MUL`, `OP_DIV`, `OP_MOD`, `OP_NEG`, `OP_AND`, `OP_OR`, `OP_XOR`, `OP_NOT`, `OP_SHL`, `OP_SHR`, `OP_SAR`
- **Comparison & Control Flow**: `OP_CMP`, `OP_CMP_ZERO`, `OP_COND_SET`, `OP_JMP`, `OP_JMP_LABEL`, `OP_JE`, `OP_JNE`, `OP_JL`, `OP_JLE`, `OP_JG`, `OP_JCOND`, `OP_LABEL`
- **Calls & Frame Management**: `OP_PROLOGUE`, `OP_EPILOGUE`, `OP_CALL`, `OP_CALL_INDIRECT`, `OP_RET`, `OP_PARAM_SAVE_INT`, `OP_PARAM_SAVE_FP`, `OP_VARIADIC_SPILL`, `OP_VLA_ALLOC`, `OP_CALLEE_SAVE_STORES`, `OP_CALLEE_SAVE_LOADS`
- **C11 Atomics**: `OP_ATOMIC_XADD`, `OP_ATOMIC_CMPXCHG`, `OP_ATOMIC_CMPXCHG_BOOL`, `OP_ATOMIC_XCHG`, `OP_MFENCE`
- **Scalar Floating-Point**: `OP_FMOV_IMM`, `OP_FLOAD`, `OP_FSTORE`, `OP_FMOV`, `OP_FADD`, `OP_FSUB`, `OP_FMUL`, `OP_FDIV`, `OP_FCMP`, `OP_CVTSI2SD`, `OP_CVTTSD2SI`
- **128-Bit SIMD Vector (`float4`, `double2`, `int4`)**: `OP_VMOV`, `OP_VLOAD`, `OP_VSTORE`, `OP_VADDPS`, `OP_VSUBPS`, `OP_VMULPS`, `OP_VDIVPS`, `OP_VADDPD`, `OP_VSUBPD`, `OP_VMULPD`, `OP_VDIVPD`, `OP_VPADDD`, `OP_VPSUBD`, `OP_VPMULLD`, `OP_VSHUFPS`, `OP_VBROADCASTSS`, `OP_VHADDPS`, and scalar-lane ops (`OP_VADDSS`, `OP_VADDSD`, etc.)

---

## Debugging & Verification

```bash
# Dump IR before/after optimizations
./build/bin/zerocc -dump-ir path/to/file.c

# Emit AOT assembly (.s)
./build/bin/zerocc -S path/to/file.c -o /tmp/out.s

# Compile & run via AOT ELF and JIT (--run)
./build/bin/zerocc path/to/file.c -o /tmp/out && /tmp/out
./build/bin/zerocc --run path/to/file.c

# Run full regression suite (140/140 tests)
make test
```

## References

- [OKF Knowledge Graph Index](../../../docs/okf/index.md)
- [Two-Pass IR & Linear Scan Register Allocation](../../../docs/okf/ir-and-regalloc.md)
- [Compilation Pipeline & Two-Pass Architecture](../../../docs/okf/architecture-pipeline.md)
- [Optimization Passes & Safety Invariants](../../../docs/okf/optimization-passes.md)
- [System V AMD64 ABI & Struct Calling Conventions](../../../docs/okf/abi-sysv-x86_64.md)
- [Multi-ISA Backends (x86-64, AArch64, RISC-V)](../../../docs/okf/isa-backends.md)
