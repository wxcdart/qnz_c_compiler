/* =========================================================================
 * RISC-V shared encoders & helpers (Sprint 114).
 *
 * Extracted from riscv.c: the instruction encoders and register-mapping
 * helpers used by BOTH the RV64 and RV32 backend translation units
 * (riscv64.c / riscv32.c). Everything here is stateless (no
 * ZeroCCCompiler access except the byte-append helper), so it is safe to
 * duplicate per-TU as static inline.
 * ========================================================================= */
#ifndef ZERO_CC_RISCV_COMMON_H
#define ZERO_CC_RISCV_COMMON_H

#include <stdint.h>
#include "zero_cc/ir/machine_emitter.h"

/* Helper macro to convert VREG to physical register */
static inline int riscv_vreg_to_phys(int vreg) {
    switch (vreg) {
        case 0:  return 10;  /* a0 - result */
        case 1:  return 11;  /* a1 */
        case 2:  return 12;  /* a2 */
        case 3:  return 13;  /* a3 */
        case 4:  return 14;  /* a4 */
        case 5:  return 15;  /* a5 */
        case 6:  return 16;  /* a6 */
        case 7:  return 17;  /* a7 */
        case 8:  return 5;   /* t0 */
        case 9:  return 6;   /* t1 */
        case 10: return 7;   /* t2 */
        case 11: return 28;  /* t3 */
        case 12: return 29;  /* t4 */
        case 13: return 30;  /* t5 */
        case 14: return 31;  /* t6 */
        case 15: return 3;   /* gp (avoid for now) */
        default: return vreg;
    }
}

/* Wrapper functions for VREG mapping */
static inline int riscv_get_int_reg(int vreg) { return riscv_vreg_to_phys(vreg); }
static inline int riscv_get_fp_reg(int vreg) { return vreg; }  /* Float regs are sequential */

/* The IR layer hands the raw emitters PHYSICAL RISC-V register numbers
 * (a0=10, a1=11, ..., s0=8, sp=2, t0=5, ...; FP: fa0=10, ...), so the
 * encoders must use them verbatim - remapping them (as the legacy
 * riscv_vreg_to_phys did for VREGs) scrambled every frame access and
 * argument. */
#define RV_REG(vreg) ((vreg) & 0x1F)

/* =========================================================================
 * RV64 instruction encoders (base ISA + M + F/D; no RVC)
 * ========================================================================= */

/* R-type: funct7 rs2 rs1 funct3 rd opcode(0x33) */
static inline uint32_t rv64_R(int rd, int rs1, int rs2, int f3, int f7) {
    return (uint32_t)(((uint32_t)f7 << 25) | (((uint32_t)(rs2 & 0x1F)) << 20) |
                      (((uint32_t)(rs1 & 0x1F)) << 15) | (((uint32_t)(f3 & 7)) << 12) |
                      (((uint32_t)(rd & 0x1F)) << 7) | 0x33u);
}
/* I-type: imm[11:0] rs1 funct3 rd opcode (0x13 OP-IMM / 0x03 load / 0x07 FLx) */
static inline uint32_t rv64_I(int rd, int rs1, int f3, int16_t imm, uint32_t opcode) {
    return (uint32_t)((((uint32_t)(uint16_t)imm & 0xFFFu) << 20) |
                      (((uint32_t)(rs1 & 0x1F)) << 15) | (((uint32_t)(f3 & 7)) << 12) |
                      (((uint32_t)(rd & 0x1F)) << 7) | (opcode & 0x7Fu));
}
/* S-type: imm[11:5] rs2 rs1 funct3 imm[4:0] opcode (0x23 store / 0x27 FSx).
 * The immediate is split across the top and bottom of the word. */
