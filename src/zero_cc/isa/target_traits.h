#ifndef ZERO_CC_ISA_TARGET_TRAITS_H
#define ZERO_CC_ISA_TARGET_TRAITS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "zero_cc/isa/isa_backend.h"

/*
 * Declarative Architecture & Platform Metrics (Phase 1 Refactoring).
 * Consolidates pointer sizing, word sizing, ABI calling conventions,
 * ELF container properties, and DWARF debug relocation types into a
 * single immutable descriptor table per ISA target.
 */
typedef struct TargetTraits {
    ZeroCCISA isa;
    const char *name;              /* "x86-64", "aarch64", "aarch32", "riscv64", "riscv32" */
    uint8_t   ptr_size;            /* 4 or 8 */
    uint8_t   word_size;           /* 4 or 8 (GPR width) */
    uint8_t   slot_size;           /* 4 or 8 (stack slot width) */
    uint8_t   stack_align;         /* 16 or 8 (ABI stack boundary alignment) */
    bool      is_32bit;            /* true if ptr_size == 4 */
    bool      has_red_zone;        /* true on x86-64 (128 bytes below rsp) */
    bool      has_flags_register;  /* true on x86-64, aarch64, aarch32 */
    int       ret_gpr_lo;          /* primary integer return GPR (e.g. rax=0, x0=0, a0=10, r0=0) */
    int       ret_gpr_hi;          /* secondary return GPR for 64-bit pair or 9-16B struct (rdx=2, x1=1, a1=11, r1=1) */
    int       sret_gpr;            /* hidden sret pointer argument register (rdi=5, x8=8, a0=10, r0=0) */
    bool      sret_consumes_arg0;  /* false on aarch64 (dedicated x8), true on x86/RISC-V/ARM */
    uint16_t  elf_machine;         /* EM_X86_64(62), EM_AARCH64(183), EM_ARM(40), EM_RISCV(243) */
    uint8_t   elf_class;           /* 1 = ELFCLASS32, 2 = ELFCLASS64 */
    uint32_t  elf_flags;           /* EF_ARM_EABI_VER5, EF_RISCV_FLOAT_ABI_DOUBLE, etc. */
    uint32_t  dwarf_addr_reloc;    /* Target relocation type for pointer/address in DWARF */
    uint32_t  dwarf_sec_reloc;     /* Target relocation type for section offset in DWARF */
} TargetTraits;

