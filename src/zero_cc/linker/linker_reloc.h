#ifndef ZERO_CC_LINKER_RELOC_H
#define ZERO_CC_LINKER_RELOC_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* Forward declarations */
typedef struct ZcLink ZcLink;

typedef struct ZcRiscvPcrel {
    uint64_t p_hi;
    int64_t delta;
} ZcRiscvPcrel;

typedef struct TargetLinkerOps {
    uint16_t machine;
    const char *name;
    uint64_t default_text_base;
    uint32_t default_elf_flags;
    size_t   plt_stub_size;
    int (*apply_reloc)(ZcLink *L, uint8_t *site, uint32_t type,
                       uint64_t P, int64_t S, int64_t A,
                       const char *sym_name, const char *obj_path,
                       const char *sec_name,
                       const ZcRiscvPcrel *pcrel_tab, size_t pcrel_count,
                       char *err, size_t errsz);
    void (*emit_start_stub)(uint8_t *st, uint64_t text_vma, uint64_t main_vma);
    int (*reserve_thunks)(ZcLink *L, char *err, size_t errsz);
    int (*precollect_pcrel)(ZcLink *L, ZcRiscvPcrel **tab_out, size_t *cnt_out, size_t *cap_out);
} TargetLinkerOps;

/* AArch64 Relocation Handler */
static inline int linker_reloc_aarch64(ZcLink *L, uint8_t *site, uint32_t type,
                                      uint64_t P, int64_t S, int64_t A,
                                      const char *sym_name, const char *obj_path,
                                      const char *sec_name,
                                      const ZcRiscvPcrel *pcrel_tab, size_t pcrel_count,
                                      char *err, size_t errsz) {
    (void)L; (void)sec_name; (void)pcrel_tab; (void)pcrel_count;
    uint32_t w = (uint32_t)site[0] | ((uint32_t)site[1] << 8) |
                 ((uint32_t)site[2] << 16) | ((uint32_t)site[3] << 24);
    uint32_t nw = w;
    switch (type) {
    case 257: /* R_AARCH64_ABS64 */
        for (int b = 0; b < 8; b++)
            site[b] = (uint8_t)((uint64_t)(S + A) >> (8 * b));
        return 0;
    case 256: /* R_AARCH64_ABS32 */
        for (int b = 0; b < 4; b++)
            site[b] = (uint8_t)((uint32_t)(uint64_t)(S + A) >> (8 * b));
        return 0;
    case 275: /* R_AARCH64_ADR_PREL_PG_HI21 */
    {
        int64_t r = ((int64_t)(((uint64_t)(S + A) & ~0xFFFULL) - (P & ~0xFFFULL))) >> 12;
        if (r < -(1LL << 20) || r >= (1LL << 20)) {
            snprintf(err, errsz,
                     "ADR_PREL_PG_HI21 out of range for `%s' at 0x%lx in %s",
                     sym_name, (unsigned long)P, obj_path);
            return -1;
        }
        uint32_t imm21 = (uint32_t)r & 0x1FFFFF;
        uint32_t patch = ((imm21 & 0x1FFFFC) << 3) | ((imm21 & 3) << 29);
        nw = (w & ~0x60FFFFE0U) | patch;
        break;
    }
    case 277: /* R_AARCH64_ADD_ABS_LO12_NC */
    {
        uint32_t lo = (uint32_t)((S + A) & 0xFFF);
        nw = (w & ~0x003FFC00U) | (lo << 10);
        break;
    }
    case 283: /* R_AARCH64_CALL26 */
    {
        int64_t d = S + A - (int64_t)P;
        if ((d & 3) != 0 || d < -(1LL << 27) || d >= (1LL << 27)) {
            snprintf(err, errsz,
                     "CALL26 out of range for `%s' at 0x%lx in %s",
                     sym_name, (unsigned long)P, obj_path);
            return -1;
        }
        nw = (w & ~0x3FFFFFFU) | (uint32_t)((d >> 2) & 0x3FFFFFF);
        break;
    }
    default:
        snprintf(err, errsz,
                 "unsupported AArch64 relocation type %u in %s (%s)",
                 type, sec_name, obj_path);
        return -1;
    }
    site[0] = (uint8_t)nw;
    site[1] = (uint8_t)(nw >> 8);
    site[2] = (uint8_t)(nw >> 16);
    site[3] = (uint8_t)(nw >> 24);
    return 0;
}

