#ifndef ZERO_CC_ISA_TARGET_TRAITS_H
#define ZERO_CC_ISA_TARGET_TRAITS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "zero_cc/isa/isa_backend.h"

/*
 * Target Register File & Allocation Descriptor (Phase 2 Refactoring).
 */
typedef struct TargetRegDesc {
    uint32_t allocatable_gpr_mask;       /* Bitmask of all registers that can be allocated */
    uint32_t callee_saved_gpr_mask;      /* Bitmask of callee-saved registers */
    uint32_t reserved_gpr_mask;          /* Bitmask of permanently reserved registers */
    int      scratch_gpr1;               /* Primary backend scratch register */
    int      scratch_gpr2;               /* Secondary backend scratch register */
    int      op_scratch_gpr;             /* Extra operation scratch register (-1 if none, 7 on rv32) */
    int      fp_gpr;                     /* Frame pointer register */
    int      sp_gpr;                     /* Stack pointer register */
    int      lr_gpr;                     /* Link register (-1 if none) */
    int      num_gprs;                   /* Number of general purpose registers */
    int      num_vec_regs;               /* Number of vector registers */
    bool     vec_aliases_xmm;            /* True if vector registers share physical register file with FP */

    /* GPR allocation order pool */
    const int *alloc_pool;
    int        alloc_pool_count;

    /* GPR pair allocation pools (for 128-bit on 64-bit targets, or 64-bit on 32-bit targets) */
    const int *pair_cs_pool;
    int        pair_cs_pool_count;
    uint32_t   pair_cs_mask;
    const int *pair_gp_pool;
    int        pair_gp_pool_count;
} TargetRegDesc;

/*
 * Declarative Architecture & Platform Metrics (Phase 1-3 Refactoring).
 * Consolidates pointer sizing, word sizing, ABI calling conventions,
 * ELF container properties, DWARF debug relocation types, and register
 * descriptors into a single immutable descriptor table per ISA target.
 */
struct TargetTraits {
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
    bool      vararg_floats_in_gpr;/* true on aarch32, riscv64, riscv32 */
    uint8_t   num_int_arg_regs;    /* 6 on x86-64, 8 on aarch64/riscv, 4 on aarch32 */
    uint8_t   num_fp_arg_regs;     /* 8 on x86/aarch64/riscv, 16 on aarch32 */
    int       ret_struct_max_regs_size; /* max bytes for struct return in registers (16, 8, or 4) */
    uint16_t  elf_machine;         /* EM_X86_64(62), EM_AARCH64(183), EM_ARM(40), EM_RISCV(243) */
    uint8_t   elf_class;           /* 1 = ELFCLASS32, 2 = ELFCLASS64 */
    uint32_t  elf_flags;           /* EF_ARM_EABI_VER5, EF_RISCV_FLOAT_ABI_DOUBLE, etc. */
    uint32_t  dwarf_addr_reloc;    /* Target relocation type for pointer/address in DWARF */
    uint32_t  dwarf_sec_reloc;     /* Target relocation type for section offset in DWARF */
    TargetRegDesc regs;
};

/* Register allocation pool definitions */
static const int s_x86_alloc_pool[] = {7, 6, 2, 8, 9, 0, 3, 12, 13, 14, 15};
static const int s_x86_pair_cs_pool[] = {3, 12, 13, 14, 15};
static const int s_x86_pair_gp_pool[] = {7, 6, 8, 9, 3, 12, 13, 14, 15};

static const int s_aarch64_alloc_pool[] = {
    0, 1, 2, 3, 4, 5, 6, 7,
    11, 12, 13, 14,
    19, 20, 21, 22, 23, 24, 25, 26, 27, 28
};
static const int s_aarch64_pair_cs_pool[] = {19, 20, 21, 22, 23, 24, 25, 26, 27, 28};

static const int s_aarch32_alloc_pool[] = {0, 1, 2, 3, 4, 5, 6, 7};
static const int s_aarch32_pair_cs_pool[] = {4, 5, 6, 7};
static const int s_aarch32_pair_gp_pool[] = {0, 1, 2, 3, 4, 5, 6, 7};