/* Get immutable target traits for a given ISA */
static inline const TargetTraits *zero_cc_get_target_traits(ZeroCCISA isa) {
    switch (isa) {
        case ISA_X86_64: {
            static const TargetTraits t = {
                .isa = ISA_X86_64,
                .name = "x86-64",
                .ptr_size = 8,
                .word_size = 8,
                .slot_size = 8,
                .stack_align = 16,
                .is_32bit = false,
                .has_red_zone = true,
                .has_flags_register = true,
                .ret_gpr_lo = 0,    /* RAX */
                .ret_gpr_hi = 2,    /* RDX */
                .sret_gpr = 5,      /* RDI */
                .sret_consumes_arg0 = true,
                .elf_machine = 62,  /* ELF_MACHINE_X86_64 */
                .elf_class = 2,    /* ELF_CLASS_64 */
                .elf_flags = 0,
                .dwarf_addr_reloc = 1,   /* R_X86_64_64 */
                .dwarf_sec_reloc = 10,  /* R_X86_64_32 */
            };
            return &t;
        }
        case ISA_AARCH64: {
            static const TargetTraits t = {
                .isa = ISA_AARCH64,
                .name = "aarch64",
                .ptr_size = 8,
                .word_size = 8,
                .slot_size = 8,
                .stack_align = 16,
                .is_32bit = false,
                .has_red_zone = false,
                .has_flags_register = true,
                .ret_gpr_lo = 0,    /* X0 */
                .ret_gpr_hi = 1,    /* X1 */
                .sret_gpr = 8,      /* X8 */
                .sret_consumes_arg0 = false,
                .elf_machine = 183, /* ELF_MACHINE_AARCH64 */
                .elf_class = 2,    /* ELF_CLASS_64 */
                .elf_flags = 0,
                .dwarf_addr_reloc = 257, /* R_AARCH64_ABS64 */
                .dwarf_sec_reloc = 258,  /* R_AARCH64_ABS32 */
            };
            return &t;
        }
        case ISA_AARCH32: {
            static const TargetTraits t = {
                .isa = ISA_AARCH32,
                .name = "aarch32",
                .ptr_size = 4,
                .word_size = 4,
                .slot_size = 4,
                .stack_align = 8,
                .is_32bit = true,
                .has_red_zone = false,
                .has_flags_register = true,
                .ret_gpr_lo = 0,    /* R0 */
                .ret_gpr_hi = 1,    /* R1 */
                .sret_gpr = 0,      /* R0 */
                .sret_consumes_arg0 = true,
                .elf_machine = 40,  /* ELF_MACHINE_ARM */
                .elf_class = 1,    /* ELF_CLASS_32 */
                .elf_flags = 0x05000000u, /* ZC_EF_ARM_EABI_VER5 */
                .dwarf_addr_reloc = 2,   /* R_ARM_ABS32 */
                .dwarf_sec_reloc = 2,    /* R_ARM_ABS32 */
            };
            return &t;
        }
        case ISA_RISCV64: {
            static const TargetTraits t = {
                .isa = ISA_RISCV64,
                .name = "riscv64",
                .ptr_size = 8,
                .word_size = 8,
                .slot_size = 8,
                .stack_align = 16,
                .is_32bit = false,
                .has_red_zone = false,
                .has_flags_register = false,
                .ret_gpr_lo = 10,   /* A0 */
                .ret_gpr_hi = 11,   /* A1 */
                .sret_gpr = 10,     /* A0 */
                .sret_consumes_arg0 = true,
                .elf_machine = 243, /* ELF_MACHINE_RISCV */
                .elf_class = 2,    /* ELF_CLASS_64 */
                .elf_flags = 4,     /* EF_RISCV_FLOAT_ABI_DOUBLE */
                .dwarf_addr_reloc = 2,   /* R_RISCV_64 */
                .dwarf_sec_reloc = 1,    /* R_RISCV_32 */
            };
            return &t;
        }
        case ISA_RISCV32: {
            static const TargetTraits t = {
                .isa = ISA_RISCV32,
                .name = "riscv32",
                .ptr_size = 4,
                .word_size = 4,
                .slot_size = 4,
                .stack_align = 16,
                .is_32bit = true,
                .has_red_zone = false,
                .has_flags_register = false,
                .ret_gpr_lo = 10,   /* A0 */
                .ret_gpr_hi = 11,   /* A1 */
                .sret_gpr = 10,     /* A0 */
                .sret_consumes_arg0 = true,
                .elf_machine = 243, /* ELF_MACHINE_RISCV */
                .elf_class = 1,    /* ELF_CLASS_32 */
                .elf_flags = 0,
                .dwarf_addr_reloc = 1,   /* R_RISCV_32 */
                .dwarf_sec_reloc = 1,    /* R_RISCV_32 */
            };
            return &t;
        }
        default:
            return zero_cc_get_target_traits(ISA_X86_64);
    }
}

/* Forward declaration for accessors */
#include "zero_cc/types/symbols.h"

static inline const TargetTraits *zc_target_traits(const ZeroCCCompiler *c) {
    if (!c) return zero_cc_get_target_traits(ISA_X86_64);
    if (c->isa_backend && c->isa_backend->traits) return c->isa_backend->traits;
    return zero_cc_get_target_traits(c->isa);
}

static inline bool zc_target_is_32bit(const ZeroCCCompiler *c) {
    return zc_target_traits(c)->is_32bit;
}

static inline int zc_target_ptr_size(const ZeroCCCompiler *c) {
    return (int)zc_target_traits(c)->ptr_size;
}

static inline int zc_target_slot_size(const ZeroCCCompiler *c) {
    return (int)zc_target_traits(c)->slot_size;
}

#endif /* ZERO_CC_ISA_TARGET_TRAITS_H */
