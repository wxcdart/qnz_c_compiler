---
type: Concept
title: Multi-ISA Target Architecture & Modular Backend Roadmap
description: Architectural roadmap for decoupling ZeroCC's multi-ISA backends via TargetTraits, table-driven regalloc, modular linker ops, baremetal CPU support, and decoupled GPU toolchains.
resource: backend/target-abstraction-roadmap
tags: [multi-isa, target-traits, architecture, roadmap, baremetal, modularity, refactoring]
timestamp: 2026-11-18T10:30:13Z
generated: antigravity-agent
verified: engineering-lead
status: active
---

# Multi-ISA Target Architecture & Modular Backend Roadmap

> **Status (Sprint 186):** **Phases 1-5 and Baremetal CPU Scope Complete & Verified**.
> - **Phase 1 (TargetTraits)**: Declarative architecture metrics (`ptr_size`, `word_size`, `stack_align`, `has_red_zone`, `ret_gpr_lo/hi`, `sret_gpr`, `sret_consumes_arg0`, `elf_machine`, `elf_flags`, `dwarf_addr_reloc`). Eliminates ~50 scattered `(c->isa == ...)` checks across AST lowering, `main.c`, `dwarf.c`, and `elf_writer.c`.
> - **Phase 2 (Table-Driven Regalloc)**: Completed. Declarative `TargetRegDesc` bitmasks (`allocatable_gpr_mask`, `callee_saved_gpr_mask`, `reserved_gpr_mask`), scratch registers, and register pools (`alloc_pool`, `pair_cs_pool`, `pair_gp_pool`) in `src/zero_cc/isa/target_traits.h` and `src/zero_cc/ir/regalloc.h`.
> - **Phase 3 (Decoupled ABI Calling Conventions)**: Completed. `abi_classify_arg` and `abi_classify_return` in `src/zero_cc/ir/abi_classify.h`, with `sret_consumes_arg0` and `vararg_floats_in_gpr` declarative traits in `ast_lowering_call.c` and `ast_lowering_varargs.c`.
> - **Phase 4 (Modular Linker Relocation Ops)**: Completed. `TargetLinkerOps` (`apply_reloc`, `emit_start_stub`, `default_text_base`, `default_elf_flags`, `plt_stub_size`) in `src/zero_cc/linker/linker_reloc.h` and `src/zero_cc/linker/linker.c`. Replaced ~260 lines of cascading architecture relocation code.
> - **Phase 5 (Clean ASM Emitter Separation)**: Completed. Added `emit_load_sized_asm`, `emit_store_sized_asm`, `emit_test_zero_asm`, and `emit_cmp_regs_asm` in `src/zero_cc/ir/asm_emitter.c`. Replaced redundant per-ISA switch blocks in memory load/store and conditional branch opcodes.
> - **Baremetal CPU Scope**: Completed & Verified. Added linker script parser (`src/zero_cc/linker/linker_script.h`) supporting `MEMORY { FLASH, RAM }`, `ORIGIN`, `LENGTH`, `ENTRY`, and `_stack_top`. Implemented LMA vs VMA separation in `src/zero_cc/linker/linker.c` with correct ELF `p_paddr` vs `p_vaddr` Program Headers, boundary symbols (`_sidata`, `_sdata`, `_edata`, `_sbss`, `_ebss`, `_stack_top`), and freestanding CLI driver flags (`--baremetal`, `-ffreestanding`, `-T`, `-nostdlib`). Verified across all 5 architectures via `tools/verify/baremetal_test.py` (`make test-baremetal`).
> - **GPU Scope (`zero_gpu`)**: *Decoupled Sister Project*. Standalone toolchain for SSA, SIMT divergence, multiple address spaces, and PTX / SPIR-V bytecode.

---

## 1. Overview & Architectural Motivation

ZeroCC supports 5 target configurations across 4 instruction set families (x86-64, AArch64, RV64GC, RV32GC, and AArch32). However, historical evolution led to tight coupling:
1. **Scattered ISA Checks**: Over 80+ ad-hoc `if (c->isa == ...)` checks exist across AST lowering (`ast_lowering_*.c`), `asm_emitter.c`, register allocation (`regalloc.h`), DWARF emission (`dwarf.c`), and linker (`linker.c`).
2. **150+ Slot Backend Vtable**: `ZeroCCISABackend` combines MC binary emitters, assembly emitters, calling conventions, and architecture-specific operations (e.g. `emit_cqo`, `emit_add64`, 40+ vector callbacks) without a capability query mechanism.
3. **High Integration Overhead**: Integrating a new architecture (such as AArch32) previously required touching 15+ files across all compiler phases.