/* ARM (AArch32) Relocation Handler */
static inline int linker_reloc_arm(ZcLink *L, uint8_t *site, uint32_t type,
                                  uint64_t P, int64_t S, int64_t A,
                                  const char *sym_name, const char *obj_path,
                                  const char *sec_name,
                                  const ZcRiscvPcrel *pcrel_tab, size_t pcrel_count,
                                  char *err, size_t errsz) {
    (void)L; (void)sec_name; (void)pcrel_tab; (void)pcrel_count;
    uint32_t w = (uint32_t)site[0] | ((uint32_t)site[1] << 8) |
                 ((uint32_t)site[2] << 16) | ((uint32_t)site[3] << 24);
    uint32_t nw = w;
    switch (type) {
    case 0:  /* R_ARM_NONE */
    case 40: /* R_ARM_V4BX */
        return 0;
    case 2:  /* R_ARM_ABS32 */
        for (int b = 0; b < 4; b++)
            site[b] = (uint8_t)((uint32_t)(uint64_t)(S + A) >> (8 * b));
        return 0;
    case 3:  /* R_ARM_REL32 */
    {
        uint32_t val = (uint32_t)(int32_t)((S + A) - (int64_t)P);
        for (int b = 0; b < 4; b++)
            site[b] = (uint8_t)(val >> (8 * b));
        return 0;
    }
    case 1:  /* R_ARM_PC24 */
    case 28: /* R_ARM_CALL */
    case 29: /* R_ARM_JUMP24 */
    {
        int64_t d = (S + A) - (int64_t)(P + 8);
        if ((d & 3) != 0 || d < -(1LL << 25) || d >= (1LL << 25)) {
            snprintf(err, errsz,
                     "R_ARM_CALL out of range for `%s' at 0x%lx in %s",
                     sym_name, (unsigned long)P, obj_path);
            return -1;
        }
        nw = (w & 0xFF000000u) | (uint32_t)((d >> 2) & 0x00FFFFFFu);
        break;
    }
    case 43: /* R_ARM_MOVW_ABS_NC */
    {
        uint32_t v16 = (uint32_t)(S + A) & 0xFFFFu;
        nw = (w & ~0x000F0FFFu) | (((v16 >> 12) & 0xFu) << 16) | (v16 & 0xFFFu);
        break;
    }
    case 44: /* R_ARM_MOVT_ABS */
    {
        uint32_t v16 = ((uint32_t)(S + A) >> 16) & 0xFFFFu;
        nw = (w & ~0x000F0FFFu) | (((v16 >> 12) & 0xFu) << 16) | (v16 & 0xFFFu);
        break;
    }
    case 47: /* R_ARM_THM_MOVW_ABS_NC */
    {
        uint16_t hw1 = (uint16_t)site[0] | ((uint16_t)site[1] << 8);
        uint16_t hw2 = (uint16_t)site[2] | ((uint16_t)site[3] << 8);
        uint32_t val = (uint32_t)(S + A) & 0xFFFFu;
        uint32_t imm4 = (val >> 12) & 0xFu;
        uint32_t i = (val >> 11) & 0x1u;
        uint32_t imm3 = (val >> 8) & 0x7u;
        uint32_t imm8 = val & 0xFFu;
        hw1 = (uint16_t)((hw1 & ~0x040Fu) | (i << 10) | imm4);
        hw2 = (uint16_t)((hw2 & ~0x70FFu) | (imm3 << 12) | imm8);
        site[0] = (uint8_t)hw1; site[1] = (uint8_t)(hw1 >> 8);
        site[2] = (uint8_t)hw2; site[3] = (uint8_t)(hw2 >> 8);
        return 0;
    }
    case 48: /* R_ARM_THM_MOVT_ABS */
    {
        uint16_t hw1 = (uint16_t)site[0] | ((uint16_t)site[1] << 8);
        uint16_t hw2 = (uint16_t)site[2] | ((uint16_t)site[3] << 8);
        uint32_t val = ((uint32_t)(S + A) >> 16) & 0xFFFFu;
        uint32_t imm4 = (val >> 12) & 0xFu;
        uint32_t i = (val >> 11) & 0x1u;
        uint32_t imm3 = (val >> 8) & 0x7u;
        uint32_t imm8 = val & 0xFFu;
        hw1 = (uint16_t)((hw1 & ~0x040Fu) | (i << 10) | imm4);
        hw2 = (uint16_t)((hw2 & ~0x70FFu) | (imm3 << 12) | imm8);
        site[0] = (uint8_t)hw1; site[1] = (uint8_t)(hw1 >> 8);
        site[2] = (uint8_t)hw2; site[3] = (uint8_t)(hw2 >> 8);
        return 0;
    }
    case 10: /* R_ARM_THM_CALL */
    case 30: /* R_ARM_THM_JUMP24 */
    {
        uint16_t hw1 = (uint16_t)site[0] | ((uint16_t)site[1] << 8);
        uint16_t hw2 = (uint16_t)site[2] | ((uint16_t)site[3] << 8);
        int64_t d = (S + A) - (int64_t)(P + 4);
        uint32_t S_bit = (uint32_t)((d >> 24) & 1);
        uint32_t I1 = (uint32_t)((d >> 23) & 1);
        uint32_t I2 = (uint32_t)((d >> 22) & 1);
        uint32_t J1 = (~I1 ^ S_bit) & 1;
        uint32_t J2 = (~I2 ^ S_bit) & 1;
        uint32_t imm10 = (uint32_t)((d >> 12) & 0x3FF);
        uint32_t imm11 = (uint32_t)((d >> 1) & 0x7FF);
        hw1 = (uint16_t)((hw1 & 0xF800u) | (S_bit << 10) | imm10);
        if (type == 10) {
            hw2 = (uint16_t)((hw2 & 0xD000u) | (J1 << 13) | (1u << 12) | (J2 << 11) | imm11);
        } else {
            hw2 = (uint16_t)((hw2 & 0xD000u) | (J1 << 13) | (J2 << 11) | imm11);
        }
        site[0] = (uint8_t)hw1; site[1] = (uint8_t)(hw1 >> 8);
        site[2] = (uint8_t)hw2; site[3] = (uint8_t)(hw2 >> 8);
        return 0;
    }
    default:
        snprintf(err, errsz,
                 "unsupported ARM relocation type %u in %s (%s)",
                 type, sec_name, obj_path);
        return -1;
    }
    site[0] = (uint8_t)nw;
    site[1] = (uint8_t)(nw >> 8);
    site[2] = (uint8_t)(nw >> 16);
    site[3] = (uint8_t)(nw >> 24);
    return 0;
}

