---
type: Architecture
title: Multi-ISA Backends (x86-64, AArch64, RISC-V) & Linker/JIT
description: Backend vtable interface (ZeroCCISABackend), JIT machine code emission, AOT assembly generation, parallel argument moves, and symbol relocation resolution.
resource: backend/isa-backends
tags: [backend, x86_64, aarch64, riscv, jit, aot, elf, relocations]
timestamp: 2026-10-16T04:32:18Z
generated: antigravity-agent
verified: engineering-lead
status: active
---

# Multi-ISA Backends (x86-64, AArch64, RISC-V) & Linker/JIT

ZeroCC abstracts target-specific code generation behind `ZeroCCISABackend` (`src/zero_cc/isa/isa_backend.h`), supporting three architectures:
- **x86-64**: `src/zero_cc/isa/x86_64.c` (JIT binary emission) & `src/zero_cc/isa/x86_64_asm.c` (AOT `.s` assembly emission)
- **AArch64**: `src/zero_cc/isa/aarch64.c` & `src/zero_cc/isa/aarch64_asm.c`
- **RISC-V (RV64GC)**: `src/zero_cc/isa/riscv.c` & `src/zero_cc/isa/riscv_asm.c`

## Lazy Backend Initialization

Backend structs (`x86_64_backend`, `aarch64_backend`, `riscv64_backend`) are non-`const` and lazily wired with their assembly emitter callbacks via `zero_cc_init_backends_once()` inside `zero_cc_get_backend()` and `zero_cc_get_native_backend()`.

## Multi-ISA `sret` (>16B Struct Return) Conventions

Across target ISAs, structs exceeding 16 bytes return via an implicit caller-allocated buffer pointer:
- **x86-64 (System V AMD64 ABI)**:
  - Caller allocates stack buffer and passes its pointer in `%rdi`.
  - Regular integer arguments shift starting from `%rsi` (index 1).
  - Callee writes struct to `[%rdi]` and returns the buffer pointer in `%rax`.
- **AArch64 (ARM64 AAPCS64 ABI)**:
  - Caller allocates buffer and passes its pointer in `x8` (dedicated indirect result location register).
  - Regular arguments start at `x0` without register consumption/shifting.
  - Callee preserves `x8` in its prologue (`param_save_int_asm(8, sret_offset)`) and returns the pointer in `x0`.
- **RISC-V (RV64GC LP64D ABI)**:
  - Caller allocates buffer and passes its pointer in `a0` (first integer argument register).
  - Regular integer arguments shift starting from `a1` (index 1).
  - Callee writes struct to caller buffer and returns pointer in `a0`.

## Parallel Argument Setup in Function Calls

When setting up arguments for `OP_CALL`, source virtual registers may reside in physical registers that overlap with destination ABI argument registers (e.g., arg 1 currently in `%rdi` while `%rdi` must receive the `sret` pointer).
- **AOT (`x86_emit_call_asm` in `x86_64_asm.c`)**:
  1. If the call is indirect (`fn_name == NULL`), moves the callee address register into `%r11` first.
  2. Pushes all integer argument source registers onto the stack in reverse order (`n_int - 1` down to `0`), then pops them into their target ABI registers (`%rdi`, `%rsi`, `%rdx`, `%rcx`, `%r8`, `%r9`).
  3. Emits `call fn_name` or `call *%r11`.
- **JIT (`OP_CALL` in `ast_lowering_mc.c`)**:
  1. If the call is indirect (`fn_name == NULL`), copies the callee address register into `BE(c)->scratch_reg2` (`%r11`) before touching argument registers.
  2. Uses push/pop or register moves for integer/FP arguments, then emits `BE(c)->emit_call_reg(&c->code_buf, BE(c)->scratch_reg2)`.

## Symbol & Function Address Resolution (`OP_MOV_GLOBAL`)

When taking the address of a function (`fn` or `&fn` assigned to a function pointer), `ast_lowering_expr.c` emits `ir_emit_mov_global(temp, fn_name)`:
- **AOT Assembly (`asm_emitter.c`)**:
  - Checks `zero_cc_find_function(c, sym_name)`. If it is a function symbol, emits `leaq sym(%rip), %reg` to load the function's code address (whereas global data variables emit `movq var(%rip), %reg` to load the variable's value).
- **AOT ELF Object / Executable (`src/main.c`)**:
  - Resolves `c->global_relocs[i]` against both `c->globals` (`.data`) and `c->functions` (`.text`) so internal function pointers relocate to their `.text` offset.
- **JIT Execution (`src/zero_cc/jit/execution.h`)**:
  - Resolves `c->global_relocs[i]` by checking `c->globals`, then `zero_cc_find_function(c, sym_name)` (computing `(void*)(c->code_buf.data + fn->code_offset)`), and finally `zero_cc_resolve_symbol(sym_name)` (`dlsym`).

## Multi-ISA SIMD / Vector Emission (SSE, NEON, RVV 1.0)

When loop vectorization emits packed vector opcodes (`OP_VLOAD`, `OP_VSTORE`, `OP_VADDPS`, `OP_VSUBPS`, `OP_VMULPS`, `OP_VDIVPS`):
- **x86-64 (SSE/AVX)**:
  - Vector registers `xmm0`–`xmm15`.
  - Memory moves: `movups (%rsi), %xmm0` / `movups %xmm0, (%rdi)`.
  - Vector arithmetic: `addps %xmm1, %xmm0`, `subps`, `mulps`, `divps`.
- **AArch64 (NEON)**:
  - 128-bit SIMD registers `v0`–`v31` (referenced as `q0`–`q31` for 128-bit vector loads/stores and `s0`–`s31` for scalar singles).
  - Memory moves: `ldr q0, [x1]` / `str q0, [x0]`.
  - Vector arithmetic: `fadd v0.4s, v0.4s, v1.4s`, `fsub`, `fmul`, `fdiv`.
- **RISC-V (RVV 1.0 `rv64gcv`)**:
  - Vector registers `v0`–`v31` with explicit vector length/type configuration.
  - Configuration: `vsetivli zero, 4, e32, m1, ta, ma` (sets vector length to 4 elements of 32-bit floats, LMUL=1).
  - Memory moves: `vle32.v v0, 0(a1)` / `vse32.v v0, 0(a0)`.
  - Vector arithmetic: `vfadd.vv vd, vs2, vs1` (standard GNU assembler format: `vfadd.vv v0, v0, v1`).
  - Scalar float operations continue using standard RISC-V FP registers (`fa0`–`fa7`, `ft0`–`ft11`) via `flw`/`fsw`/`fadd.s`.

## Related Concepts
- Return to [Root Index](./index.md)
- See [System V AMD64 ABI & Struct Calling Conventions](./abi-sysv-x86_64.md)
- See [Two-Pass IR & Register Allocation](./ir-and-regalloc.md)
- See [Optimization Passes & Safety Invariants](./optimization-passes.md)