static inline uint32_t rv64_S(int rs1, int rs2, int f3, int16_t imm, uint32_t opcode) {
    uint32_t u = (uint32_t)(uint16_t)imm & 0xFFFu;
    return (uint32_t)(((u & 0xFE0u) << 20) | ((u & 0x1Fu) << 7) |
                      (((uint32_t)(rs2 & 0x1F)) << 20) | (((uint32_t)(rs1 & 0x1F)) << 15) |
                      (((uint32_t)(f3 & 7)) << 12) | (opcode & 0x7Fu));
}
/* B-type branch: imm[12|10:5] rs2 rs1 funct3 imm[4:1|11] opcode(0x63) */
static inline uint32_t rv64_B(int rs1, int rs2, int f3, int32_t imm) {
    uint32_t u = (uint32_t)imm & 0x1FFFu;
    return (uint32_t)(((u & 0x1000u) << 19) | ((u & 0x7E0u) << 20) |
                      (((uint32_t)(rs2 & 0x1F)) << 20) | (((uint32_t)(rs1 & 0x1F)) << 15) |
                      (((uint32_t)(f3 & 7)) << 12) | ((u & 0x1Eu) << 7) |
                      ((u & 0x800u) >> 4) | 0x63u);
}
/* J-type jal: imm[20|10:1|11|19:12] rd opcode(0x6F) */
static inline uint32_t rv64_J(int rd, int32_t imm) {
    uint32_t u = (uint32_t)imm & 0x1FFFFFu;
    return (uint32_t)(((u & 0x100000u) << 11) | ((u & 0x7FEu) << 20) |
                      ((u & 0x800u) << 9) | (u & 0xFF000u) |
                      (((uint32_t)(rd & 0x1F)) << 7) | 0x6Fu);
}
/* LUI / AUIPC: imm[31:12] rd opcode(0x37/0x17) */
static inline uint32_t rv64_LUI(int rd, uint32_t imm20) {
    return (uint32_t)(((imm20 & 0xFFFFFu) << 12) | (((uint32_t)(rd & 0x1F)) << 7) | 0x37u);
}
static inline uint32_t rv64_AUIPC(int rd, uint32_t imm20) {
    return (uint32_t)(((imm20 & 0xFFFFFu) << 12) | (((uint32_t)(rd & 0x1F)) << 7) | 0x17u);
}
/* OP-FP: funct5 rs2 rs1 rm rd opcode(0x53) */
static inline uint32_t rv64_FP(int rd, int rs1, int rs2, int funct5, int rm) {
    return (uint32_t)(((uint32_t)funct5 << 25) | (((uint32_t)(rs2 & 0x1F)) << 20) |
                      (((uint32_t)(rs1 & 0x1F)) << 15) | (((uint32_t)(rm & 7)) << 12) |
                      (((uint32_t)(rd & 0x1F)) << 7) | 0x53u);
}

/* Frame geometry (matches the text emitter in riscv_asm.c):
 *   addi sp, sp, -(aligned+16);  sd ra, aligned+8(sp);  sd s0, aligned(sp)
 *   addi s0, sp, aligned
 * i.e. s0 = entry_sp - 16, the ra/s0 pair sits 16 bytes above s0, and all
 * locals/callee-save slots live at negative offsets from s0. The epilogue
 * needs no frame size: sp = s0; ra = [s0+8]; s0 = [s0]; sp += 16. */
#define RV64_RA  1
#define RV64_SP  2
#define RV64_S0  8
#define RV64_T0  5
#define RV64_T1  6
#define RV64_T5  30  /* large-immediate/offset address scratch (Sprint 109): never
                      * allocatable, never a 128-bit value scratch - the only GPR
                      * guaranteed free when a 12-bit displacement overflows. */
#define RV64_T6  31

/* Little-endian 32-bit instruction emit (defined once, used by all encoders) */
static inline void riscv_emit_u32(ZeroCCCompiler *c, uint32_t insn) {
    me_emit_u8(c, (uint8_t)(insn & 0xFF));
    me_emit_u8(c, (uint8_t)((insn >> 8) & 0xFF));
    me_emit_u8(c, (uint8_t)((insn >> 16) & 0xFF));
    me_emit_u8(c, (uint8_t)((insn >> 24) & 0xFF));
}

#endif /* ZERO_CC_RISCV_COMMON_H */