/* RISC-V Relocation Handler */
static inline int linker_reloc_riscv(ZcLink *L, uint8_t *site, uint32_t type,
                                    uint64_t P, int64_t S, int64_t A,
                                    const char *sym_name, const char *obj_path,
                                    const char *sec_name,
                                    const ZcRiscvPcrel *pcrel_tab, size_t pcrel_count,
                                    char *err, size_t errsz) {
    (void)L; (void)sec_name;
    uint32_t w = (uint32_t)site[0] | ((uint32_t)site[1] << 8) |
                 ((uint32_t)site[2] << 16) | ((uint32_t)site[3] << 24);
    uint32_t nw = w;
    switch (type) {
    case 0:  /* R_RISCV_NONE */
    case 43: /* R_RISCV_ALIGN */
    case 51: /* R_RISCV_RELAX */
        return 0;
    case 2:  /* R_RISCV_64 */
        for (int b = 0; b < 8; b++)
            site[b] = (uint8_t)((uint64_t)(S + A) >> (8 * b));
        return 0;
    case 1:  /* R_RISCV_32 */
        for (int b = 0; b < 4; b++)
            site[b] = (uint8_t)((uint32_t)(uint64_t)(S + A) >> (8 * b));
        return 0;
    case 16: /* R_RISCV_BRANCH */
    {
        int64_t d = S + A - (int64_t)P;
        if ((d & 1) != 0 || d < -(1LL << 12) || d >= (1LL << 12)) {
            snprintf(err, errsz,
                     "R_RISCV_BRANCH out of range for `%s' at 0x%lx in %s",
                     sym_name, (unsigned long)P, obj_path);
            return -1;
        }
        uint32_t u = (uint32_t)d & 0x1FFFu;
        uint32_t patch = ((u & 0x1000u) << 19) | ((u & 0x7E0u) << 20) |
                         ((u & 0x800u) >> 4) | ((u & 0x1Eu) << 7);
        nw = (w & 0x01FFF07Fu) | patch;
        break;
    }
    case 17: /* R_RISCV_JAL */
    {
        int64_t d = S + A - (int64_t)P;
        if ((d & 1) != 0 || d < -(1LL << 20) || d >= (1LL << 20)) {
            snprintf(err, errsz,
                     "R_RISCV_JAL out of range for `%s' at 0x%lx in %s",
                     sym_name, (unsigned long)P, obj_path);
            return -1;
        }
        uint32_t u = (uint32_t)d & 0x1FFFFFu;
        uint32_t patch = ((u & 0x100000u) << 11) | ((u & 0x7FEu) << 20) |
                         ((u & 0x800u) << 9) | (u & 0xFF000u);
        nw = (w & 0xFFFu) | patch;
        break;
    }
    case 45: /* R_RISCV_RVC_JUMP (c.j) */
    {
        int64_t d = S + A - (int64_t)P;
        if ((d & 1) != 0 || d < -(1LL << 11) || d >= (1LL << 11)) {
            snprintf(err, errsz,
                     "R_RISCV_RVC_JUMP out of range for `%s' at 0x%lx in %s",
                     sym_name, (unsigned long)P, obj_path);
            return -1;
        }
        uint32_t u = (uint32_t)d & 0xFFFu;
        uint16_t w16 = (uint16_t)site[0] | ((uint16_t)site[1] << 8);
        uint16_t patch = (((u >> 11) & 1) << 12) |
                         (((u >> 4)  & 1) << 11) |
                         (((u >> 8)  & 3) << 9)  |
                         (((u >> 10) & 1) << 8)  |
                         (((u >> 6)  & 1) << 7)  |
                         (((u >> 7)  & 1) << 6)  |
                         (((u >> 1)  & 7) << 3)  |
                         (((u >> 5)  & 1) << 2);
        w16 = (w16 & ~0x1FFCu) | patch;
        site[0] = (uint8_t)w16;
        site[1] = (uint8_t)(w16 >> 8);
        return 0;
    }
    case 18: /* R_RISCV_CALL */
    case 19: /* R_RISCV_CALL_PLT */
    {
        int64_t d = S + A - (int64_t)P;
        int32_t hi = (int32_t)((d + 0x800) >> 12);
        int32_t lo = (int32_t)(d & 0xFFF);
        uint32_t w1 = (uint32_t)site[4] | ((uint32_t)site[5] << 8) |
                      ((uint32_t)site[6] << 16) | ((uint32_t)site[7] << 24);
        nw = (w & 0xFFFu) | ((uint32_t)hi << 12);
        uint32_t nw1 = (w1 & 0x000FFFFFu) | (((uint32_t)lo & 0xFFFu) << 20);
        site[4] = (uint8_t)nw1; site[5] = (uint8_t)(nw1 >> 8);
        site[6] = (uint8_t)(nw1 >> 16); site[7] = (uint8_t)(nw1 >> 24);
        break;
    }
    case 23: /* R_RISCV_PCREL_HI20 */
    {
        int64_t delta = S + A - (int64_t)P;
        int32_t hi = (int32_t)((delta + 0x800) >> 12);
        nw = (w & 0xFFFu) | ((uint32_t)hi << 12);
        break;
    }
    case 24: /* R_RISCV_PCREL_LO12_I */
    {
        int64_t delta = 0;
        bool found = false;
        for (size_t k = 0; k < pcrel_count; k++) {
            if (pcrel_tab[k].p_hi == (uint64_t)S) {
                delta = pcrel_tab[k].delta;
                found = true;
                break;
            }
        }
        if (!found) {
            snprintf(err, errsz,
                     "R_RISCV_PCREL_LO12_I missing matching HI20 for `%s' at 0x%lx in %s",
                     sym_name, (unsigned long)P, obj_path);
            return -1;
        }
        int32_t lo = (int32_t)((delta + A) & 0xFFF);
        nw = (w & 0x000FFFFFu) | (((uint32_t)lo & 0xFFFu) << 20);
        break;
    }
    case 25: /* R_RISCV_PCREL_LO12_S */
    {
        int64_t delta = 0;
        bool found = false;
        for (size_t k = 0; k < pcrel_count; k++) {
            if (pcrel_tab[k].p_hi == (uint64_t)S) {
                delta = pcrel_tab[k].delta;
                found = true;
                break;
            }
        }
        if (!found) {
            snprintf(err, errsz,
                     "R_RISCV_PCREL_LO12_S missing matching HI20 for `%s' at 0x%lx in %s",
                     sym_name, (unsigned long)P, obj_path);
            return -1;
        }
        int32_t lo = (int32_t)((delta + A) & 0xFFF);
        uint32_t u = (uint32_t)lo & 0xFFFu;
        nw = (w & 0x01FFF07Fu) | ((u & 0xFE0u) << 20) | ((u & 0x1Fu) << 7);
        break;
    }
    default:
        snprintf(err, errsz,
                 "unsupported RISC-V relocation type %u in %s (%s)",
                 type, sec_name, obj_path);
        return -1;
    }
    if (type != 1 && type != 2 && type != 0 && type != 43 && type != 51) {
        site[0] = (uint8_t)nw;
        site[1] = (uint8_t)(nw >> 8);
        site[2] = (uint8_t)(nw >> 16);
        site[3] = (uint8_t)(nw >> 24);
    }
    return 0;
}