---

## 2. Baremetal CPUs vs. GPUs Scope Boundary

### A. Baremetal CPUs (In-Tree Scope)
Baremetal CPUs (Cortex-M, RV32E, Xtensa, MIPS) share ZeroCC's core computational model (Von Neumann / Harvard architecture, standard GPR register files, linear instruction streams, stack frames, and standard ELF relocations):
- **Requirements**:
  - **LMA vs. VMA**: Separate load address (ROM/Flash) from runtime virtual address (RAM) in `src/zero_cc/linker/linker.c`.
  - **Linker Script Support (`-T script.ld`)**: Declarative memory regions (`MEMORY { FLASH: ..., RAM: ... }`).
  - **Freestanding Crt0**: Reset handler to initialize `.data`, zero `.bss`, configure initial stack pointer, and branch to `main`.
  - **Target Environment Trait**: `OS_LINUX` vs. `OS_BAREMETAL` to bypass POSIX syscall shims in `zero_rt`.

### B. GPUs (Decoupled Sister Project `zero_gpu`)
GPU compilation (NVIDIA PTX, Vulkan/OpenCL SPIR-V) violates the fundamental assumptions of ZeroCC's pipeline:
- **Execution Model**: SIMT/SPMD (warps, workgroups, divergence masks) vs. CPU SISD/MIMD.
- **IR & Register Model**: PTX uses infinite virtual registers; SPIR-V uses pure SSA values. ZeroCC's linear-scan physical register allocator is inapplicable.
- **Memory Address Spaces**: Multiple distinct address spaces (`__global`, `__shared`, `__constant`, `__private`) requiring address-space qualifiers in types and memory opcodes.
- **Output**: UTF-8 text (PTX) or 32-bit binary words (SPIR-V) rather than ELF objects or native executables.
- **Verdict**: Maintained as an independent sister project (`zero_gpu`) sharing the lexer/preprocessor and utility libraries, keeping ZeroCC focused on high-speed CPU compilation.

---

## 3. The 5-Phase Refactoring Implementation

### Phase 1: Declarative `TargetTraits` (`src/zero_cc/isa/target_traits.h`)
Consolidates target properties into a static table indexed by `ZeroCCISA`:
```c
typedef struct TargetTraits {
    ZeroCCISA isa;
    uint8_t   ptr_size;            /* 4 or 8 */
    uint8_t   word_size;           /* 4 or 8 */
    uint8_t   stack_align;         /* 16 or 8 or 4 */
    bool      has_red_zone;        /* true on x86-64 */
    int       ret_gpr_lo;          /* r0, a0, rax */
    int       ret_gpr_hi;          /* r1, a1, rdx */
    int       sret_gpr;            /* r0, a0, rdi, x8 */
    bool      sret_consumes_arg0;  /* false on aarch64 (x8) */
    uint16_t  elf_machine;         /* EM_X86_64, EM_AARCH64, EM_ARM, EM_RISCV */
    uint32_t  elf_flags;           /* EF_ARM_EABI_VER5, EF_RISCV_FLOAT_ABI_DOUBLE */
    uint32_t  dwarf_addr_reloc;    /* R_X86_64_64, R_ARM_ABS32, etc. */
} TargetTraits;

const TargetTraits *zero_cc_get_target_traits(ZeroCCISA isa);
```

### Phase 2: Table-Driven Register Allocator (`TargetRegDesc`)
Decouples `regalloc.h` by initializing register availability from declarative target bitmasks (`allocatable_gpr_mask`, `callee_saved_gpr_mask`, `reserved_gpr_mask`, scratch registers, vector register counts).

### Phase 3: Decoupled ABI Calling Conventions (`TargetABIOps`)
Introduces clean ABI hooks (`classify_arg`, `classify_return`) to separate platform calling convention rules from AST lowering.

### Phase 4: Modular Linker Relocation Ops (`TargetLinkerOps`)
Refactors `apply_relocations()` and PLT generation out of `linker.c` into target-specific relocation engines (`src/zero_cc/linker/arch_*.c`).

### Phase 5: Clean ASM Emitter Separation
Eliminates hardcoded `c->isa` branches in `asm_emitter.c` by ensuring complete hook coverage or declarative format tables.
