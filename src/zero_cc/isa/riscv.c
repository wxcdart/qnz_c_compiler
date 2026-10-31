#include "zero_cc/isa/isa_backend.h"
#include "zero_cc/ir/machine_emitter.h"
#include "zero_cc/ir/asm_emitter.h"
#include "zero_cc/isa/riscv_asm.c"  /* Include assembly emission functions */
#include <stdint.h>

/* Helper macro to convert VREG to physical register */
static int riscv_vreg_to_phys(int vreg) {
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
static int riscv_get_int_reg(int vreg) { return riscv_vreg_to_phys(vreg); }
static int riscv_get_fp_reg(int vreg) { return vreg; }  /* Float regs are sequential */

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

/* RISC-V RV64 backend implementation */
static void riscv64_emit_mov_imm(ZeroCCCompiler *c, int dst_reg, int64_t imm);  /* fwd (big frames) */
static void riscv64_emit_mov_reg(ZeroCCCompiler *c, int dst, int src);           /* fwd */
static void riscv64_emit_leave(ZeroCCCompiler *c);                               /* fwd */

/* Little-endian 32-bit instruction emit (defined once, used by all encoders) */
static inline void riscv_emit_u32(ZeroCCCompiler *c, uint32_t insn) {
    me_emit_u8(c, insn & 0xFF);
    me_emit_u8(c, (insn >> 8) & 0xFF);
    me_emit_u8(c, (insn >> 16) & 0xFF);
    me_emit_u8(c, (insn >> 24) & 0xFF);
}

static void riscv64_emit_prologue(ZeroCCCompiler *c, size_t stack_size, uint32_t callee_save_mask) {
    if (c->lowering.is_frameless) return;
    (void)callee_save_mask;  /* callee-save stores are emitted by emit_prologue_mc() */
    size_t aligned = (stack_size + 15) & ~(size_t)15;
    size_t total = aligned + 16;

    if (c->lowering.omit_frame_pointer) {
        /* Option 4: no frame pointer (s0). The decision in ast_lowering.c
         * guarantees total <= 2047 so a single addi suffices. The 8 bytes
         * that would hold s0 are left unused, keeping slot geometry
         * identical to the FP layout (delta = aligned). */
        int f3 = (c->isa == ISA_RISCV32) ? 2 : 3;  /* sw (32-bit) vs sd (64-bit) */
        riscv_emit_u32(c, rv64_I(RV64_SP, RV64_SP, 0, (int16_t)(-(int32_t)total), 0x13));
        riscv_emit_u32(c, rv64_S(RV64_SP, RV64_RA, f3, (int16_t)(aligned + 8), 0x23));  /* s ra, aligned+8(sp) */
        return;
    }

    int big = (total > 2047);
    if (big) {
        riscv64_emit_mov_imm(c, RV64_T0, (int64_t)total);  /* li t0, total */
        riscv_emit_u32(c, rv64_R(RV64_SP, RV64_SP, RV64_T0, 0, 0x20));  /* sub sp, sp, t0 */
        if (aligned > 2047) {
            riscv64_emit_mov_imm(c, 6 /* t1 */, (int64_t)aligned);
            riscv_emit_u32(c, rv64_R(6 /* t1 */, RV64_SP, 6 /* t1 */, 0, 0));  /* add t1, sp, t1 */
        } else {
            riscv_emit_u32(c, rv64_I(6 /* t1 */, RV64_SP, 0, (int16_t)aligned, 0x13));
        }
        riscv_emit_u32(c, rv64_S(6 /* t1 */, RV64_RA, 3, 8, 0x23));    /* sd ra, 8(t1) */
        riscv_emit_u32(c, rv64_S(6 /* t1 */, RV64_S0, 3, 0, 0x23));    /* sd s0, 0(t1) */
        riscv_emit_u32(c, rv64_R(RV64_S0, 6 /* t1 */, 0, 0, 0));       /* add s0, t1, zero */
    } else {
        riscv_emit_u32(c, rv64_I(RV64_SP, RV64_SP, 0, (int16_t)(-(int32_t)total), 0x13));
        riscv_emit_u32(c, rv64_S(RV64_SP, RV64_RA, 3, (int16_t)(aligned + 8), 0x23));  /* sd ra, aligned+8(sp) */
        riscv_emit_u32(c, rv64_S(RV64_SP, RV64_S0, 3, (int16_t)aligned, 0x23));        /* sd s0, aligned(sp) */
        riscv_emit_u32(c, rv64_I(RV64_S0, RV64_SP, 0, (int16_t)aligned, 0x13));        /* addi s0, sp, aligned */
    }
}

static void riscv64_emit_epilogue(ZeroCCCompiler *c) {
    riscv64_emit_leave(c);
    /* ret: jalr zero, 0(ra) */
    riscv_emit_u32(c, 0x00008067);
}

static void riscv64_emit_leave(ZeroCCCompiler *c) {
    if (c->lowering.is_frameless) return;
    if (c->lowering.omit_frame_pointer) {
        /* Option 4: no s0. aligned = frame_delta (decision in
         * ast_lowering.c guarantees aligned+16 <= 2047). */
        int aligned = (int)c->lowering.frame_delta;
        int f3 = (c->isa == ISA_RISCV32) ? 2 : 3;  /* lw (32-bit) vs ld (64-bit) */
        riscv_emit_u32(c, rv64_I(RV64_RA, RV64_SP, f3, (int16_t)(aligned + 8), 0x03));  /* l ra, aligned+8(sp) */
        riscv_emit_u32(c, rv64_I(RV64_SP, RV64_SP, 0, (int16_t)(aligned + 16), 0x13));  /* addi sp, sp, aligned+16 */
        return;
    }
    /* Deallocate the frame without knowing its size: s0 = entry_sp - 16. */
    riscv_emit_u32(c, rv64_I(RV64_SP, RV64_S0, 0, 0, 0x13));  /* addi sp, s0, 0 */
    riscv_emit_u32(c, rv64_I(RV64_RA, RV64_SP, 3, 8, 0x03));  /* ld ra, 8(sp) */
    riscv_emit_u32(c, rv64_I(RV64_S0, RV64_SP, 3, 0, 0x03));  /* ld s0, 0(sp) */
    riscv_emit_u32(c, rv64_I(RV64_SP, RV64_SP, 0, 16, 0x13)); /* addi sp, sp, 16 */
}

/* RISC-V register info */
static const ISARegInfo riscv_regs = {
    .int_regs = {
        "zero", "ra", "sp", "gp", "tp", "t0", "t1", "t2",
        "s0", "s1", "a0", "a1", "a2", "a3", "a4", "a5",
        "a6", "a7", "s2", "s3", "s4", "s5", "s6", "s7",
        "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6",
    },
    .fp_regs = {
        "ft0", "ft1", "ft2", "ft3", "ft4", "ft5", "ft6", "ft7",
        "fs0", "fs1", "fa0", "fa1", "fa2", "fa3", "fa4", "fa5",
        "fa6", "fa7", "fs2", "fs3", "fs4", "fs5", "fs6", "fs7",
        "fs8", "fs9", "fs10", "fs11", "ft8", "ft9", "ft10", "ft11",
    },
    .int_arg_regs = {"a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7"},
    .fp_arg_regs = {"fa0", "fa1", "fa2", "fa3", "fa4", "fa5", "fa6", "fa7"},
    .int_ret_reg = "a0",
    .fp_ret_reg = "fa0",
    .num_int_regs = 32,
    .num_fp_regs = 32,
    .vec_regs = {"v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v8", "v9", "v10", "v11", "v12", "v13", "v14", "v15", "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23", "v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31"},
    .num_vec_regs = 32,
};

/* RISC-V calling convention (LP64D for 64-bit, ILP32D for 32-bit) */
/* Register numbers: 0=zero 1=ra 2=sp 3=gp 4=tp 5=t0 6=t1 7=t2 8=s0 9=s1 10=a0... */
static const int riscv64_int_arg_regs[] = {10, 11, 12, 13, 14, 15, 16, 17};  /* a0-a7 */
static const int riscv64_fp_arg_regs[] = {10, 11, 12, 13, 14, 15, 16, 17};  /* fa0-fa7 */
static const int riscv64_callee_saved_gpr[] = {8, 9, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27};  /* s0-s11 */

static const ISACallingConvention riscv64_conv = {
    .int_arg_regs = riscv64_int_arg_regs,
    .int_arg_reg_count = 8,
    .fp_arg_regs = riscv64_fp_arg_regs,
    .fp_arg_reg_count = 8,
    .callee_saved_gpr = riscv64_callee_saved_gpr,
    .callee_saved_gpr_count = 12,
    .callee_save_slot_offsets = (const int32_t[]){8, 16, 24, 32, 40, 48, 56, 64, 72, 80, 88, 96},
};

/* RISC-V 32-bit calling convention (similar but different register sizes) */
static const int riscv32_int_arg_regs[] = {10, 11, 12, 13, 14, 15, 16, 17};  /* a0-a7 */
static const int riscv32_fp_arg_regs[] = {10, 11, 12, 13, 14, 15, 16, 17};  /* fa0-fa7 */
static const int riscv32_callee_saved_gpr[] = {8, 9, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27};  /* s0-s11 */

static const ISACallingConvention riscv32_conv = {
    .int_arg_regs = riscv32_int_arg_regs,
    .int_arg_reg_count = 8,
    .fp_arg_regs = riscv32_fp_arg_regs,
    .fp_arg_reg_count = 8,
    .callee_saved_gpr = riscv32_callee_saved_gpr,
    .callee_saved_gpr_count = 12,
    .callee_save_slot_offsets = (const int32_t[]){8, 16, 24, 32, 40, 48, 56, 64, 72, 80, 88, 96},
};

/* Assembly emission functions for text output - defined in riscv_asm.c */
/* static void riscv64_emit_prologue_asm(ZeroCCCompiler *c, size_t stack_size) { ... } */
/* static void riscv64_emit_epilogue_asm(ZeroCCCompiler *c) { ... } */
/* static const char *riscv64_get_hw_reg_name(int phys_reg) { ... } */

static size_t riscv64_get_code_offset(ZeroCCCompiler *c) {
    return me_get_offset(c);
}

static void riscv64_emit_mov_imm_32(ZeroCCCompiler *c, int dst_reg, int32_t imm) {
    if (imm >= -2048 && imm <= 2047) {
        riscv_emit_u32(c, rv64_I(RV_REG(dst_reg), 0, 0, (int16_t)imm, 0x13));  /* addi dst, zero, imm */
    } else {
        /* lui + addi: rounded high 20 bits, remainder as 12-bit two's complement.
         * LUI sign-extends bit 31, so this yields the correct 64-bit value. */
        uint32_t u = (uint32_t)imm;
        uint32_t hi = (u + 0x800u) >> 12;
        int64_t lo64 = (int64_t)imm - (int64_t)(hi << 12);  /* -2048..2047 */
        if (hi) {
            riscv_emit_u32(c, rv64_LUI(RV_REG(dst_reg), hi));      /* lui dst, hi */
        }
        if (lo64) {
            riscv_emit_u32(c, rv64_I(RV_REG(dst_reg), hi ? RV_REG(dst_reg) : 0, 0, (int16_t)lo64, 0x13));
        }
    }
}

static void riscv64_emit_mov_imm(ZeroCCCompiler *c, int dst_reg, int64_t imm) {
    if (c->isa == ISA_RISCV32 || (imm >= -2147483648LL && imm <= 2147483647LL)) {
        riscv64_emit_mov_imm_32(c, dst_reg, (int32_t)imm);
        return;
    }
    uint64_t u64 = (uint64_t)imm;
    uint32_t hi32 = (uint32_t)(u64 >> 32);
    uint32_t lo32 = (uint32_t)u64;

    riscv64_emit_mov_imm_32(c, dst_reg, (int32_t)hi32);
    /* slli dst, dst, 32 */
    riscv_emit_u32(c, rv64_I(RV_REG(dst_reg), RV_REG(dst_reg), 1, 32, 0x13));

    if (lo32 != 0) {
        int scratch = (dst_reg != RV64_T0) ? RV64_T0 : RV64_T1;
        riscv64_emit_mov_imm_32(c, scratch, (int32_t)lo32);
        /* zext 32-bit scratch: slli scratch, 32; srli scratch, 32 */
        riscv_emit_u32(c, rv64_I(RV_REG(scratch), RV_REG(scratch), 1, 32, 0x13));
        riscv_emit_u32(c, rv64_I(RV_REG(scratch), RV_REG(scratch), 5, 32, 0x13));
        /* or dst, dst, scratch */
        riscv_emit_u32(c, rv64_R(RV_REG(dst_reg), RV_REG(dst_reg), RV_REG(scratch), 6, 0));
    }
}

static void riscv64_emit_add(ZeroCCCompiler *c, int dst, int src1, int src2) {
    riscv_emit_u32(c, rv64_R(RV_REG(dst), RV_REG(src1), RV_REG(src2), 0, 0));
}

static void riscv64_emit_sub(ZeroCCCompiler *c, int dst, int src1, int src2) {
    riscv_emit_u32(c, rv64_R(RV_REG(dst), RV_REG(src1), RV_REG(src2), 0, 0x20));
}

static void riscv64_emit_mul(ZeroCCCompiler *c, int dst, int src1, int src2) {
    riscv_emit_u32(c, rv64_R(RV_REG(dst), RV_REG(src1), RV_REG(src2), 0, 1));
}

static void riscv64_emit_div(ZeroCCCompiler *c, int dst, int src1, int src2) {
    /* div/divu dst, src1, src2: opcode 0x33, funct3=4 (div) or 5 (divu), funct7=1 */
    int f3 = c->lowering.div_is_unsigned ? 5 : 4;
    riscv_emit_u32(c, rv64_R(RV_REG(dst), RV_REG(src1), RV_REG(src2), f3, 1));
}

static void riscv64_emit_and(ZeroCCCompiler *c, int dst, int src1, int src2) {
    riscv_emit_u32(c, rv64_R(RV_REG(dst), RV_REG(src1), RV_REG(src2), 7, 0));
}

static void riscv64_emit_or(ZeroCCCompiler *c, int dst, int src1, int src2) {
    riscv_emit_u32(c, rv64_R(RV_REG(dst), RV_REG(src1), RV_REG(src2), 6, 0));
}

static void riscv64_emit_xor(ZeroCCCompiler *c, int dst, int src1, int src2) {
    riscv_emit_u32(c, rv64_R(RV_REG(dst), RV_REG(src1), RV_REG(src2), 4, 0));
}

static void riscv64_emit_shl(ZeroCCCompiler *c, int dst, int src1, int src2) {
    /* sll dst, src1, src2 (register shift amount) */
    riscv_emit_u32(c, rv64_R(RV_REG(dst), RV_REG(src1), RV_REG(src2), 1, 0));
}

/* RISC-V has no flags register: the operand registers of the last
 * OP_CMP/OP_CMP_ZERO are recorded in c->lowering.cmp_src1/2 (by the MC
 * dispatch, together with cmp_is_fp); their live ranges are extended to
 * the flag consumer by live_range_compute(), so re-materializing the
 * predicate from them here is safe. */
static void riscv64_emit_cmp(ZeroCCCompiler *c, int src1, int src2) {
    (void)c; (void)src1; (void)src2;  /* state recorded by the MC dispatch */
}

static void riscv64_emit_neg(ZeroCCCompiler *c, int dst, int src) {
    /* sub dst, zero, src */
    riscv_emit_u32(c, rv64_R(RV_REG(dst), 0, RV_REG(src), 0, 0x20));
}

/* In-place width extension (dst and src are the same physical register in
 * the width-fixup path; the general form handles distinct registers too).
 * sext:  slli rd, rs, N; srai rd, rd, N     (N = 56/48/32 for 8/16/32-bit)
 * zext8: andi rd, rs, 0xff
 * zext16/32: slli rd, rs, N; srli rd, rd, N */
static void riscv64_emit_extend(ZeroCCCompiler *c, int dst, int src, int width, int sign) {
    if (c->isa == ISA_RISCV32) {
        if (sign) {
            int shamt = (width == 1) ? 24 : (width == 2) ? 16 : 0;
            if (shamt > 0) {
                riscv_emit_u32(c, rv64_I(RV_REG(dst), RV_REG(src), 1, shamt, 0x13));          /* slli (f3=1) */
                riscv_emit_u32(c, rv64_I(RV_REG(dst), RV_REG(dst), 5, shamt, 0x13) | (1u << 30)); /* srai (f3=5, bit 30) */
            } else if (dst != src) {
                riscv64_emit_mov_reg(c, dst, src);
            }
        } else if (width == 1) {
            riscv_emit_u32(c, rv64_I(RV_REG(dst), RV_REG(src), 7, 0xFF, 0x13));           /* andi rd, rs, 0xff */
        } else if (width == 2) {
            int shamt = 16;
            riscv_emit_u32(c, rv64_I(RV_REG(dst), RV_REG(src), 1, shamt, 0x13));          /* slli (f3=1) */
            riscv_emit_u32(c, rv64_I(RV_REG(dst), RV_REG(dst), 5, shamt, 0x13));          /* srli (f3=5) */
        } else if (dst != src) {
            riscv64_emit_mov_reg(c, dst, src);
        }
        return;
    }
    if (sign) {
        int shamt = (width == 1) ? 56 : (width == 2) ? 48 : 32;
        riscv_emit_u32(c, rv64_I(RV_REG(dst), RV_REG(src), 1, shamt, 0x13));          /* slli (f3=1) */
        riscv_emit_u32(c, rv64_I(RV_REG(dst), RV_REG(dst), 5, shamt, 0x13) | (1u << 30)); /* srai (f3=5, bit 30) */
    } else if (width == 1) {
        riscv_emit_u32(c, rv64_I(RV_REG(dst), RV_REG(src), 7, 0xFF, 0x13));           /* andi rd, rs, 0xff */
    } else {
        int shamt = (width == 2) ? 48 : 32;
        riscv_emit_u32(c, rv64_I(RV_REG(dst), RV_REG(src), 1, shamt, 0x13));          /* slli (f3=1) */
        riscv_emit_u32(c, rv64_I(RV_REG(dst), RV_REG(dst), 5, shamt, 0x13));          /* srli (f3=5) */
    }
}

static void riscv64_emit_load(ZeroCCCompiler *c, int dst_reg, int base_reg, int32_t offset) {
    /* ld dst, offset(base) for RV64 (f3=3), lw for RV32 (f3=2) */
    int f3 = (c->isa == ISA_RISCV32) ? 2 : 3;
    if (offset >= -2048 && offset <= 2047) {
        riscv_emit_u32(c, rv64_I(RV_REG(dst_reg), RV_REG(base_reg), f3, (int16_t)offset, 0x03));
    } else {
        int scratch = (base_reg != RV64_T0 && dst_reg != RV64_T0) ? RV64_T0 : RV64_T1;
        riscv64_emit_mov_imm(c, scratch, offset);
        riscv_emit_u32(c, rv64_R(scratch, RV_REG(base_reg), scratch, 0, 0));
        riscv_emit_u32(c, rv64_I(RV_REG(dst_reg), scratch, f3, 0, 0x03));
    }
}

static void riscv64_emit_store(ZeroCCCompiler *c, int src_reg, int base_reg, int32_t offset) {
    /* sd src, offset(base) for RV64 (f3=3), sw for RV32 (f3=2) */
    int f3 = (c->isa == ISA_RISCV32) ? 2 : 3;
    if (offset >= -2048 && offset <= 2047) {
        riscv_emit_u32(c, rv64_S(RV_REG(base_reg), RV_REG(src_reg), f3, (int16_t)offset, 0x23));
    } else {
        int scratch = (base_reg != RV64_T0 && src_reg != RV64_T0) ? RV64_T0 : RV64_T1;
        riscv64_emit_mov_imm(c, scratch, offset);
        riscv_emit_u32(c, rv64_R(scratch, RV_REG(base_reg), scratch, 0, 0));
        riscv_emit_u32(c, rv64_S(scratch, RV_REG(src_reg), f3, 0, 0x23));
    }
}

static size_t riscv64_emit_jmp(ZeroCCCompiler *c, const char *label) {
    (void)label;
    size_t patch_pos = me_get_offset(c);
    riscv_emit_u32(c, rv64_J(0, 0));  /* jal zero, 0 - patched later */
    return patch_pos;
}

/* OP_JE/JNE lower to `test src,src` + je/jne: emit_test records the value
 * register (below); the branch is on that register against zero.
 * To support arbitrary target distances up to +/-1MB (exceeding B-type's
 * +/-4KB limit), every conditional branch is emitted as an inverted
 * B-type branch that skips (+8) a 20-bit JAL trampoline. */
static size_t riscv64_emit_jeq(ZeroCCCompiler *c, const char *label) {
    (void)label;
    riscv_emit_u32(c, rv64_B(c->lowering.cmp_src1, 0, 1, 8));  /* bne val, zero, +8 */
    size_t patch_pos = me_get_offset(c);
    riscv_emit_u32(c, rv64_J(0, 0));                           /* jal zero, 0 */
    return patch_pos;
}

static size_t riscv64_emit_jne(ZeroCCCompiler *c, const char *label) {
    (void)label;
    riscv_emit_u32(c, rv64_B(c->lowering.cmp_src1, 0, 0, 8));  /* beq val, zero, +8 */
    size_t patch_pos = me_get_offset(c);
    riscv_emit_u32(c, rv64_J(0, 0));                           /* jal zero, 0 */
    return patch_pos;
}

/* JL/JLE/JG/JGE follow an OP_CMP: inverted B-type skip (+8) + JAL trampoline */
static size_t riscv64_emit_jlt(ZeroCCCompiler *c, const char *label) {
    (void)label;
    riscv_emit_u32(c, rv64_B(c->lowering.cmp_src1, c->lowering.cmp_src2, 5, 8));  /* bge a, b, +8 */
    size_t patch_pos = me_get_offset(c);
    riscv_emit_u32(c, rv64_J(0, 0));
    return patch_pos;
}

static size_t riscv64_emit_jle(ZeroCCCompiler *c, const char *label) {
    (void)label;
    riscv_emit_u32(c, rv64_B(c->lowering.cmp_src2, c->lowering.cmp_src1, 4, 8));  /* blt b, a, +8 */
    size_t patch_pos = me_get_offset(c);
    riscv_emit_u32(c, rv64_J(0, 0));
    return patch_pos;
}

static size_t riscv64_emit_jgt(ZeroCCCompiler *c, const char *label) {
    (void)label;
    riscv_emit_u32(c, rv64_B(c->lowering.cmp_src2, c->lowering.cmp_src1, 5, 8));  /* bge b, a, +8 */
    size_t patch_pos = me_get_offset(c);
    riscv_emit_u32(c, rv64_J(0, 0));
    return patch_pos;
}

static size_t riscv64_emit_jge(ZeroCCCompiler *c, const char *label) {
    (void)label;
    riscv_emit_u32(c, rv64_B(c->lowering.cmp_src1, c->lowering.cmp_src2, 4, 8));  /* blt a, b, +8 */
    size_t patch_pos = me_get_offset(c);
    riscv_emit_u32(c, rv64_J(0, 0));
    return patch_pos;
}

static void riscv64_emit_call(ZeroCCCompiler *c, const char *func_name) {
    /* jal ra, offset.
     * For internal (defined) functions: patch the jal directly at codegen time.
     * For external/prototype functions: emit R_RISCV_CALL_PLT relocation for linker. */
    ZeroCCFunction *fn = func_name ? zero_cc_find_function(c, func_name) : NULL;
    size_t pos = me_get_offset(c);

    if (fn && !fn->is_prototype) {
        /* Internal function: compute relative offset and emit jal with correct displacement */
        int32_t rel = (int32_t)(fn->text_offset - pos);
        riscv_emit_u32(c, rv64_J(1 /* ra */, rel));
    } else {
        /* External or prototype function: emit jal with 0 offset + relocation */
        riscv_emit_u32(c, rv64_J(1 /* ra */, 0));
        if (func_name && c->reloc_count < ZERO_CC_MAX_RELOCS) {
            ZeroCCReloc *r = &c->relocs[c->reloc_count++];
            r->text_patch_offset = pos;
            snprintf(r->target_symbol, sizeof(r->target_symbol), "%s", func_name);
            r->is_call = true;
        }
    }
}

static void riscv64_emit_ret(ZeroCCCompiler *c) {
    /* jalr zero, 0(ra) */
    riscv_emit_u32(c, 0x00008067);
}

/* OS syscall (Linux RISC-V ABI): number in a7, args 1-6 in a0-a5, result
 * in a0 (= VREG_0, no fix-up needed). ecall clobbers the caller-saved set
 * (t0-t6, a0-a7), so every allocatable clobbered register that may hold a
 * live value - t2 (x7), a1-a7 (x11-x17), t3-t6 (x28-x31); t0/t1 are
 * scratch, a0 is the result - is saved before and restored after, the
 * same caller-saved-fix idiom the OP_CALL lowering uses for calls. */
static void riscv_emit_syscall(ZeroCCCompiler *c, int arg_count, const int *arg_temps) {
    static const int linux_syscall_regs[7] = {17, 10, 11, 12, 13, 14, 15}; /* a7, a0-a5 */
    static const int clobbered_live_regs[] = {7, 11, 12, 13, 14, 15, 16, 17, 28, 29, 30, 31};
    int32_t spill_base_offset = c->regalloc.spill_base_offset;
    int n = (arg_count < 7) ? arg_count : 7;
    /* Save the live values the kernel would clobber. */
    for (size_t r = 0; r < sizeof(clobbered_live_regs) / sizeof(clobbered_live_regs[0]); r++) {
        if (BE(c)->emit_push_reg) BE(c)->emit_push_reg(c, clobbered_live_regs[r]);
    }
    int src_regs[8] = {0};
    /* Pass 1: resolve every source (spill reloads into scratch_reg2), push
     * the ones that still need a move. */
    for (int a = 0; a < n; a++) {
        int temp = arg_temps[a];
        int src_reg;
        int sp = regalloc_get_spill_slot_for_temp(&c->regalloc, temp);
        if (sp >= 0) {
            int32_t off = spill_base_offset + (sp + 1) * 8;
            if (BE(c)->emit_load) BE(c)->emit_load(c, BE(c)->scratch_reg2, BE(c)->fp_reg, -off);
            src_reg = BE(c)->scratch_reg2;
        } else {
            src_reg = regalloc_get_reg_for_temp(&c->regalloc, temp);
        }
        src_regs[a] = src_reg;
        if (src_reg != linux_syscall_regs[a] && BE(c)->emit_push_reg) {
            BE(c)->emit_push_reg(c, src_reg);
        }
    }
    /* Pass 2: pop into the syscall registers (reverse order - LIFO). */
    for (int a = n - 1; a >= 0; a--) {
        if (src_regs[a] == linux_syscall_regs[a]) continue;
        if (BE(c)->emit_pop_reg) BE(c)->emit_pop_reg(c, linux_syscall_regs[a]);
    }
    riscv_emit_u32(c, 0x00000073); /* ecall */
    /* Restore the saved registers (reverse order - LIFO). */
    for (size_t r = sizeof(clobbered_live_regs) / sizeof(clobbered_live_regs[0]); r-- > 0; ) {
        if (BE(c)->emit_pop_reg) BE(c)->emit_pop_reg(c, clobbered_live_regs[r]);
    }
}

/* sar dst_reg, src1, src2 (arithmetic shift right: dst = src1 >> src2) */
static void riscv64_emit_sar(ZeroCCCompiler *c, int dst_reg, int src1, int src2) {
    riscv_emit_u32(c, rv64_R(RV_REG(dst_reg), RV_REG(src1), RV_REG(src2), 5, 0x20));  /* sra */
}

/* call [reg] - call through register */
static void riscv64_emit_call_reg(ZeroCCCompiler *c, int reg) {
    /* jalr ra, 0(reg) */
    riscv_emit_u32(c, (uint32_t)(((uint32_t)(reg & 0x1F) << 15) | (1u << 7) | 0x67u));
}

/* stack_align - ensure 16-byte stack alignment (frames are 16-byte aligned) */
static void riscv64_emit_stack_align(ZeroCCCompiler *c, uint8_t alignment) {
    (void)c;
    (void)alignment;
}

/* add_imm dst_reg, imm (add immediate) */
static void riscv64_emit_add_imm(ZeroCCCompiler *c, int dst_reg, int16_t imm) {
    riscv_emit_u32(c, rv64_I(RV_REG(dst_reg), RV_REG(dst_reg), 0, imm, 0x13));
}

/* sub_imm dst_reg, imm (subtract immediate) */
static void riscv64_emit_sub_imm(ZeroCCCompiler *c, int dst_reg, int16_t imm) {
    riscv_emit_u32(c, rv64_I(RV_REG(dst_reg), RV_REG(dst_reg), 0, (int16_t)(-imm), 0x13));
}

/* inc dst_reg (increment) */
static void riscv64_emit_inc(ZeroCCCompiler *c, int dst_reg) {
    riscv_emit_u32(c, rv64_I(RV_REG(dst_reg), RV_REG(dst_reg), 0, 1, 0x13));
}

/* dec dst_reg (decrement) */
static void riscv64_emit_dec(ZeroCCCompiler *c, int dst_reg) {
    riscv_emit_u32(c, rv64_I(RV_REG(dst_reg), RV_REG(dst_reg), 0, -1, 0x13));
}

/* Floating point operations (D extension; FP register numbers are used as-is) */
static void riscv64_emit_load_fp(ZeroCCCompiler *c, int dst_reg, int base_reg, int32_t offset) {
    /* fld fd, offset(base) */
    if (offset >= -2048 && offset <= 2047) {
        riscv_emit_u32(c, rv64_I(RV_REG(dst_reg), RV_REG(base_reg), 3, (int16_t)offset, 0x07));
    } else {
        int scratch = (base_reg != RV64_T0) ? RV64_T0 : RV64_T1;
        riscv64_emit_mov_imm(c, scratch, offset);
        riscv_emit_u32(c, rv64_R(scratch, RV_REG(base_reg), scratch, 0, 0));
        riscv_emit_u32(c, rv64_I(RV_REG(dst_reg), scratch, 3, 0, 0x07));
    }
}

static void riscv64_emit_store_fp(ZeroCCCompiler *c, int src_reg, int base_reg, int32_t offset) {
    /* fsd fs, offset(base) */
    if (offset >= -2048 && offset <= 2047) {
        riscv_emit_u32(c, rv64_S(RV_REG(base_reg), RV_REG(src_reg), 3, (int16_t)offset, 0x27));
    } else {
        int scratch = (base_reg != RV64_T0) ? RV64_T0 : RV64_T1;
        riscv64_emit_mov_imm(c, scratch, offset);
        riscv_emit_u32(c, rv64_R(scratch, RV_REG(base_reg), scratch, 0, 0));
        riscv_emit_u32(c, rv64_S(scratch, RV_REG(src_reg), 3, 0, 0x27));
    }
}

static void riscv64_emit_vloadss(ZeroCCCompiler *c, int dst, int base, int32_t offset) {
    /* flw fd, offset(base) */
    if (offset >= -2048 && offset <= 2047) {
        riscv_emit_u32(c, rv64_I(RV_REG(dst), RV_REG(base), 2, (int16_t)offset, 0x07));
    } else {
        int scratch = (base != RV64_T0) ? RV64_T0 : RV64_T1;
        riscv64_emit_mov_imm(c, scratch, offset);
        riscv_emit_u32(c, rv64_R(scratch, RV_REG(base), scratch, 0, 0));
        riscv_emit_u32(c, rv64_I(RV_REG(dst), scratch, 2, 0, 0x07));
    }
    /* fcvt.d.s fd, fd - widen single to double (double FP model; a raw flw
     * leaves the float bit pattern in the low half of the 64-bit register,
     * i.e. a denormalized double - isa_qemu 07_float). */
    riscv_emit_u32(c, rv64_FP(RV_REG(dst), RV_REG(dst), 0, 0x21, 0));
}

static void riscv64_emit_vstoress(ZeroCCCompiler *c, int src, int base, int32_t offset) {
    /* fcvt.s.d f15, fs then fsw f15, offset(base) - narrow WITH ROUNDING
     * before the 32-bit store (a raw fsw truncates the low 32 bits of the
     * double bit pattern; the low half of the 2.5 double is 0x00000000).
     * f15 is the backend FP scratch (outside the allocatable f0-f7 pool);
     * fcvt.s.d with fs == f15 is a valid in-place narrowing. */
    const int scratch = 15;
    riscv_emit_u32(c, rv64_FP(scratch, RV_REG(src), 1, 0x20, 7));  /* fcvt.s.d f15, fs */
    if (offset >= -2048 && offset <= 2047) {
        riscv_emit_u32(c, rv64_S(RV_REG(base), scratch, 2, (int16_t)offset, 0x27));
    } else {
        int scratch_gpr = (base != RV64_T0) ? RV64_T0 : RV64_T1;
        riscv64_emit_mov_imm(c, scratch_gpr, offset);
        riscv_emit_u32(c, rv64_R(scratch_gpr, RV_REG(base), scratch_gpr, 0, 0));
        riscv_emit_u32(c, rv64_S(scratch_gpr, scratch, 2, 0, 0x27));
    }
}

static void riscv64_emit_load_fp_rodata(ZeroCCCompiler *c, int dst_reg, size_t rodata_offset) {
    /* auipc t0, 0; fld dst, 0(t0) with a string relocation on the fld
     * (the ELF writer attaches R_RISCV_PCREL_HI20 to the auipc).
     * Note: dst_reg is an FP register; auipc requires an integer register (t0). */
    riscv_emit_u32(c, rv64_AUIPC(RV64_T0, 0));
    size_t patch_pos = me_get_offset(c);
    riscv_emit_u32(c, rv64_I(RV_REG(dst_reg), RV64_T0, 3, 0, 0x07));
    if (c->str_reloc_count < ZERO_CC_MAX_STR_RELOCS) {
        c->str_relocs[c->str_reloc_count].text_patch_offset = patch_pos;
        c->str_relocs[c->str_reloc_count].rodata_offset = rodata_offset;
        c->str_relocs[c->str_reloc_count].dst_reg = dst_reg;
        c->str_reloc_count++;
    }
}

static void riscv64_emit_fp_add(ZeroCCCompiler *c, int dst, int src1, int src2) {
    riscv_emit_u32(c, rv64_FP(RV_REG(dst), RV_REG(src1), RV_REG(src2), 0x01, 7));   /* fadd.d */
}

static void riscv64_emit_fp_sub(ZeroCCCompiler *c, int dst, int src1, int src2) {
    riscv_emit_u32(c, rv64_FP(RV_REG(dst), RV_REG(src1), RV_REG(src2), 0x05, 7));   /* fsub.d */
}

static void riscv64_emit_fp_mul(ZeroCCCompiler *c, int dst, int src1, int src2) {
    riscv_emit_u32(c, rv64_FP(RV_REG(dst), RV_REG(src1), RV_REG(src2), 0x09, 7));  /* fmul.d */
}

static void riscv64_emit_fp_div(ZeroCCCompiler *c, int dst, int src1, int src2) {
    riscv_emit_u32(c, rv64_FP(RV_REG(dst), RV_REG(src1), RV_REG(src2), 0x0D, 7));  /* fdiv.d */
}

static void riscv64_emit_fp_neg(ZeroCCCompiler *c, int dst, int src) {
    /* fneg.d fd, fs = fsgnjn.d fd, fs, fs (rm=1) */
    riscv_emit_u32(c, rv64_FP(RV_REG(dst), RV_REG(src), RV_REG(src), 0x11, 1));
}

/* FP compare: no instruction is emitted - the consumer (cond_set/jcond) */
static void riscv64_emit_fp_cmp(ZeroCCCompiler *c, int src1, int src2) {
    (void)c; (void)src1; (void)src2;  /* state recorded by the MC dispatch */
}

static void riscv64_emit_fp_cvtsi(ZeroCCCompiler *c, int int_dst, int fp_src) {
    /* fcvt.l.d rd, fs (round toward zero, rm=1) - 64-bit result, matching the
     * IR contract "CVTTSD2SI yields a 64-bit int" (narrower targets are
     * sign/zero-extended afterwards by the width fixup). */
    riscv_emit_u32(c, rv64_FP(RV_REG(int_dst), RV_REG(fp_src), 2, 0x61, 1));
}

static void riscv64_emit_fp_cvtsf(ZeroCCCompiler *c, int fp_dst, int int_src) {
    /* fcvt.d.w fd, rs (round to nearest even / dynamic rm=7; 32-bit source, as in x86) */
    riscv_emit_u32(c, rv64_FP(RV_REG(fp_dst), RV_REG(int_src), 0, 0x69, 7));
}

static void riscv64_emit_fp_mov(ZeroCCCompiler *c, int dst, int src) {
    if (dst == src) return;
    /* fmv.d fd, fs = fsgnj.d fd, fs, fs (rm=0) */
    riscv_emit_u32(c, rv64_FP(RV_REG(dst), RV_REG(src), RV_REG(src), 0x11, 0));
}

static void riscv64_emit_fp_cvtss2sd(ZeroCCCompiler *c, int dst, int src) {
    /* fcvt.d.s fd, fs (rm=0) - widen single to double (GNU vector per-lane) */
    riscv_emit_u32(c, rv64_FP(RV_REG(dst), RV_REG(src), 0, 0x21, 0));
}

static void riscv64_emit_fp_cvtsd2ss(ZeroCCCompiler *c, int dst, int src) {
    /* fcvt.s.d fd, fs (rs2=1, rm=7) - narrow double to single (GNU vector
     * per-lane). Note the encoding is NOT the mirror of fcvt.d.s: the
     * assembler encodes fcvt.s.d as f7=0x20, rs2=1, f3=7. */
    riscv_emit_u32(c, rv64_FP(RV_REG(dst), RV_REG(src), 1, 0x20, 7));
}

static void riscv64_emit_const(ZeroCCCompiler *c, int64_t value) {
    riscv64_emit_mov_imm(c, 10, value);  /* a0 */
}

static void riscv64_emit_stack_adjust(ZeroCCCompiler *c, int32_t delta) {
    if (delta == 0) return;
    /* addi sp, sp, delta (12-bit range; call-stack deltas are small) */
    riscv_emit_u32(c, rv64_I(RV64_SP, RV64_SP, 0, (int16_t)delta, 0x13));
}

static void riscv64_emit_push_reg(ZeroCCCompiler *c, int reg) {
    riscv_emit_u32(c, rv64_I(RV64_SP, RV64_SP, 0, -8, 0x13));  /* addi sp, sp, -8 */
    riscv_emit_u32(c, rv64_S(RV64_SP, RV_REG(reg), 3, 0, 0x23));  /* sd reg, 0(sp) */
}

static void riscv64_emit_pop_reg(ZeroCCCompiler *c, int reg) {
    riscv_emit_u32(c, rv64_I(RV_REG(reg), RV64_SP, 3, 0, 0x03));  /* ld reg, 0(sp) */
    riscv_emit_u32(c, rv64_I(RV64_SP, RV64_SP, 0, 8, 0x13));      /* addi sp, sp, 8 */
}

static void riscv64_emit_label(ZeroCCCompiler *c, const char *name) {
    (void)c; (void)name;
}



static size_t riscv64_patch_jmp(ZeroCCCompiler *c, size_t patch_offset, size_t target_offset) {
    if (patch_offset + 4 > c->text_section.length) return patch_offset;
    uint32_t *insn = (uint32_t *)((uint8_t *)c->text_section.data + patch_offset);
    int32_t rel = (int32_t)(target_offset - patch_offset);
    uint32_t cur = *insn;
    uint32_t op = cur & 0x7Fu;
    if (op == 0x6Fu) {
        /* J-type (jal): imm[20|10:1|11|19:12] */
        uint32_t u = (uint32_t)rel & 0x1FFFFFu;
        uint32_t imm = ((u & 0x100000u) << 11) | ((u & 0x7FEu) << 20) |
                       ((u & 0x800u) << 9) | (u & 0xFF000u);
        *insn = (cur & 0xFFFu) | imm;  /* keep rd + opcode */
    } else if (op == 0x63u) {
        /* B-type (branch): imm[12|10:5] rs2 rs1 funct3 imm[4:1|11] */
        uint32_t u = (uint32_t)rel & 0x1FFFu;
        uint32_t keep = 0x7Fu | (7u << 12) | (0x1Fu << 15) | (0x1Fu << 20);
        uint32_t imm = ((u & 0x1000u) << 19) | ((u & 0x7E0u) << 20) |
                       ((u & 0x1Eu) << 7) | ((u & 0x800u) >> 4);
        *insn = (cur & keep) | imm;
    }
    return patch_offset;
}



static void riscv64_emit_test(ZeroCCCompiler *c, int src1, int src2) {
    /* No-op on RISC-V: record the value register so emit_jeq/jne can
     * branch on it against zero (mirrors x86 `test src,src; je/jne`). */
    (void)src2;
    c->lowering.cmp_src1 = src1;
}

static void riscv64_emit_cqo(ZeroCCCompiler *c) {
    (void)c;
}

static void riscv64_emit_idiv(ZeroCCCompiler *c, int src) {
    /* div a0, a0, src: opcode 0x33, funct3=4, funct7=1 */
    riscv_emit_u32(c, rv64_R(10, 10, RV_REG(src), 4, 1));
}

static void riscv64_emit_lea(ZeroCCCompiler *c, int dst, int base, int32_t offset) {
    /* addi dst, base, offset */
    if (offset >= -2048 && offset <= 2047) {
        riscv_emit_u32(c, rv64_I(RV_REG(dst), RV_REG(base), 0, (int16_t)offset, 0x13));
    } else {
        int scratch = (base != RV64_T0 && dst != RV64_T0) ? RV64_T0 : RV64_T1;
        riscv64_emit_mov_imm(c, scratch, offset);
        riscv_emit_u32(c, rv64_R(RV_REG(dst), RV_REG(base), RV_REG(scratch), 0, 0));
    }
}

static void riscv64_emit_mod(ZeroCCCompiler *c, int dst, int src1, int src2) {
    /* rem/remu dst, src1, src2: opcode 0x33, funct3=6 (rem) or 7 (remu), funct7=1 */
    int f3 = c->lowering.div_is_unsigned ? 7 : 6;
    riscv_emit_u32(c, rv64_R(RV_REG(dst), RV_REG(src1), RV_REG(src2), f3, 1));
}

/* Core emission functions */

/* mov_reg dst, src - register-to-register move */
static void riscv64_emit_mov_reg(ZeroCCCompiler *c, int dst, int src) {
    if (dst == src) return;
    /* addi dst, src, 0 */
    riscv_emit_u32(c, rv64_I(RV_REG(dst), RV_REG(src), 0, 0, 0x13));
}

/* not dst, src - bitwise NOT */
static void riscv64_emit_not(ZeroCCCompiler *c, int dst, int src) {
    /* xori dst, src, -1 (copy src first when distinct) */
    if (dst != src) {
        riscv_emit_u32(c, rv64_I(RV_REG(dst), RV_REG(src), 0, 0, 0x13));
    }
    riscv_emit_u32(c, rv64_I(RV_REG(dst), RV_REG(dst), 4, -1, 0x13));  /* xori dst, dst, -1 */
}

/* shr dst, src, shift - logical shift right (register shift amount) */
static void riscv64_emit_shr(ZeroCCCompiler *c, int dst, int src1, int src2) {
    /* srl dst, src1, src2 */
    riscv_emit_u32(c, rv64_R(RV_REG(dst), RV_REG(src1), RV_REG(src2), 5, 0));
}

/* Memory operations */

/* deref dst, [src] - pointer dereference (64-bit load on RV64, 32-bit on RV32) */
static void riscv64_emit_deref(ZeroCCCompiler *c, int dst, int src) {
    int f3 = (c->isa == ISA_RISCV32) ? 2 : 3;
    riscv_emit_u32(c, rv64_I(RV_REG(dst), RV_REG(src), f3, 0, 0x03));
}

/* load_32 dst, [base] - 32-bit load from the address in base (offset 0). */
static void riscv64_emit_load_32(ZeroCCCompiler *c, int dst, int base) {
    /* lw dst, 0(base) */
    riscv_emit_u32(c, rv64_I(RV_REG(dst), RV_REG(base), 2, 0, 0x03));
}

/* load_16 dst, [base] - 16-bit load from the address in base (offset 0). */
static void riscv64_emit_load_16(ZeroCCCompiler *c, int dst, int base) {
    /* lh dst, 0(base) */
    riscv_emit_u32(c, rv64_I(RV_REG(dst), RV_REG(base), 1, 0, 0x03));
}

/* load_byte dst, [base] - 8-bit load from the address in base (offset 0). */
static void riscv64_emit_load_byte(ZeroCCCompiler *c, int dst, int base) {
    /* lb dst, 0(base) */
    riscv_emit_u32(c, rv64_I(RV_REG(dst), RV_REG(base), 0, 0, 0x03));
}

/* store_64 src, [base] - 64-bit store to the address in base (offset 0). */
static void riscv64_emit_store_64(ZeroCCCompiler *c, int src, int base) {
    /* sd src, 0(base) (f3=3) */
    riscv_emit_u32(c, rv64_S(RV_REG(base), RV_REG(src), 3, 0, 0x23));
}

/* store_32 src, [base] - 32-bit store to the address in base (offset 0). */
static void riscv64_emit_store_32(ZeroCCCompiler *c, int src, int base) {
    /* sw src, 0(base) */
    riscv_emit_u32(c, rv64_S(RV_REG(base), RV_REG(src), 2, 0, 0x23));
}

/* store_16 src, [base] - 16-bit store to the address in base (offset 0). */
static void riscv64_emit_store_16(ZeroCCCompiler *c, int src, int base) {
    /* sh src, 0(base) */
    riscv_emit_u32(c, rv64_S(RV_REG(base), RV_REG(src), 1, 0, 0x23));
}

/* store_byte src, [base] - 8-bit store to the address in base (offset 0). */
static void riscv64_emit_store_byte(ZeroCCCompiler *c, int src, int base) {
    /* sb src, 0(base) */
    riscv_emit_u32(c, rv64_S(RV_REG(base), RV_REG(src), 0, 0, 0x23));
}

/* lea_rip dst, rodata_offset - address of a .rodata string. RISC-V has no
 * RIP-relative mode: emit auipc dst, 0; addi dst, dst, 0 and attach a
 * string relocation to the addi (the ELF writer pairs it with an
 * R_RISCV_PCREL_HI20 on the auipc). */
static void riscv64_emit_lea_rip(ZeroCCCompiler *c, int dst, size_t rodata_offset) {
    riscv_emit_u32(c, rv64_AUIPC(RV_REG(dst), 0));
    size_t patch_pos = me_get_offset(c);
    riscv_emit_u32(c, rv64_I(RV_REG(dst), RV_REG(dst), 0, 0, 0x13));
    if (c->str_reloc_count < ZERO_CC_MAX_STR_RELOCS) {
        c->str_relocs[c->str_reloc_count].text_patch_offset = patch_pos;
        c->str_relocs[c->str_reloc_count].rodata_offset = rodata_offset;
        c->str_relocs[c->str_reloc_count].dst_reg = dst;
        c->str_reloc_count++;
    }
}

/* Comparisons */

/* cond_set dst, cond - set dst to 1 or 0 based on condition code
 * IR condition codes: CC_LT=0, CC_LE=1, CC_GT=2, CC_GE=3, CC_EQ=4, CC_NE=5
 *
 * For RISC-V, we use SLT with the operands from the previous CMP instruction.
 * The CMP instruction stores the operands in c->lowering.cmp_src1/2.
 *
 * Condition code mapping:
 * - CC_LT: slt dst, src1, src2 (dst = src1 < src2)
 * - CC_LE: slt dst, src2, src1; xnor dst, dst, zero (dst = !(src2 < src1) = src1 <= src2)
 * - CC_GT: slt dst, src2, src1; xnor dst, dst, zero (dst = !(src2 < src1) = src1 > src2)
 * - CC_GE: slt dst, src1, src2; xnor dst, dst, zero (dst = !(src1 < src2) = src1 >= src2)
 * - CC_EQ: seqz dst, src1; seqz dst, dst (src1 == 0)
 * - CC_NE: snez dst, src1; snez dst, dst (src1 != 0)
 */
/* Comparisons (flag materialization).
 *
 * RISC-V has no flags register. The MC dispatch records the operands of
 * the last OP_CMP/OP_CMP_ZERO in c->lowering.cmp_src1/cmp_src2 (plus the
 * FP-class flag cmp_is_fp), and live_range_compute() extends those operand
 * live ranges to the flag consumer. The consumer then re-materializes the
 * predicate straight from the operand registers - no scratch register, so
 * spill reloads or register reuse between producer and consumer cannot
 * corrupt the condition.
 *
 * IR condition codes: CC_LT=0, CC_LE=1, CC_GT=2, CC_GE=3, CC_EQ=4, CC_NE=5
 */

/* Emit the FP predicate for (fs1 OP fs2) into integer register rd. */
static void riscv64_emit_fp_pred(ZeroCCCompiler *c, int rd, int fs1, int fs2, int cond) {
    switch (cond) {
        case 0:  riscv_emit_u32(c, rv64_FP(rd, fs1, fs2, 0x51, 1)); break;  /* flt.d (rm=1) */
        case 1:  riscv_emit_u32(c, rv64_FP(rd, fs1, fs2, 0x51, 0)); break;  /* fle.d (rm=0) */
        case 2:  riscv_emit_u32(c, rv64_FP(rd, fs2, fs1, 0x51, 1)); break;  /* flt.d swapped */
        case 3:  riscv_emit_u32(c, rv64_FP(rd, fs2, fs1, 0x51, 0)); break;  /* fle.d swapped */
        case 4:  riscv_emit_u32(c, rv64_FP(rd, fs1, fs2, 0x51, 2)); break;  /* feq.d (rm=2) */
        default: riscv_emit_u32(c, rv64_FP(rd, fs1, fs2, 0x51, 2));         /* feq.d */
                 riscv_emit_u32(c, rv64_I(rd, rd, 4, 1, 0x13));             /* xori rd, rd, 1 */
    }
}

static void riscv64_emit_cond_set(ZeroCCCompiler *c, int dst, int cond) {
    int src1 = c->lowering.cmp_src1;
    int src2 = c->lowering.cmp_src2;
    if (c->lowering.cmp_is_fp) {
        /* FP compare results write directly into the integer destination */
        riscv64_emit_fp_pred(c, RV_REG(dst), src1, src2, cond);
        return;
    }
    switch (cond) {
        case 0: /* CC_LT: dst = src1 < src2 */
            riscv_emit_u32(c, rv64_R(RV_REG(dst), RV_REG(src1), RV_REG(src2), 2, 0));  /* slt */
            break;
        case 1: /* CC_LE: dst = !(src2 < src1) */
            riscv_emit_u32(c, rv64_R(RV_REG(dst), RV_REG(src2), RV_REG(src1), 2, 0));  /* slt */
            riscv_emit_u32(c, rv64_I(RV_REG(dst), RV_REG(dst), 4, 1, 0x13));           /* xori dst, dst, 1 */
            break;
        case 2: /* CC_GT: dst = src2 < src1 */
            riscv_emit_u32(c, rv64_R(RV_REG(dst), RV_REG(src2), RV_REG(src1), 2, 0));  /* slt */
            break;
        case 3: /* CC_GE: dst = !(src1 < src2) */
            riscv_emit_u32(c, rv64_R(RV_REG(dst), RV_REG(src1), RV_REG(src2), 2, 0));  /* slt */
            riscv_emit_u32(c, rv64_I(RV_REG(dst), RV_REG(dst), 4, 1, 0x13));           /* xori dst, dst, 1 */
            break;
        case 6: /* CC_ULT: dst = (uint)src1 < (uint)src2 */
            riscv_emit_u32(c, rv64_R(RV_REG(dst), RV_REG(src1), RV_REG(src2), 3, 0));  /* sltu */
            break;
        case 7: /* CC_ULE: dst = !((uint)src2 < (uint)src1) */
            riscv_emit_u32(c, rv64_R(RV_REG(dst), RV_REG(src2), RV_REG(src1), 3, 0));  /* sltu */
            riscv_emit_u32(c, rv64_I(RV_REG(dst), RV_REG(dst), 4, 1, 0x13));           /* xori dst, dst, 1 */
            break;
        case 8: /* CC_UGT: dst = (uint)src2 < (uint)src1 */
            riscv_emit_u32(c, rv64_R(RV_REG(dst), RV_REG(src2), RV_REG(src1), 3, 0));  /* sltu */
            break;
        case 9: /* CC_UGE: dst = !((uint)src1 < (uint)src2) */
            riscv_emit_u32(c, rv64_R(RV_REG(dst), RV_REG(src1), RV_REG(src2), 3, 0));  /* sltu */
            riscv_emit_u32(c, rv64_I(RV_REG(dst), RV_REG(dst), 4, 1, 0x13));           /* xori dst, dst, 1 */
            break;
        case 4: /* CC_EQ: dst = (src1 - src2) == 0 (sub into dst, then seqz: sltiu dst, dst, 1) */
            riscv_emit_u32(c, rv64_R(RV_REG(dst), RV_REG(src1), RV_REG(src2), 0, 0x20));  /* sub dst, src1, src2 */
            riscv_emit_u32(c, rv64_I(RV_REG(dst), RV_REG(dst), 3, 1, 0x13));              /* sltiu dst, dst, 1 (seqz) */
            break;
        default: /* CC_NE: dst = (src1 - src2) != 0 (sub into dst, then snez: sltu dst, zero, dst) */
            riscv_emit_u32(c, rv64_R(RV_REG(dst), RV_REG(src1), RV_REG(src2), 0, 0x20));  /* sub dst, src1, src2 */
            riscv_emit_u32(c, rv64_R(RV_REG(dst), 0, RV_REG(dst), 3, 0));                 /* sltu dst, zero, dst (snez) */
            break;
    }
}

/* jcond cond, label - conditional jump based on condition code
 * IR condition codes: CC_LT=0, CC_LE=1, CC_GT=2, CC_GE=3, CC_EQ=4, CC_NE=5
 *
 * For RISC-V, we use Bcc instructions with the operands from the previous CMP.
 * The CMP instruction stores the operands in c->lowering.cmp_src1/2.
 *
 * Condition code mapping:
 * - CC_LT: blt src1, src2, label
 * - CC_LE: ble src1, src2, label
 * - CC_GT: bgt src1, src2, label
 * - CC_GE: bge src1, src2, label
 * - CC_EQ: beq src1, src2, label
 * - CC_NE: bne src1, src2, label
 */
/* jcond cond, label - conditional jump on the last CMP's operands. The
 * patch position points at the emitted branch; the FP form needs a
 * predicate materialization first, so the branch is the second insn. */
static size_t riscv64_emit_jcond(ZeroCCCompiler *c, int cond, const char *label) {
    (void)label;
    int src1 = c->lowering.cmp_src1;
    int src2 = c->lowering.cmp_src2;
    if (c->lowering.cmp_is_fp) {
        riscv64_emit_fp_pred(c, RV64_T0, src1, src2, cond);
        riscv_emit_u32(c, rv64_B(RV64_T0, 0, 0, 8));  /* beq t0, zero, +8 (inverted) */
        size_t patch_pos = me_get_offset(c);
        riscv_emit_u32(c, rv64_J(0, 0));              /* jal zero, 0 */
        return patch_pos;
    }
    switch (cond) {
        case 0:  riscv_emit_u32(c, rv64_B(src1, src2, 5, 8)); break;  /* blt -> bge a, b, +8 */
        case 1:  riscv_emit_u32(c, rv64_B(src2, src1, 4, 8)); break;  /* ble -> blt b, a, +8 */
        case 2:  riscv_emit_u32(c, rv64_B(src2, src1, 5, 8)); break;  /* bgt -> bge b, a, +8 */
        case 3:  riscv_emit_u32(c, rv64_B(src1, src2, 4, 8)); break;  /* bge -> blt a, b, +8 */
        case 4:  riscv_emit_u32(c, rv64_B(src1, src2, 1, 8)); break;  /* beq -> bne a, b, +8 */
        default: riscv_emit_u32(c, rv64_B(src1, src2, 0, 8)); break;  /* bne -> beq a, b, +8 */
    }
    size_t patch_pos = me_get_offset(c);
    riscv_emit_u32(c, rv64_J(0, 0));                  /* jal zero, 0 */
    return patch_pos;
}

/* setl/setle/setg/setge/sete/setne dst - materialize the last CMP's
 * predicate into dst (1/0). */
static void riscv64_emit_setl(ZeroCCCompiler *c, int dst) {
    riscv64_emit_cond_set(c, dst, 0);
}

static void riscv64_emit_setle(ZeroCCCompiler *c, int dst) {
    riscv64_emit_cond_set(c, dst, 1);
}

static void riscv64_emit_setg(ZeroCCCompiler *c, int dst) {
    riscv64_emit_cond_set(c, dst, 2);
}

static void riscv64_emit_setge(ZeroCCCompiler *c, int dst) {
    riscv64_emit_cond_set(c, dst, 3);
}

static void riscv64_emit_sete(ZeroCCCompiler *c, int dst) {
    riscv64_emit_cond_set(c, dst, 4);
}

static void riscv64_emit_setne(ZeroCCCompiler *c, int dst) {
    riscv64_emit_cond_set(c, dst, 5);
}

/* cmp_zero src - compare to zero (state recorded by the MC dispatch,
 * with cmp_src2 = zero). */
static void riscv64_emit_cmp_zero(ZeroCCCompiler *c, int src) {
    (void)c; (void)src;
}

/* lnot dst, src - logical NOT (dst = !src) */
static void riscv64_emit_lnot(ZeroCCCompiler *c, int dst, int src) {
    /* seqz dst, src: sltiu dst, src, 1 */
    riscv_emit_u32(c, rv64_I(RV_REG(dst), RV_REG(src), 3, 1, 0x13));
}

/* Control flow */

/* Function ABI */

/* param_save_int reg_idx, offset - save incoming integer parameter a0..a7 to -offset(s0) (or SP+delta) */
static void riscv64_emit_param_save_int(ZeroCCCompiler *c, int reg_idx, int slot) {
    if (reg_idx < 0 || reg_idx >= 8) return;
    int reg = 10 + reg_idx;  /* a0..a7 = x10..x17 */
    int f3 = (c->isa == ISA_RISCV32) ? 2 : 3;  /* sw (32-bit) vs sd (64-bit) */
    int32_t off = zero_cc_frame_disp(c, -slot);
    int base = zero_cc_frame_base_reg(c);
    if (off >= -2048 && off <= 2047) {
        riscv_emit_u32(c, rv64_S(base, reg, f3, (int16_t)off, 0x23));
    } else {
        int scratch = (base != RV64_T0 && reg != RV64_T0) ? RV64_T0 : RV64_T1;
        riscv64_emit_mov_imm(c, scratch, off);
        riscv_emit_u32(c, rv64_R(scratch, base, scratch, 0, 0));
        riscv_emit_u32(c, rv64_S(scratch, reg, f3, 0, 0x23));
    }
}

/* param_save_fp reg_idx, offset - save incoming FP parameter fa0..fa7 to -offset(s0) (or SP+delta) */
static void riscv64_emit_param_save_fp(ZeroCCCompiler *c, int reg_idx, int slot, int is_single) {
    if (reg_idx < 0 || reg_idx >= 8) return;
    /* The caller packs FP arguments into conv->fp_arg_regs (f10-f17 in
     * zerocc's RISC-V table) - map the index through it, like the
     * aarch64/x86 backends do (a raw reg_idx saved f0-f7, i.e. garbage,
     * for every non-promoted FP parameter). */
    int src_reg = BE(c)->conv->fp_arg_regs[reg_idx];
    int base = zero_cc_frame_base_reg(c);
    int32_t disp = zero_cc_frame_disp(c, -slot);
    if (is_single && c->isa != ISA_RISCV32) {
        riscv64_emit_vstoress(c, src_reg, base, disp);
    } else {
        riscv64_emit_store_fp(c, src_reg, base, disp);
    }
}

/* variadic_spill - spill ALL argument registers (a0..a7, then fa0..fa7) into
 * the 16-slot save area. va_start points the streams just past the slots
 * consumed by named parameters, so the spill is unconditional. (Was a
 * no-op: variadic args were never accessible via va_arg. RV32 keeps the
 * no-op: the 8-byte slot model assumes LP64 pointers/args.) */
static void riscv64_emit_variadic_spill(ZeroCCCompiler *c, int param_count, int32_t base_offset) {
    (void)param_count;
    if (c->isa == ISA_RISCV32) return;
    int base = zero_cc_frame_base_reg(c);
    int gp_regs = BE(c)->conv->int_arg_reg_count;
    int fp_regs = BE(c)->conv->fp_arg_reg_count;
    for (int i = 0; i < gp_regs; i++) {
        int r = BE(c)->conv->int_arg_regs[i];
        riscv64_emit_store(c, r, base, -(base_offset + (int32_t)i * 8));
    }
    for (int i = 0; i < fp_regs; i++) {
        int r = BE(c)->conv->fp_arg_regs[i];
        riscv64_emit_store_fp(c, r, base, -(base_offset + (int32_t)(gp_regs + i) * 8));
    }
}

/* variadic_al_setup - setup for variadic arguments */
static void riscv64_emit_variadic_al_setup(ZeroCCCompiler *c, int num_fp_args) {
    (void)c; (void)num_fp_args;
}

/* Special operations */

/* vla_alloc dst, size - variable length array allocation */
static void riscv64_emit_vla_alloc(ZeroCCCompiler *c, int dst_reg, int size_reg) {
    /* sub sp, sp, size */
    riscv_emit_u32(c, 0x400001b3 | (2 << 7) | (2 << 15) | (size_reg << 20));
    if (dst_reg != size_reg) {
        riscv64_emit_mov_reg(c, dst_reg, size_reg);
    }
}

/* atomic_cmpxchg dst, addr, expected, desired, width - atomic compare and swap */
static void riscv64_emit_atomic_cmpxchg(ZeroCCCompiler *c, int dst, int addr, int expected, int desired, int width) {
    /* lr/sc loop */
    (void)width;
    riscv_emit_u32(c, 0x1000202F | (addr << 15) | dst);  /* lr.w dst, (addr) */
    riscv_emit_u32(c, 0x02000033 | (dst << 7) | (dst << 15) | (expected << 20));  /* bne check */
    riscv_emit_u32(c, 0x1000202F | (addr << 15) | (desired << 20));  /* sc.w */
}

/* atomic_xadd dst, addr, val, width - atomic add and swap */
static void riscv64_emit_atomic_xadd(ZeroCCCompiler *c, int dst, int addr, int val, int width) {
    /* lr/sc loop with add */
    (void)width;
    riscv_emit_u32(c, 0x1000202F | (addr << 15) | dst);  /* lr.w dst, (addr) */
    riscv_emit_u32(c, 0x00000033 | (dst << 7) | (dst << 15) | (val << 20));  /* add */
    riscv_emit_u32(c, 0x1000202F | (addr << 15) | (dst << 20));  /* sc.w */
}

/* atomic_xchg dst, addr, val, width - atomic exchange */
static void riscv64_emit_atomic_xchg(ZeroCCCompiler *c, int dst, int addr, int val, int width) {
    /* lr/sc loop */
    (void)width;
    riscv_emit_u32(c, 0x1000202F | (addr << 15) | dst);  /* lr.w dst, (addr) */
    riscv_emit_u32(c, 0x1000202F | (addr << 15) | (val << 20));  /* sc.w val, (addr) */
}

/* mfence - memory fence */
static void riscv64_emit_mfence(ZeroCCCompiler *c) {
    /* fence iorw, iorw */
    riscv_emit_u32(c, 0x0000000F);
}

/* __builtin_trap / __builtin_unreachable */
static void riscv64_emit_trap(ZeroCCCompiler *c) {
    /* unimp (illegal instruction) */
    riscv_emit_u32(c, 0x00002007);
}

static void riscv64_emit_trap_asm(ZeroCCCompiler *c, FILE *out) {
    (void)c;
    fprintf(out, "\tunimp\n");
}

/* RISC-V 64-bit backend instance */

/* RISC-V VREG mapping: map virtual register numbers to RISC-V encodings
 * VREG_0 (result) -> a0 (x10)
 * VREG_1-7 (temps/args) -> a1-a7 (x11-x17)
 * VREG_8-15 (temps) -> t0-t7 (x5-x7, x28-x30)
 * Special: use BE(c)->fp_reg (s0/x8) and BE(c)->sp_reg (sp/x2)
 */

ZeroCCISABackend riscv64_backend = {  /* writable: ASM emit pointers are filled in lazily */
    .isa = ISA_RISCV64,
    .regs = &riscv_regs,
    .conv = &riscv64_conv,
    .fp_reg = 8,   /* s0 */
    .sp_reg = 2,   /* sp */
    .scratch_reg1 = 5,                         /* t0 */
    .scratch_reg2 = 6,                         /* t1 */
    .fp_scratch_reg1 = REGALLOC_FP_SCRATCH1,   /* f15 - outside XMM pool 0-7 */
    .fp_scratch_reg2 = REGALLOC_FP_SCRATCH2,     /* f14 - outside XMM pool 0-7 */
    .callee_save_frame_reserve = 96,           /* 12 callee-saved regs: s0-s11 */

    .emit_prologue = riscv64_emit_prologue,
    .emit_leave = riscv64_emit_leave,
    .emit_epilogue = riscv64_emit_epilogue,
    .get_code_offset = riscv64_get_code_offset,

    .emit_prologue_asm = riscv_emit_prologue_asm,
    .emit_epilogue_asm = riscv_emit_epilogue_asm,
    .get_hw_reg_name = riscv_get_hw_reg_name,
    .get_vec_reg_name = riscv_get_vec_reg_name,

    .emit_mov_imm = riscv64_emit_mov_imm,
    .emit_mov_reg = riscv64_emit_mov_reg,
    .emit_add = riscv64_emit_add,
    .emit_sub = riscv64_emit_sub,
    .emit_mul = riscv64_emit_mul,
    .emit_div = riscv64_emit_div,
    .emit_mod = riscv64_emit_mod,
    .emit_and = riscv64_emit_and,
    .emit_or = riscv64_emit_or,
    .emit_xor = riscv64_emit_xor,
    .emit_not = riscv64_emit_not,
    .emit_shl = riscv64_emit_shl,
    .emit_shr = riscv64_emit_shr,
    .emit_cmp = riscv64_emit_cmp,
    .emit_cond_set = riscv64_emit_cond_set,
    .emit_jcond = riscv64_emit_jcond,
    .emit_neg = riscv64_emit_neg,
    .emit_extend = riscv64_emit_extend,
    .emit_test = riscv64_emit_test,
    .emit_cqo = riscv64_emit_cqo,
    .emit_idiv = riscv64_emit_idiv,
    .emit_lea = riscv64_emit_lea,
    .patch_jmp = riscv64_patch_jmp,
    .resolve_labels = NULL,

    .emit_load = riscv64_emit_load,
    .emit_store = riscv64_emit_store,
    .emit_vloadss = riscv64_emit_vloadss,
    .emit_vloadsd = riscv64_emit_load_fp,
    .emit_vstoress = riscv64_emit_vstoress,
    .emit_vstoredsd = riscv64_emit_store_fp,
    .emit_deref = riscv64_emit_deref,
    .emit_load_32 = riscv64_emit_load_32,
    .emit_load_16 = riscv64_emit_load_16,
    .emit_load_byte = riscv64_emit_load_byte,
    .emit_store_64 = riscv64_emit_store_64,
    .emit_store_32 = riscv64_emit_store_32,
    .emit_store_16 = riscv64_emit_store_16,
    .emit_store_byte = riscv64_emit_store_byte,
    .emit_lea_rip = riscv64_emit_lea_rip,

    .emit_jmp = riscv64_emit_jmp,
    .emit_jeq = riscv64_emit_jeq,
    .emit_jne = riscv64_emit_jne,
    .emit_jlt = riscv64_emit_jlt,
    .emit_jle = riscv64_emit_jle,
    .emit_jgt = riscv64_emit_jgt,
    .emit_jge = riscv64_emit_jge,
    .emit_call = riscv64_emit_call,
    .emit_call_reg = riscv64_emit_call_reg,
    .emit_ret = riscv64_emit_ret,
    .emit_syscall = riscv_emit_syscall,

    .emit_setl = riscv64_emit_setl,
    .emit_setle = riscv64_emit_setle,
    .emit_setg = riscv64_emit_setg,
    .emit_setge = riscv64_emit_setge,
    .emit_sete = riscv64_emit_sete,
    .emit_setne = riscv64_emit_setne,
    .emit_cmp_zero = riscv64_emit_cmp_zero,
    .emit_lnot = riscv64_emit_lnot,

    .emit_param_save_int = riscv64_emit_param_save_int,
    .emit_param_save_fp = riscv64_emit_param_save_fp,
    .emit_variadic_spill = riscv64_emit_variadic_spill,
    .emit_variadic_al_setup = riscv64_emit_variadic_al_setup,

    .emit_vla_alloc = riscv64_emit_vla_alloc,
    .emit_atomic_cmpxchg = riscv64_emit_atomic_cmpxchg,
    .emit_atomic_xadd = riscv64_emit_atomic_xadd,
    .emit_atomic_xchg = riscv64_emit_atomic_xchg,
    .emit_mfence = riscv64_emit_mfence,
    .emit_trap = riscv64_emit_trap,
    .emit_trap_asm = riscv64_emit_trap_asm,

    .get_int_reg = riscv_get_int_reg,
    .get_fp_reg = riscv_get_fp_reg,

    .emit_load_fp = riscv64_emit_load_fp,
    .emit_store_fp = riscv64_emit_store_fp,
    .emit_load_fp_rodata = riscv64_emit_load_fp_rodata,
    .emit_fp_add = riscv64_emit_fp_add,
    .emit_fp_sub = riscv64_emit_fp_sub,
    .emit_fp_mul = riscv64_emit_fp_mul,
    .emit_fp_div = riscv64_emit_fp_div,
    .emit_fp_neg = riscv64_emit_fp_neg,
    .emit_fp_cmp = riscv64_emit_fp_cmp,
    .emit_fp_cvtsi = riscv64_emit_fp_cvtsi,
    .emit_fp_cvtsf = riscv64_emit_fp_cvtsf,
    .emit_fp_mov = riscv64_emit_fp_mov,

    .emit_const = riscv64_emit_const,
    .emit_stack_adjust = riscv64_emit_stack_adjust,
    .emit_stack_align = riscv64_emit_stack_align,
    .emit_push_reg = riscv64_emit_push_reg,
    .emit_pop_reg = riscv64_emit_pop_reg,
    .emit_add_imm = riscv64_emit_add_imm,
    .emit_sub_imm = riscv64_emit_sub_imm,
    .emit_inc = riscv64_emit_inc,
    .emit_dec = riscv64_emit_dec,
    .emit_sar = riscv64_emit_sar,
    .emit_label = riscv64_emit_label,

    /* Assembly emission for IR instructions (Option A) */
    .emit_mov_imm_asm = riscv_emit_mov_imm_asm,
    .emit_mov_reg_asm = riscv_emit_mov_reg_asm,
    .emit_load_asm = riscv_emit_load_asm,
    .emit_store_asm = riscv_emit_store_asm,
    .emit_add_asm = riscv_emit_add_asm,
    .emit_sub_asm = riscv_emit_sub_asm,
    .emit_mul_asm = riscv_emit_mul_asm,
    .emit_div_asm = riscv_emit_div_asm,
    .emit_and_asm = riscv_emit_and_asm,
    .emit_or_asm = riscv_emit_or_asm,
    .emit_xor_asm = riscv_emit_xor_asm,
    .emit_shl_asm = riscv_emit_shl_asm,
    .emit_shr_asm = riscv_emit_shr_asm,
    .emit_sar_asm = riscv_emit_sar_asm,
    .emit_extend_asm = riscv_emit_extend_asm,
    .emit_cmp_asm = riscv_emit_cmp_asm,
    .emit_cond_set_asm = riscv_emit_cond_set_asm,
    .emit_neg_asm = riscv_emit_neg_asm,
    .emit_lea_asm = riscv_emit_lea_asm,
    .emit_deref_asm = riscv_emit_deref_asm,
    .emit_call_asm = riscv_emit_call_asm,
    .emit_ret_asm = riscv_emit_ret_asm,
    .emit_label_asm = riscv_emit_label_asm,
    .emit_jmp_asm = riscv_emit_jmp_asm,
    .emit_jeq_asm = riscv_emit_jeq_asm,
    .emit_jne_asm = riscv_emit_jne_asm,
    .emit_jlt_asm = riscv_emit_jlt_asm,
    .emit_jle_asm = riscv_emit_jle_asm,
    .emit_jgt_asm = riscv_emit_jgt_asm,
    .emit_jge_asm = riscv_emit_jge_asm,
    .emit_load_fp_asm = riscv_emit_load_fp_asm,
    .emit_store_fp_asm = riscv_emit_store_fp_asm,
    .emit_fp_add_asm = riscv_emit_fp_add_asm,
    .emit_fp_sub_asm = riscv_emit_fp_sub_asm,
    .emit_fp_mul_asm = riscv_emit_fp_mul_asm,
    .emit_fp_div_asm = riscv_emit_fp_div_asm,
    .emit_fp_mov_asm = riscv_emit_fp_mov_asm,
    .emit_vla_alloc_asm = riscv_emit_vla_alloc_asm,
    .emit_param_save_int_asm = riscv_emit_param_save_int_asm,
    .emit_param_save_fp_asm = riscv_emit_param_save_fp_asm,
};

/* RISC-V 32-bit backend (for ESP32-C3/C6/H2, rv32gc) */
static void riscv32_emit_prologue(ZeroCCCompiler *c, size_t stack_size, uint32_t callee_save_mask) {
    if (c->lowering.is_frameless) return;
    (void)callee_save_mask;
    size_t aligned = (stack_size + 15) & ~(size_t)15;
    size_t total = aligned + 16;
    int big = (total > 2047);
    if (big) {
        riscv64_emit_mov_imm(c, RV64_T0, (int64_t)total);
        riscv_emit_u32(c, rv64_R(RV64_SP, RV64_SP, RV64_T0, 0, 0x20)); /* sub sp, sp, t0 */
        if (aligned > 2047) {
            riscv64_emit_mov_imm(c, 6 /* t1 */, (int64_t)aligned);
            riscv_emit_u32(c, rv64_R(6 /* t1 */, RV64_SP, 6 /* t1 */, 0, 0)); /* add t1, sp, t1 */
        } else {
            riscv_emit_u32(c, rv64_I(6 /* t1 */, RV64_SP, 0, (int16_t)aligned, 0x13));
        }
        riscv_emit_u32(c, rv64_S(6 /* t1 */, RV64_RA, 2, 4, 0x23)); /* sw ra, 4(t1) */
        riscv_emit_u32(c, rv64_S(6 /* t1 */, RV64_S0, 2, 0, 0x23)); /* sw s0, 0(t1) */
        riscv_emit_u32(c, rv64_R(RV64_S0, 6 /* t1 */, 0, 0, 0));    /* add s0, t1, zero */
    } else {
        riscv_emit_u32(c, rv64_I(RV64_SP, RV64_SP, 0, (int16_t)(-(int32_t)total), 0x13));
        riscv_emit_u32(c, rv64_S(RV64_SP, RV64_RA, 2, (int16_t)(aligned + 4), 0x23)); /* sw ra, aligned+4(sp) */
        riscv_emit_u32(c, rv64_S(RV64_SP, RV64_S0, 2, (int16_t)aligned, 0x23));       /* sw s0, aligned(sp) */
        riscv_emit_u32(c, rv64_I(RV64_S0, RV64_SP, 0, (int16_t)aligned, 0x13));       /* addi s0, sp, aligned */
    }
}

static void riscv32_emit_leave(ZeroCCCompiler *c) {
    if (c->lowering.is_frameless) return;
    riscv_emit_u32(c, rv64_I(RV64_SP, RV64_S0, 0, 0, 0x13));  /* addi sp, s0, 0 */
    riscv_emit_u32(c, rv64_I(RV64_RA, RV64_SP, 2, 4, 0x03));  /* lw ra, 4(sp) */
    riscv_emit_u32(c, rv64_I(RV64_S0, RV64_SP, 2, 0, 0x03));  /* lw s0, 0(sp) */
    riscv_emit_u32(c, rv64_I(RV64_SP, RV64_SP, 0, 16, 0x13)); /* addi sp, sp, 16 */
}

static void riscv32_emit_epilogue(ZeroCCCompiler *c) {
    riscv32_emit_leave(c);
    riscv_emit_u32(c, 0x00008067); /* ret: jalr zero, 0(ra) */
}

static void riscv32_emit_load(ZeroCCCompiler *c, int dst_reg, int base_reg, int32_t offset) {
    riscv_emit_u32(c, rv64_I(RV_REG(dst_reg), RV_REG(base_reg), 2, (int16_t)offset, 0x03));
}

static void riscv32_emit_store(ZeroCCCompiler *c, int src_reg, int base_reg, int32_t offset) {
    riscv_emit_u32(c, rv64_S(RV_REG(base_reg), RV_REG(src_reg), 2, (int16_t)offset, 0x23));
}

static void riscv32_emit_push_reg(ZeroCCCompiler *c, int reg) {
    riscv_emit_u32(c, rv64_I(RV64_SP, RV64_SP, 0, -4, 0x13));     /* addi sp, sp, -4 */
    riscv_emit_u32(c, rv64_S(RV64_SP, RV_REG(reg), 2, 0, 0x23));  /* sw reg, 0(sp) */
}

static void riscv32_emit_pop_reg(ZeroCCCompiler *c, int reg) {
    riscv_emit_u32(c, rv64_I(RV_REG(reg), RV64_SP, 2, 0, 0x03));  /* lw reg, 0(sp) */
    riscv_emit_u32(c, rv64_I(RV64_SP, RV64_SP, 0, 4, 0x13));      /* addi sp, sp, 4 */
}

/* RISC-V 32-bit backend instance */
ZeroCCISABackend riscv32_backend = {  /* writable: ASM emit pointers are filled in lazily */
    .isa = ISA_RISCV32,
    .regs = &riscv_regs,
    .conv = &riscv32_conv,
    .fp_reg = 8,
    .sp_reg = 2,
    .scratch_reg1 = 5,                         /* t0 (non-argument/return) */
    .scratch_reg2 = 6,                         /* t1 (non-argument/return) */
    .fp_scratch_reg1 = REGALLOC_FP_SCRATCH1,   /* f15 - outside XMM pool 0-7 */
    .fp_scratch_reg2 = REGALLOC_FP_SCRATCH2,   /* f14 - outside XMM pool 0-7 */
    .callee_save_frame_reserve = 96,           /* 12 callee-saved regs: s0-s11 */

    .emit_prologue = riscv32_emit_prologue,
    .emit_leave = riscv32_emit_leave,
    .emit_epilogue = riscv32_emit_epilogue,
    .get_code_offset = riscv64_get_code_offset,

    /* Assembly emission (shared with RV64) */
    .emit_prologue_asm = riscv_emit_prologue_asm,
    .emit_epilogue_asm = riscv_emit_epilogue_asm,
    .get_hw_reg_name = riscv_get_hw_reg_name,
    .get_vec_reg_name = riscv_get_vec_reg_name,

    .emit_mov_imm = riscv64_emit_mov_imm,
    .emit_add = riscv64_emit_add,
    .emit_sub = riscv64_emit_sub,
    .emit_mul = riscv64_emit_mul,
    .emit_div = riscv64_emit_div,
    .emit_mod = riscv64_emit_mod,
    .emit_and = riscv64_emit_and,
    .emit_or = riscv64_emit_or,
    .emit_xor = riscv64_emit_xor,
    .emit_shl = riscv64_emit_shl,
    .emit_cmp = riscv64_emit_cmp,
    .emit_cond_set = riscv64_emit_cond_set,
    .emit_jcond = riscv64_emit_jcond,
    .emit_neg = riscv64_emit_neg,
    .emit_test = riscv64_emit_test,
    .emit_cqo = riscv64_emit_cqo,
    .emit_idiv = riscv64_emit_idiv,
    .emit_lea = riscv64_emit_lea,
    .patch_jmp = riscv64_patch_jmp,
    .resolve_labels = NULL,  /* No label resolution needed for now */

    /* VREG mapping */
    .get_int_reg = riscv_get_int_reg,
    .get_fp_reg = riscv_get_fp_reg,

    .emit_load = riscv32_emit_load,
    .emit_store = riscv32_emit_store,
    .emit_vloadss = riscv64_emit_vloadss,
    .emit_vloadsd = riscv64_emit_load_fp,
    .emit_vstoress = riscv64_emit_vstoress,
    .emit_vstoredsd = riscv64_emit_store_fp,

    .emit_jmp = riscv64_emit_jmp,
    .emit_jeq = riscv64_emit_jeq,
    .emit_jne = riscv64_emit_jne,
    .emit_jlt = riscv64_emit_jlt,
    .emit_jle = riscv64_emit_jle,
    .emit_jgt = riscv64_emit_jgt,
    .emit_jge = riscv64_emit_jge,
    .emit_call = riscv64_emit_call,
    .emit_call_reg = riscv64_emit_call_reg,
    .emit_ret = riscv64_emit_ret,
    .emit_syscall = riscv_emit_syscall,

    .emit_load_fp = riscv64_emit_load_fp,
    .emit_store_fp = riscv64_emit_store_fp,
    .emit_load_fp_rodata = riscv64_emit_load_fp_rodata,
    .emit_fp_add = riscv64_emit_fp_add,
    .emit_fp_sub = riscv64_emit_fp_sub,
    .emit_fp_mul = riscv64_emit_fp_mul,
    .emit_fp_div = riscv64_emit_fp_div,
    .emit_fp_neg = riscv64_emit_fp_neg,
    .emit_fp_cmp = riscv64_emit_fp_cmp,
    .emit_fp_cvtsi = riscv64_emit_fp_cvtsi,
    .emit_fp_cvtsf = riscv64_emit_fp_cvtsf,
    .emit_fp_mov = riscv64_emit_fp_mov,

    .emit_const = riscv64_emit_const,
    .emit_stack_adjust = riscv64_emit_stack_adjust,
    .emit_stack_align = riscv64_emit_stack_align,
    .emit_push_reg = riscv32_emit_push_reg,
    .emit_pop_reg = riscv32_emit_pop_reg,
    .emit_add_imm = riscv64_emit_add_imm,
    .emit_sub_imm = riscv64_emit_sub_imm,
    .emit_inc = riscv64_emit_inc,
    .emit_dec = riscv64_emit_dec,
    .emit_sar = riscv64_emit_sar,
    .emit_label = riscv64_emit_label,

    /* Shared with RV64 for 32-bit operations */
    .emit_mov_reg = riscv64_emit_mov_reg,
    .emit_shr = riscv64_emit_shr,
    .emit_not = riscv64_emit_not,
    .emit_extend = riscv64_emit_extend,
    .emit_cmp_zero = riscv64_emit_cmp_zero,
    .emit_lnot = riscv64_emit_lnot,
    .emit_setl = riscv64_emit_setl,
    .emit_setle = riscv64_emit_setle,
    .emit_setg = riscv64_emit_setg,
    .emit_setge = riscv64_emit_setge,
    .emit_sete = riscv64_emit_sete,
    .emit_setne = riscv64_emit_setne,
    .emit_deref = riscv64_emit_deref,
    .emit_load_byte = riscv64_emit_load_byte,
    .emit_load_16 = riscv64_emit_load_16,
    .emit_load_32 = riscv64_emit_load_32,
    .emit_store_byte = riscv64_emit_store_byte,
    .emit_store_16 = riscv64_emit_store_16,
    .emit_store_32 = riscv64_emit_store_32,
    .emit_vla_alloc = riscv64_emit_vla_alloc,
    .emit_atomic_cmpxchg = riscv64_emit_atomic_cmpxchg,
    .emit_atomic_xadd = riscv64_emit_atomic_xadd,
    .emit_atomic_xchg = riscv64_emit_atomic_xchg,
    .emit_mfence = riscv64_emit_mfence,
    .emit_trap = riscv64_emit_trap,
    .emit_trap_asm = riscv64_emit_trap_asm,
    .emit_param_save_int = riscv64_emit_param_save_int,
    .emit_param_save_fp = riscv64_emit_param_save_fp,
    .emit_variadic_spill = riscv64_emit_variadic_spill,
    .emit_variadic_al_setup = riscv64_emit_variadic_al_setup,
    .emit_lea_rip = riscv64_emit_lea_rip,

    /* Assembly emission for IR instructions (shared with RV64) */
    .emit_mov_imm_asm = riscv_emit_mov_imm_asm,
    .emit_mov_reg_asm = riscv_emit_mov_reg_asm,
    .emit_load_asm = riscv_emit_load_asm,
    .emit_store_asm = riscv_emit_store_asm,
    .emit_add_asm = riscv_emit_add_asm,
    .emit_sub_asm = riscv_emit_sub_asm,
    .emit_mul_asm = riscv_emit_mul_asm,
    .emit_div_asm = riscv_emit_div_asm,
    .emit_and_asm = riscv_emit_and_asm,
    .emit_or_asm = riscv_emit_or_asm,
    .emit_xor_asm = riscv_emit_xor_asm,
    .emit_shl_asm = riscv_emit_shl_asm,
    .emit_shr_asm = riscv_emit_shr_asm,
    .emit_sar_asm = riscv_emit_sar_asm,
    .emit_extend_asm = riscv_emit_extend_asm,
    .emit_cmp_asm = riscv_emit_cmp_asm,
    .emit_cond_set_asm = riscv_emit_cond_set_asm,
    .emit_neg_asm = riscv_emit_neg_asm,
    .emit_add_imm_asm = riscv_emit_add_imm_asm,
    .emit_sub_imm_asm = riscv_emit_sub_imm_asm,
    .emit_inc_asm = riscv_emit_inc_asm,
    .emit_dec_asm = riscv_emit_dec_asm,
    .emit_not_asm = riscv_emit_not_asm,
    .emit_lea_asm = riscv_emit_lea_asm,
    .emit_deref_asm = riscv_emit_deref_asm,
    .emit_call_asm = riscv_emit_call_asm,
    .emit_ret_asm = riscv_emit_ret_asm,
    .emit_label_asm = riscv_emit_label_asm,
    .emit_jmp_asm = riscv_emit_jmp_asm,
    .emit_jeq_asm = riscv_emit_jeq_asm,
    .emit_jne_asm = riscv_emit_jne_asm,
    .emit_jlt_asm = riscv_emit_jlt_asm,
    .emit_jle_asm = riscv_emit_jle_asm,
    .emit_jgt_asm = riscv_emit_jgt_asm,
    .emit_jge_asm = riscv_emit_jge_asm,
    .emit_load_fp_asm = riscv_emit_load_fp_asm,
    .emit_store_fp_asm = riscv_emit_store_fp_asm,
    .emit_fp_add_asm = riscv_emit_fp_add_asm,
    .emit_fp_sub_asm = riscv_emit_fp_sub_asm,
    .emit_fp_mul_asm = riscv_emit_fp_mul_asm,
    .emit_fp_div_asm = riscv_emit_fp_div_asm,
    .emit_fp_mov_asm = riscv_emit_fp_mov_asm,
    .emit_fp_cvtsf_asm = riscv_emit_fp_cvtsf_asm,
    .emit_fp_cvtsi_asm = riscv_emit_fp_cvtsi_asm,
    .emit_vla_alloc_asm = riscv_emit_vla_alloc_asm,
    .emit_param_save_int_asm = riscv_emit_param_save_int_asm,
    .emit_param_save_fp_asm = riscv_emit_param_save_fp_asm,

    /* Vector instruction emission (RISC-V doesn't have vector yet) */
    .emit_vload_asm = NULL,
    .emit_vstore_asm = NULL,
    .emit_vaddss_asm = NULL,
    .emit_vsubss_asm = NULL,
    .emit_vmulss_asm = NULL,
    .emit_vdivss_asm = NULL,
    .emit_vaddps_asm = NULL,
    .emit_vsubps_asm = NULL,
    .emit_vmulps_asm = NULL,
    .emit_vdivps_asm = NULL,
    .emit_vaddsd_asm = NULL,
    .emit_vsubsd_asm = NULL,
    .emit_vmulsd_asm = NULL,
    .emit_vdivsd_asm = NULL,
    .emit_vaddpd_asm = NULL,
    .emit_vsubpd_asm = NULL,
    .emit_vmulpd_asm = NULL,
    .emit_vdivpd_asm = NULL,
    .emit_vpaddd_asm = NULL,
    .emit_vpsubd_asm = NULL,
    .emit_vpmulld_asm = NULL,
    .emit_vmovss_asm = NULL,
    .emit_vmovsd_asm = NULL,
    .emit_vmovaps_asm = NULL,
    .emit_vmovapd_asm = NULL,
    .emit_vandps_asm = NULL,
    .emit_vorps_asm = NULL,
    .emit_vxorps_asm = NULL,
    .emit_vshufps_asm = NULL,
    .emit_vcvt_si2ss_asm = NULL,
    .emit_vcvt_ss2si_asm = NULL,
    .emit_cvtss2sd_asm = NULL,
    .emit_cvtsd2ss_asm = NULL,
    .emit_cvtss2sd = riscv64_emit_fp_cvtss2sd,
    .emit_cvtsd2ss = riscv64_emit_fp_cvtsd2ss,
    .emit_vcmpss_asm = NULL,
    .emit_vcmpsd_asm = NULL,
};