static const int s_riscv_alloc_pool[] = {
    10, 11, 12, 13, 14, 15, 16, 17,
    7,
    9, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27
};
static const int s_riscv64_pair_cs_pool[] = {18, 19, 20, 21, 22, 23, 24, 25, 26, 27};
static const int s_riscv32_pair_cs_pool[] = {9, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27};

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
                .vararg_floats_in_gpr = false,
                .num_int_arg_regs = 6,
                .num_fp_arg_regs = 8,
                .ret_struct_max_regs_size = 16,
                .elf_machine = 62,  /* ELF_MACHINE_X86_64 */
                .elf_class = 2,    /* ELF_CLASS_64 */
                .elf_flags = 0,
                .dwarf_addr_reloc = 1,   /* R_X86_64_64 */
                .dwarf_sec_reloc = 10,  /* R_X86_64_32 */
                .regs = {
                    .allocatable_gpr_mask = (1u<<0)|(1u<<2)|(1u<<3)|(1u<<6)|(1u<<7)|(1u<<8)|(1u<<9)|(1u<<12)|(1u<<13)|(1u<<14)|(1u<<15),
                    .callee_saved_gpr_mask = (1u<<3)|(1u<<12)|(1u<<13)|(1u<<14)|(1u<<15),
                    .reserved_gpr_mask = (1u<<1)|(1u<<4)|(1u<<5)|(1u<<10)|(1u<<11),
                    .scratch_gpr1 = 10,
                    .scratch_gpr2 = 11,
                    .op_scratch_gpr = -1,
                    .fp_gpr = 5,
                    .sp_gpr = 4,
                    .lr_gpr = -1,
                    .num_gprs = 16,
                    .num_vec_regs = 16,
                    .vec_aliases_xmm = true,
                    .alloc_pool = s_x86_alloc_pool,
                    .alloc_pool_count = sizeof(s_x86_alloc_pool) / sizeof(s_x86_alloc_pool[0]),
                    .pair_cs_pool = s_x86_pair_cs_pool,
                    .pair_cs_pool_count = sizeof(s_x86_pair_cs_pool) / sizeof(s_x86_pair_cs_pool[0]),
                    .pair_cs_mask = (1u<<3)|(1u<<12)|(1u<<13)|(1u<<14)|(1u<<15),
                    .pair_gp_pool = s_x86_pair_gp_pool,
                    .pair_gp_pool_count = sizeof(s_x86_pair_gp_pool) / sizeof(s_x86_pair_gp_pool[0]),
                },
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
                .vararg_floats_in_gpr = false,
                .num_int_arg_regs = 8,
                .num_fp_arg_regs = 8,
                .ret_struct_max_regs_size = 16,
                .elf_machine = 183, /* ELF_MACHINE_AARCH64 */
                .elf_class = 2,    /* ELF_CLASS_64 */
                .elf_flags = 0,
                .dwarf_addr_reloc = 257, /* R_AARCH64_ABS64 */
                .dwarf_sec_reloc = 258,  /* R_AARCH64_ABS32 */
                .regs = {
                    .allocatable_gpr_mask = 0x1FF878FFu,
                    .callee_saved_gpr_mask = 0x1FF80000u,
                    .reserved_gpr_mask = 0xE0078700u,
                    .scratch_gpr1 = 9,
                    .scratch_gpr2 = 10,
                    .op_scratch_gpr = -1,
                    .fp_gpr = 29,
                    .sp_gpr = 31,
                    .lr_gpr = 30,
                    .num_gprs = 32,
                    .num_vec_regs = 32,
                    .vec_aliases_xmm = true,
                    .alloc_pool = s_aarch64_alloc_pool,
                    .alloc_pool_count = sizeof(s_aarch64_alloc_pool) / sizeof(s_aarch64_alloc_pool[0]),
                    .pair_cs_pool = s_aarch64_pair_cs_pool,
                    .pair_cs_pool_count = sizeof(s_aarch64_pair_cs_pool) / sizeof(s_aarch64_pair_cs_pool[0]),
                    .pair_cs_mask = 0x1FF80000u,
                    .pair_gp_pool = NULL,
                    .pair_gp_pool_count = 0,
                },
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
                .vararg_floats_in_gpr = true,
                .num_int_arg_regs = 4,
                .num_fp_arg_regs = 16,
                .ret_struct_max_regs_size = 4,
                .elf_machine = 40,  /* ELF_MACHINE_ARM */
                .elf_class = 1,    /* ELF_CLASS_32 */
                .elf_flags = 0x05000000u, /* ZC_EF_ARM_EABI_VER5 */
                .dwarf_addr_reloc = 2,   /* R_ARM_ABS32 */
                .dwarf_sec_reloc = 2,    /* R_ARM_ABS32 */
                .regs = {
                    .allocatable_gpr_mask = 0x000000FFu,
                    .callee_saved_gpr_mask = 0x000000F0u,
                    .reserved_gpr_mask = 0xFFFFFF00u,
                    .scratch_gpr1 = 12,
                    .scratch_gpr2 = 9,
                    .op_scratch_gpr = -1,
                    .fp_gpr = 11,
                    .sp_gpr = 13,
                    .lr_gpr = 14,
                    .num_gprs = 16,
                    .num_vec_regs = 16,
                    .vec_aliases_xmm = true,
                    .alloc_pool = s_aarch32_alloc_pool,
                    .alloc_pool_count = sizeof(s_aarch32_alloc_pool) / sizeof(s_aarch32_alloc_pool[0]),
                    .pair_cs_pool = s_aarch32_pair_cs_pool,
                    .pair_cs_pool_count = sizeof(s_aarch32_pair_cs_pool) / sizeof(s_aarch32_pair_cs_pool[0]),
                    .pair_cs_mask = 0x000000F0u,
                    .pair_gp_pool = s_aarch32_pair_gp_pool,
                    .pair_gp_pool_count = sizeof(s_aarch32_pair_gp_pool) / sizeof(s_aarch32_pair_gp_pool[0]),
                },
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
                .vararg_floats_in_gpr = true,
                .num_int_arg_regs = 8,
                .num_fp_arg_regs = 8,
                .ret_struct_max_regs_size = 16,
                .elf_machine = 243, /* ELF_MACHINE_RISCV */
                .elf_class = 2,    /* ELF_CLASS_64 */
                .elf_flags = 4,     /* EF_RISCV_FLOAT_ABI_DOUBLE */
                .dwarf_addr_reloc = 2,   /* R_RISCV_64 */
                .dwarf_sec_reloc = 1,    /* R_RISCV_32 */
                .regs = {
                    .allocatable_gpr_mask = (1u<<7) | (0xFFu<<10) | (1u<<9) | (0x3FFu<<18),
                    .callee_saved_gpr_mask = (1u<<9) | (0x3FFu<<18),
                    .reserved_gpr_mask = (1u<<0)|(1u<<1)|(1u<<2)|(1u<<3)|(1u<<4)|(1u<<5)|(1u<<6)|(1u<<8)|(1u<<30),
                    .scratch_gpr1 = 5,
                    .scratch_gpr2 = 6,
                    .op_scratch_gpr = -1,
                    .fp_gpr = 8,
                    .sp_gpr = 2,
                    .lr_gpr = 1,
                    .num_gprs = 32,
                    .num_vec_regs = 32,
                    .vec_aliases_xmm = false,
                    .alloc_pool = s_riscv_alloc_pool,
                    .alloc_pool_count = sizeof(s_riscv_alloc_pool) / sizeof(s_riscv_alloc_pool[0]),
                    .pair_cs_pool = s_riscv64_pair_cs_pool,
                    .pair_cs_pool_count = sizeof(s_riscv64_pair_cs_pool) / sizeof(s_riscv64_pair_cs_pool[0]),
                    .pair_cs_mask = (0x3FFu<<18),
                    .pair_gp_pool = NULL,
                    .pair_gp_pool_count = 0,
                },
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
                .vararg_floats_in_gpr = true,
                .num_int_arg_regs = 8,
                .num_fp_arg_regs = 8,
                .ret_struct_max_regs_size = 8,
                .elf_machine = 243, /* ELF_MACHINE_RISCV */
                .elf_class = 1,    /* ELF_CLASS_32 */
                .elf_flags = 0,
                .dwarf_addr_reloc = 1,   /* R_RISCV_32 */
                .dwarf_sec_reloc = 1,    /* R_RISCV_32 */
                .regs = {
                    .allocatable_gpr_mask = (1u<<7) | (0xFFu<<10) | (1u<<9) | (0x3FFu<<18),
                    .callee_saved_gpr_mask = (1u<<9) | (0x3FFu<<18),
                    .reserved_gpr_mask = (1u<<0)|(1u<<1)|(1u<<2)|(1u<<3)|(1u<<4)|(1u<<5)|(1u<<6)|(1u<<8)|(1u<<30),
                    .scratch_gpr1 = 5,
                    .scratch_gpr2 = 6,
                    .op_scratch_gpr = 7,
                    .fp_gpr = 8,
                    .sp_gpr = 2,
                    .lr_gpr = 1,
                    .num_gprs = 32,
                    .num_vec_regs = 32,
                    .vec_aliases_xmm = false,
                    .alloc_pool = s_riscv_alloc_pool,
                    .alloc_pool_count = sizeof(s_riscv_alloc_pool) / sizeof(s_riscv_alloc_pool[0]),
                    .pair_cs_pool = s_riscv32_pair_cs_pool,
                    .pair_cs_pool_count = sizeof(s_riscv32_pair_cs_pool) / sizeof(s_riscv32_pair_cs_pool[0]),
                    .pair_cs_mask = (1u<<9) | (0x3FFu<<18),
                    .pair_gp_pool = NULL,
                    .pair_gp_pool_count = 0,
                },
            };
            return &t;
        }
        default:
            return zero_cc_get_target_traits(ISA_X86_64);
    }
}

#endif /* ZERO_CC_ISA_TARGET_TRAITS_H */