/* Entry stub emitters */
static inline void linker_stub_riscv(uint8_t *st, uint64_t text_vma, uint64_t main_vma) {
    int64_t d = (int64_t)(main_vma - text_vma);
    int32_t imm_hi = (int32_t)((d + 0x800) >> 12);
    int32_t imm_lo = (int32_t)(d & 0xFFF);
    uint32_t w0 = 0x00000317u | ((uint32_t)imm_hi << 12);
    uint32_t w1 = 0x00030313u | (((uint32_t)imm_lo & 0xFFFu) << 20);
    uint32_t w2 = 0x000300e7u; /* jalr ra, t1, 0 */
    uint32_t w3 = 0x05d00893u; /* li a7, 93 */
    uint32_t w4 = 0x00000073u; /* ecall */
    st[0] = (uint8_t)w0; st[1] = (uint8_t)(w0 >> 8);
    st[2] = (uint8_t)(w0 >> 16); st[3] = (uint8_t)(w0 >> 24);
    st[4] = (uint8_t)w1; st[5] = (uint8_t)(w1 >> 8);
    st[6] = (uint8_t)(w1 >> 16); st[7] = (uint8_t)(w1 >> 24);
    st[8] = (uint8_t)w2; st[9] = (uint8_t)(w2 >> 8);
    st[10] = (uint8_t)(w2 >> 16); st[11] = (uint8_t)(w2 >> 24);
    st[12] = (uint8_t)w3; st[13] = (uint8_t)(w3 >> 8);
    st[14] = (uint8_t)(w3 >> 16); st[15] = (uint8_t)(w3 >> 24);
    st[16] = (uint8_t)w4; st[17] = (uint8_t)(w4 >> 8);
    st[18] = (uint8_t)(w4 >> 16); st[19] = (uint8_t)(w4 >> 24);
}

