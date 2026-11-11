#ifndef ZERO_CC_VREGS_H
#define ZERO_CC_VREGS_H

#include <stdbool.h>
#include <stdint.h>

/* Virtual Register (VREG) Definitions for ZeroCC
 *
 * Sprint 155 (Priority #10 IR cleanup): Physical/ABI virtual registers
 * (VREG_*) are decoupled from the user temporary ID namespace.
 *
 *   - User virtual temporaries live in [VREG_USER_START, VREG_TAG_BIT)
 *     (16 .. 0x3FFFFFFFu; compactly renumbered into 16..65535 before
 *     register allocation).
 *   - Physical/ABI register tokens carry VREG_TAG_BIT (0x40000000u):
 *       VREG_0 .. VREG_15   = 0x40000000u + 0..15  (integer ABI/phys bank)
 *       VREG_F0 .. VREG_F15 = 0x40000010u + 0..15  (float ABI/phys bank)
 *       VREG_SP, VREG_FP, VREG_FLAGS = 0x40000020u + 0..2
 *   - TEMP_NONE = 0x7FFFFFFFu ("no temp" sentinel, outside both ranges).
 *
 * Both VREG_TAG_BIT and TEMP_NONE fit in positive signed int32_t so signed/
 * unsigned conversions preserve value and > 0 checks hold.
 *
 * Each ISA backend maps hardware index (id & 0xF) to its native register:
 *   x86-64:  0=rax, 1=rcx, 2=rdx, 3=rbx, 4=rsp, 5=rbp, 6=rsi, 7=rdi, 8=r8...
 *   aarch64: 0=x0, 1=x1, 2=x2, 3=x3, 4=x4, 5=x5, 6=x6, 7=x7, 8=x8...
 *   riscv64: 0=a0, 1=a1, 2=a2, 3=a3, 4=a4, 5=a5, 6=a6, 7=a7, 8=t0...
 */

#define VREG_TAG_BIT    0x40000000u
#define VREG_GPR_BASE   (VREG_TAG_BIT + 0x00u)
#define VREG_FP_BASE    (VREG_TAG_BIT + 0x10u)
#define VREG_SPEC_BASE  (VREG_TAG_BIT + 0x20u)

/* Integer physical/ABI registers (bank 0: 0..15) */
#define VREG_0      ((int)(VREG_GPR_BASE +  0u))  /* Result / return value */
#define VREG_1      ((int)(VREG_GPR_BASE +  1u))  /* Temp / argument */
#define VREG_2      ((int)(VREG_GPR_BASE +  2u))  /* High return / rdx / a1 / x1 */
#define VREG_3      ((int)(VREG_GPR_BASE +  3u))  /* Temp / argument */
#define VREG_4      ((int)(VREG_GPR_BASE +  4u))  /* Temp / argument */
#define VREG_5      ((int)(VREG_GPR_BASE +  5u))  /* Temp / argument */
#define VREG_6      ((int)(VREG_GPR_BASE +  6u))  /* Temp / argument */
#define VREG_7      ((int)(VREG_GPR_BASE +  7u))  /* Temp / argument */
#define VREG_8      ((int)(VREG_GPR_BASE +  8u))  /* Temp */
#define VREG_9      ((int)(VREG_GPR_BASE +  9u))  /* Temp */
#define VREG_10     ((int)(VREG_GPR_BASE + 10u))  /* Temp */
#define VREG_11     ((int)(VREG_GPR_BASE + 11u))  /* Temp */
#define VREG_12     ((int)(VREG_GPR_BASE + 12u))  /* Temp */
#define VREG_13     ((int)(VREG_GPR_BASE + 13u))  /* Temp */
#define VREG_14     ((int)(VREG_GPR_BASE + 14u))  /* Temp */
#define VREG_15     ((int)(VREG_GPR_BASE + 15u))  /* Temp */