static inline void linker_stub_aarch64(uint8_t *st, uint64_t text_vma, uint64_t main_vma) {
    /* mov x8, #94 (exit_group): 0xD2800BC8 */
    st[0] = 0xc8; st[1] = 0x0b; st[2] = 0x80; st[3] = 0xd2;
    int32_t rel = (int32_t)((main_vma - (text_vma + 4)) >> 2);
    st[4] = (uint8_t)(rel & 0xFF);
    st[5] = (uint8_t)((rel >> 8) & 0xFF);
    st[6] = (uint8_t)((rel >> 16) & 0xFF);
    st[7] = (uint8_t)(0x94 | ((rel >> 24) & 0x3));
    /* svc #0: 0xD4000001 */
    st[8] = 0x01; st[9] = 0x00; st[10] = 0x00; st[11] = 0xd4;
    /* nop (pad to 16-byte reservation): 0xD503201F */
    st[12] = 0x1f; st[13] = 0x20; st[14] = 0x03; st[15] = 0xd5;
}

static inline void linker_stub_arm(uint8_t *st, uint64_t text_vma, uint64_t main_vma) {
    if (main_vma & 1) {
        /* Interworking: main is in Thumb mode, emit blx */
        int64_t d = (int64_t)main_vma - (int64_t)(text_vma + 8);
        uint32_t H = (uint32_t)((d >> 1) & 1);
        uint32_t imm24 = (uint32_t)((d >> 2) & 0x00FFFFFFu);
        uint32_t blx = 0xFA000000u | (H << 24) | imm24;
        st[0] = (uint8_t)blx; st[1] = (uint8_t)(blx >> 8);
        st[2] = (uint8_t)(blx >> 16); st[3] = (uint8_t)(blx >> 24);
    } else {
        int32_t rel = (int32_t)((main_vma - (text_vma + 8)) >> 2); /* PC is +8 from bl */
        uint32_t bl = 0xEB000000u | (rel & 0xFFFFFF);
        st[0] = (uint8_t)bl; st[1] = (uint8_t)(bl >> 8);
        st[2] = (uint8_t)(bl >> 16); st[3] = (uint8_t)(bl >> 24);
    }
    /* mov r7, #1: 0xE3A07001 */
    st[4] = 0x01; st[5] = 0x70; st[6] = 0xA0; st[7] = 0xE3;
    /* swi #0: 0xEF000000 */
    st[8] = 0x00; st[9] = 0x00; st[10] = 0x00; st[11] = 0xEF;
    /* nop (mov r0, r0): 0xE1A00000 */
    st[12] = 0x00; st[13] = 0x00; st[14] = 0xA0; st[15] = 0xE1;
}

/* Forward declarations of target linker hooks */
int linker_reserve_thunks_x86_64(ZcLink *L, char *err, size_t errsz);
int linker_precollect_riscv_pcrel(ZcLink *L, ZcRiscvPcrel **tab_out, size_t *cnt_out, size_t *cap_out);

/* Linker context operations helper declaration */
static inline const TargetLinkerOps *zero_cc_get_linker_ops(uint16_t machine) {
    static const TargetLinkerOps ops_aarch64 = {
        .machine = 183, /* EM_AARCH64 */
        .name = "aarch64",
        .default_text_base = 0x400000ULL,
        .default_elf_flags = 0,
        .plt_stub_size = 16,
        .apply_reloc = linker_reloc_aarch64,
        .emit_start_stub = linker_stub_aarch64,
        .reserve_thunks = NULL,
        .precollect_pcrel = NULL,
    };
    static const TargetLinkerOps ops_arm = {
        .machine = 40, /* EM_ARM */
        .name = "aarch32",
        .default_text_base = 0x8000ULL,
        .default_elf_flags = 0x00000200u,
        .plt_stub_size = 16,
        .apply_reloc = linker_reloc_arm,
        .emit_start_stub = linker_stub_arm,
        .reserve_thunks = NULL,
        .precollect_pcrel = NULL,
    };
    static const TargetLinkerOps ops_riscv = {
        .machine = 243, /* EM_RISCV */
        .name = "riscv",
        .default_text_base = 0x10000ULL,
        .default_elf_flags = 4,
        .plt_stub_size = 20,
        .apply_reloc = linker_reloc_riscv,
        .emit_start_stub = linker_stub_riscv,
        .reserve_thunks = NULL,
        .precollect_pcrel = linker_precollect_riscv_pcrel,
    };
    static const TargetLinkerOps ops_x86_64 = {
        .machine = 62, /* EM_X86_64 */
        .name = "x86-64",
        .default_text_base = 0x400000ULL,
        .default_elf_flags = 0,
        .plt_stub_size = 16,
        .apply_reloc = NULL, /* inlined in linker.c due to static PIE and far thunks */
        .emit_start_stub = NULL,
        .reserve_thunks = linker_reserve_thunks_x86_64,
        .precollect_pcrel = NULL,
    };

    switch (machine) {
        case 183: return &ops_aarch64;
        case 40:  return &ops_arm;
        case 243: return &ops_riscv;
        case 62:  return &ops_x86_64;
        default:  return NULL;
    }
}

#endif /* ZERO_CC_LINKER_RELOC_H */