/* Float physical/ABI registers (bank 1: 16..31, distinct from VREG_0..15) */
#define VREG_F0     ((int)(VREG_FP_BASE +  0u))   /* Float result / return value */
#define VREG_F1     ((int)(VREG_FP_BASE +  1u))   /* Float second return / arg */
#define VREG_F2     ((int)(VREG_FP_BASE +  2u))   /* Float temp / argument */
#define VREG_F3     ((int)(VREG_FP_BASE +  3u))   /* Float temp / argument */
#define VREG_F4     ((int)(VREG_FP_BASE +  4u))   /* Float temp / argument */
#define VREG_F5     ((int)(VREG_FP_BASE +  5u))   /* Float temp / argument */
#define VREG_F6     ((int)(VREG_FP_BASE +  6u))   /* Float temp / argument */
#define VREG_F7     ((int)(VREG_FP_BASE +  7u))   /* Float temp / argument */
#define VREG_F8     ((int)(VREG_FP_BASE +  8u))   /* Float temp */
#define VREG_F9     ((int)(VREG_FP_BASE +  9u))   /* Float temp */
#define VREG_F10    ((int)(VREG_FP_BASE + 10u))   /* Float temp */
#define VREG_F11    ((int)(VREG_FP_BASE + 11u))   /* Float temp */
#define VREG_F12    ((int)(VREG_FP_BASE + 12u))   /* Float temp */
#define VREG_F13    ((int)(VREG_FP_BASE + 13u))   /* Float temp */
#define VREG_F14    ((int)(VREG_FP_BASE + 14u))   /* Float temp */
#define VREG_F15    ((int)(VREG_FP_BASE + 15u))   /* Float temp */

/* Special virtual registers (bank 2: 32..34) */
#define VREG_SP     ((int)(VREG_SPEC_BASE + 0u))  /* Stack pointer - use BE(c)->sp_reg */
#define VREG_FP     ((int)(VREG_SPEC_BASE + 1u))  /* Frame pointer - use BE(c)->fp_reg */
#define VREG_FLAGS  ((int)(VREG_SPEC_BASE + 2u))  /* Condition flags */

/* Total number of compact VREG slots (0..15 GPR, 16..31 FP, 32..34 special) */
#define VREG_COUNT  35

/* First temp ID available for user virtual temporary allocation */
#define VREG_USER_START 16

/* True if `id` is any physical/ABI VREG_* token (excludes TEMP_NONE). */
static inline bool zc_temp_is_vreg(uint32_t id) {
    return id >= VREG_GPR_BASE && id < (VREG_GPR_BASE + (uint32_t)VREG_COUNT);
}

/* True if `id` is an integer physical register token (VREG_0 .. VREG_15). */
static inline bool zc_temp_is_gpr_vreg(uint32_t id) {
    return id >= VREG_GPR_BASE && id < (VREG_GPR_BASE + 16u);
}

/* True if `id` is a floating-point physical register token (VREG_F0 .. VREG_F15). */
static inline bool zc_temp_is_fp_vreg(uint32_t id) {
    return id >= VREG_FP_BASE && id < (VREG_FP_BASE + 16u);
}

/* True if `id` is an allocatable user virtual temporary (not 0, VREG_*, or TEMP_NONE). */
static inline bool zc_temp_is_user(uint32_t id) {
    return id >= (uint32_t)VREG_USER_START && id < VREG_TAG_BIT;
}

/* Compact index in [0, VREG_COUNT) for a VREG_* token (0..15 GPR, 16..31 FP, 32..34 special). */
static inline int zc_vreg_index(uint32_t id) {
    return (int)(id - VREG_GPR_BASE);
}

/* Bank-local hardware index in [0, 15] for VREG_0..15 or VREG_F0..F15. */
static inline int zc_vreg_hw_num(uint32_t id) {
    return (int)(id & 0xFu);
}

#endif /* ZERO_CC_VREGS_H */
