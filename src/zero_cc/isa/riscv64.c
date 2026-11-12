#include <stdint.h>
#include "zero_cc/isa/isa_backend.h"
#include "zero_cc/isa/tramp_stub.h"
#include "zero_cc/ir/machine_emitter.h"
#include "zero_cc/ir/asm_emitter.h"
#include "zero_cc/isa/riscv_asm.c"  /* Include assembly emission functions */

/* Shared encoders/helpers live in riscv_common.h (Sprint 114 split). */
#include "zero_cc/isa/riscv_common.h"

/* RISC-V RV64 backend implementation */
void riscv64_emit_mov_imm(ZeroCCCompiler *c, int dst_reg, int64_t imm);  /* fwd (big frames); extern: also used by riscv32.c prologue */
static void riscv64_emit_mov_reg(ZeroCCCompiler *c, int dst, int src);           /* fwd */
static void riscv64_emit_leave(ZeroCCCompiler *c);                               /* fwd */

static void riscv64_emit_prologue(ZeroCCCompiler *c, size_t stack_size, uint32_t callee_save_mask) {
    if (c->lowering.is_frameless) return;
    (void)callee_save_mask;

    if (c->lowering.frame_align > 16) {
        /* C11 _Alignas(N) realign mode: save the ra/s0 pair (16 bytes),
         * point s0 at the pre-realign base, realign SP to N, then allocate
         * the N-rounded frame below SP'. `andi`'s 12-bit immediate caps N
         * at 4096 (checked in ast_lowering.c). Slots are [SP' - k] via
         * frame_delta = frame; the epilogue is the plain FP-mode leave
         * (sp restored from the s0 value). */
        int f3 = (c->isa == ISA_RISCV32) ? 2 : 3;  /* sw (32-bit) vs sd (64-bit) */
        size_t frame = (size_t)c->lowering.frame_delta;
        riscv_emit_u32(c, rv64_I(RV64_SP, RV64_SP, 0, -16, 0x13));         /* addi sp, sp, -16 */
        riscv_emit_u32(c, rv64_S(RV64_SP, RV64_RA, f3, 8, 0x23));          /* sd/w ra, 8(sp) */
        riscv_emit_u32(c, rv64_S(RV64_SP, RV64_S0, f3, 0, 0x23));          /* sd/w s0, 0(sp) */
        riscv_emit_u32(c, rv64_I(RV64_S0, RV64_SP, 0, 0, 0x13));           /* mv s0, sp */
        int mask = (~((int32_t)c->lowering.frame_align - 1)) & 0xFFF;      /* ~(N-1) */
        if (mask != 0) {
            riscv_emit_u32(c, rv64_I(RV64_SP, RV64_SP, 7, mask, 0x13));   /* andi sp, sp, mask */
        } else {
            /* N == 4096: ~(N-1) needs bits above the 12-bit andi immediate
             * (andi sp,sp,0 would zero SP). Use a register mask instead. */
            riscv64_emit_mov_imm(c, RV64_T0, (int64_t)~((int64_t)c->lowering.frame_align - 1)); /* li t0, ~(N-1) */
            riscv_emit_u32(c, rv64_R(RV64_SP, RV64_SP, RV64_T0, 7, 0));      /* and sp, sp, t0 (f7=0) */
        }
        if (frame <= 2047) {
            riscv_emit_u32(c, rv64_I(RV64_SP, RV64_SP, 0, (int16_t)(-(int32_t)frame), 0x13));  /* addi sp, sp, -frame */
        } else {
            riscv64_emit_mov_imm(c, RV64_T0, (int64_t)frame);              /* li t0, frame */
            riscv_emit_u32(c, rv64_R(RV64_SP, RV64_SP, RV64_T0, 0, 0x20));  /* sub sp, sp, t0 (f7=0x20) */
        }
        return;
    }

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
        riscv_emit_u32(c, rv64_R(RV64_SP, RV64_SP, RV64_T0, 0, 0x20));  /* sub sp, sp, t0 (f7=0x20) */
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
    if (c->lowering.omit_frame_pointer && c->lowering.frame_align <= 16) {
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

/* RISC-V 32-bit conv table + backend instance moved to riscv32.c (Sprint 114). */

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
            /* RV64: use ADDIW (opcode 0x1B) after LUI so the sum is re-sign-
             * extended from bit 31. Plain ADDI is wrong for hi == 0x80000
             * (imm 0x7FFFF800..0x7FFFFFFF): LUI yields 0xFFFFFFFF80000000 and
             * the positive remainder never carries back, leaving garbage in
             * bits 32-63 (e.g. INT_MAX -> 0xFFFFFFFF7FFFFFFF; Sprint 164).
             * RV32 has no ADDIW and needs none (XLEN == 32). */
            uint32_t opc = (hi && c->isa == ISA_RISCV64) ? 0x1B : 0x13;
            riscv_emit_u32(c, rv64_I(RV_REG(dst_reg), hi ? RV_REG(dst_reg) : 0, 0, (int16_t)lo64, opc));
        }
    }
}

void riscv64_emit_mov_imm(ZeroCCCompiler *c, int dst_reg, int64_t imm) {
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
        int scratch = (dst_reg != RV64_T5) ? RV64_T5 : ((dst_reg != RV64_T0) ? RV64_T0 : RV64_T1);
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
    if (width >= 8 || width == 0) {
        /* Not a narrow extension (width 0 = "no mark" per the Instr.width
         * encoding; 8+ = zero-extend flag): at most a move is needed
         * (they previously fell through to the 32-bit path and truncated
         * the value). */
        if (dst != src) riscv64_emit_mov_reg(c, dst, src);
        return;
    }
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
        int scratch = (base_reg != RV64_T5 && dst_reg != RV64_T5) ? RV64_T5 :
                      ((base_reg != RV64_T0 && dst_reg != RV64_T0) ? RV64_T0 : RV64_T1);
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
        int scratch = (base_reg != RV64_T5 && src_reg != RV64_T5) ? RV64_T5 :
                      ((base_reg != RV64_T0 && src_reg != RV64_T0) ? RV64_T0 : RV64_T1);
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
     * For external/prototype functions: emit R_RISCV_CALL_PLT relocation for linker.
     * NESTED functions are emitted AFTER their enclosing function (deferred),
     * so their text_offset is still 0 here - early-patching would bake in a
     * jump to text offset 0 (the first function) and infinite-recurse into
     * the enclosing scope. Route them through the relocation path: the .o
     * carries R_RISCV_JAL against the mangled symbol, which the linker
     * resolves once all text offsets are final. */
    ZeroCCFunction *fn = func_name ? zero_cc_find_function_by_symbol(c, func_name) : NULL;
    size_t pos = me_get_offset(c);

    if (fn && !fn->is_prototype && !fn->is_nested) {
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
        } else if (func_name) {
            zero_cc_reloc_overflow_note(c);
        }
    }
}

static void riscv_emit_stack_guard_init(ZeroCCCompiler *c, int32_t slot_off) {
    /* auipc t5, 0; addi t5, t5, 0 */
    uint32_t auipc = (uint32_t)(((uint32_t)RV64_T5 << 7) | 0x17u);
    uint32_t addi  = (uint32_t)(((uint32_t)RV64_T5 << 15) | ((uint32_t)RV64_T5 << 7) | 0x13u);
    riscv_emit_u32(c, auipc);
    riscv_emit_u32(c, addi);
    if (c->reloc_count < ZERO_CC_MAX_RELOCS) {
        ZeroCCReloc *r = &c->relocs[c->reloc_count++];
        r->text_patch_offset = me_get_offset(c) - 4;
        snprintf(r->target_symbol, sizeof(r->target_symbol), "__stack_chk_guard");
        r->is_call = false;
    }
    int f3 = (c->isa == ISA_RISCV32) ? 2 : 3;
    riscv_emit_u32(c, rv64_I(RV_REG(RV64_T5), RV_REG(RV64_T5), f3, 0, 0x03)); /* ld/lw t5, 0(t5) */
    int base_reg = zero_cc_frame_base_reg(c);
    int32_t disp = zero_cc_frame_disp(c, -slot_off);
    riscv64_emit_store(c, RV64_T5, base_reg, disp);
    riscv_emit_u32(c, 0x00000F13); /* li/mv t5, zero */
}

static void riscv_emit_stack_guard_check(ZeroCCCompiler *c, int32_t slot_off) {
    int base_reg = zero_cc_frame_base_reg(c);
    int32_t disp = zero_cc_frame_disp(c, -slot_off);
    riscv64_emit_load(c, RV64_T5, base_reg, disp);
    /* auipc t6, 0; addi t6, t6, 0 */
    uint32_t auipc = (uint32_t)(((uint32_t)RV64_T6 << 7) | 0x17u);
    uint32_t addi  = (uint32_t)(((uint32_t)RV64_T6 << 15) | ((uint32_t)RV64_T6 << 7) | 0x13u);
    riscv_emit_u32(c, auipc);
    riscv_emit_u32(c, addi);
    if (c->reloc_count < ZERO_CC_MAX_RELOCS) {
        ZeroCCReloc *r = &c->relocs[c->reloc_count++];
        r->text_patch_offset = me_get_offset(c) - 4;
        snprintf(r->target_symbol, sizeof(r->target_symbol), "__stack_chk_guard");
        r->is_call = false;
    }
    int f3 = (c->isa == ISA_RISCV32) ? 2 : 3;
    riscv_emit_u32(c, rv64_I(RV_REG(RV64_T6), RV_REG(RV64_T6), f3, 0, 0x03)); /* ld/lw t6, 0(t6) */
    riscv_emit_u32(c, 0x01ff0463); /* beq t5, t6, +8 */
    riscv64_emit_call(c, "__stack_chk_fail");
}

static void riscv64_emit_ret(ZeroCCCompiler *c) {
    /* jalr zero, 0(ra) */
    riscv_emit_u32(c, 0x00008067);
}

/* OS syscall (Linux RISC-V ABI): number in a7, args 1-6 in a0-a5, result
 * in a0 (= VREG_0, no fix-up needed). ecall clobbers the caller-saved set
 * (t0-t6, a0-a7), so every allocatable clobbered register that may hold a
 * live value - t2 (x7), a1-a7 (x11-x17), t3-t4/t6 (x28,x29,x31); t0/t1
 * are scratch, t5 (x30) is the address scratch (Sprint 109), a0 is the
 * result - is saved before and restored after, the
 * same caller-saved-fix idiom the OP_CALL lowering uses for calls. */
static void riscv_emit_syscall(ZeroCCCompiler *c, int arg_count, const int *arg_temps) {
    static const int linux_syscall_regs[7] = {17, 10, 11, 12, 13, 14, 15}; /* a7, a0-a5 */
    static const int clobbered_live_regs[] = {7, 11, 12, 13, 14, 15, 16, 17, 28, 29, 31};
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

/* OS clone (SYS_clone = 220). Arguments: flags, child_sp, ptid, tls, ctid */
static void riscv_emit_clone(ZeroCCCompiler *c, int arg_count, const int *arg_temps) {
    static const int clone_regs[5] = {10, 11, 12, 13, 14}; /* a0-a4 */
    int32_t spill_base_offset = c->regalloc.spill_base_offset;
    int n = (arg_count < 5) ? arg_count : 5;
    int src_regs[8] = {0};
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
        if (src_reg != clone_regs[a] && BE(c)->emit_push_reg) {
            BE(c)->emit_push_reg(c, src_reg);
        }
    }
    for (int a = n - 1; a >= 0; a--) {
        if (src_regs[a] == clone_regs[a]) continue;
        if (BE(c)->emit_pop_reg) BE(c)->emit_pop_reg(c, clone_regs[a]);
    }
    /* li a7, 220 */
    riscv_emit_u32(c, 0x0dc00893);
    /* ecall */
    riscv_emit_u32(c, 0x00000073);
    /* bnez a0, +36 (jump past the 8 instructions below) */
    riscv_emit_u32(c, 0x02051263);
    /* Child path: */
    /* mv s0, zero */
    riscv_emit_u32(c, 0x00000413);
    if (BE(c)->isa == ISA_RISCV32) {
        /* lw a1, 0(sp) */
        riscv_emit_u32(c, 0x00012583);
        /* lw a0, 4(sp) */
        riscv_emit_u32(c, 0x00412503);
    } else {
        /* ld a1, 0(sp) */
        riscv_emit_u32(c, 0x00013583);
        /* ld a0, 8(sp) */
        riscv_emit_u32(c, 0x00813503);
    }
    /* addi sp, sp, 16 */
    riscv_emit_u32(c, 0x01010113);
    /* jalr a1 */
    riscv_emit_u32(c, 0x000580e7);
    /* li a7, 93 (SYS_exit) */
    riscv_emit_u32(c, 0x05d00893);
    /* ecall */
    riscv_emit_u32(c, 0x00000073);
    /* ebreak */
    riscv_emit_u32(c, 0x00100073);
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

/* jmp [reg] - indirect jump through register */
static void riscv64_emit_jmp_reg(ZeroCCCompiler *c, int reg) {
    /* jalr zero, 0(reg) */
    riscv_emit_u32(c, (uint32_t)(((uint32_t)(reg & 0x1F) << 15) | 0x67u));
}

/* auipc + addi pair - load label address PC-relative */
static size_t riscv64_emit_lea_label(ZeroCCCompiler *c, int dst_reg) {
    size_t patch_pos = me_get_offset(c);
    int r = dst_reg & 0x1F;
    uint32_t auipc = (uint32_t)(((uint32_t)r << 7) | 0x17u);
    uint32_t addi  = (uint32_t)(((uint32_t)r << 15) | ((uint32_t)r << 7) | 0x13u);
    riscv_emit_u32(c, auipc);
    riscv_emit_u32(c, addi);
    return patch_pos;
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
        int scratch = (base_reg != RV64_T5) ? RV64_T5 : ((base_reg != RV64_T0) ? RV64_T0 : RV64_T1);
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
        int scratch = (base_reg != RV64_T5) ? RV64_T5 : ((base_reg != RV64_T0) ? RV64_T0 : RV64_T1);
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
        int scratch = (base != RV64_T5) ? RV64_T5 : ((base != RV64_T0) ? RV64_T0 : RV64_T1);
        riscv64_emit_mov_imm(c, scratch, offset);
        riscv_emit_u32(c, rv64_R(scratch, RV_REG(base), scratch, 0, 0));
        riscv_emit_u32(c, rv64_I(RV_REG(dst), scratch, 2, 0, 0x07));
    }
    /* fcvt.d.s fd, fd - widen single to double (double FP model; a raw flw
     * leaves the float bit pattern in the low half of the 64-bit register,
     * i.e. a denormalized double - isa_qemu 07_float). */
    riscv_emit_u32(c, rv64_FP(RV_REG(dst), RV_REG(dst), 0, 0x21, 0));
}

/* Sprint 115m: raw 4-byte ABI-float access - the register's low 32 bits ARE
 * the float pattern; no fcvt through the double-width scalar model. flw
 * zero-extends into the 64-bit F register (FLEN=64 on RV64D and RV32D). */

/* flw without the trailing fcvt.d.s widen. */
static void riscv64_emit_vloadss_raw(ZeroCCCompiler *c, int dst, int base, int32_t offset) {
    if (offset >= -2048 && offset <= 2047) {
        riscv_emit_u32(c, rv64_I(RV_REG(dst), RV_REG(base), 2, (int16_t)offset, 0x07));
    } else {
        int scratch = (base != RV64_T5) ? RV64_T5 : ((base != RV64_T0) ? RV64_T0 : RV64_T1);
        riscv64_emit_mov_imm(c, scratch, offset);
        riscv_emit_u32(c, rv64_R(scratch, RV_REG(base), scratch, 0, 0));
        riscv_emit_u32(c, rv64_I(RV_REG(dst), scratch, 2, 0, 0x07));
    }
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
        int scratch_gpr = (base != RV64_T5) ? RV64_T5 : ((base != RV64_T0) ? RV64_T0 : RV64_T1);
        riscv64_emit_mov_imm(c, scratch_gpr, offset);
        riscv_emit_u32(c, rv64_R(scratch_gpr, RV_REG(base), scratch_gpr, 0, 0));
        riscv_emit_u32(c, rv64_S(scratch_gpr, scratch, 2, 0, 0x27));
    }
}

/* Sprint 115m: fsw without the leading fcvt.s.d narrow - stores the
 * register's low 32 bits (the ABI float pattern). */
static void riscv64_emit_vstoress_raw(ZeroCCCompiler *c, int src, int base, int32_t offset) {
    if (offset >= -2048 && offset <= 2047) {
        riscv_emit_u32(c, rv64_S(RV_REG(base), RV_REG(src), 2, (int16_t)offset, 0x27));
    } else {
        int scratch_gpr = (base != RV64_T5) ? RV64_T5 : ((base != RV64_T0) ? RV64_T0 : RV64_T1);
        riscv64_emit_mov_imm(c, scratch_gpr, offset);
        riscv_emit_u32(c, rv64_R(scratch_gpr, RV_REG(base), scratch_gpr, 0, 0));
        riscv_emit_u32(c, rv64_S(scratch_gpr, RV_REG(src), 2, 0, 0x27));
    }
}

static void riscv64_emit_load_fp_rodata(ZeroCCCompiler *c, int dst_reg, size_t rodata_offset) {
    /* auipc t5, 0; fld dst, 0(t5) with a string relocation on the fld
     * (the ELF writer attaches R_RISCV_PCREL_HI20 to the auipc).
     * Note: dst_reg is an FP register; auipc requires an integer register
     * (t5 = reserved address scratch; t0/t1 may hold live reloads). */
    riscv_emit_u32(c, rv64_AUIPC(RV64_T5, 0));
    size_t patch_pos = me_get_offset(c);
    riscv_emit_u32(c, rv64_I(RV_REG(dst_reg), RV64_T5, 3, 0, 0x07));
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

static void riscv64_emit_fp_cvttsd2ss(ZeroCCCompiler *c, int dst, int src) {
    /* fcvt.s.d fd, fs (rm=7) - model double -> ABI float (in-register
     * narrow; Sprint 115m float return epilogue). */
    riscv_emit_u32(c, rv64_FP(RV_REG(dst), RV_REG(src), 1, 0x20, 7));
}



static void riscv64_emit_fp_cvtsd2ss(ZeroCCCompiler *c, int dst, int src) {
    /* fcvt.s.d fd, fs (rs2=1, rm=7) - narrow double to single (GNU vector
     * per-lane). Note the encoding is NOT the mirror of fcvt.d.s: the
     * assembler encodes fcvt.s.d as f7=0x20, rs2=1, f3=7. */
    riscv_emit_u32(c, rv64_FP(RV_REG(dst), RV_REG(src), 1, 0x20, 7));
}

static void riscv64_emit_fp_cvtsi2ss(ZeroCCCompiler *c, int fp_dst, int int_src) {
    /* fcvt.s.w fd, rs (rm=7) */
    riscv_emit_u32(c, rv64_FP(RV_REG(fp_dst), RV_REG(int_src), 0, 0x68, 7));
}

static void riscv64_emit_vload(ZeroCCCompiler *c, int dst, int base, int32_t offset) {
    riscv64_emit_load_fp(c, dst, base, offset);
}

static void riscv64_emit_vstore(ZeroCCCompiler *c, int src, int base, int32_t offset) {
    riscv64_emit_store_fp(c, src, base, offset);
}

static void riscv64_emit_vaddps(ZeroCCCompiler *c, int dst, int src1, int src2) {
    riscv_emit_u32(c, rv64_FP(RV_REG(dst), RV_REG(src1), RV_REG(src2), 0x00, 7));
}

static void riscv64_emit_vsubps(ZeroCCCompiler *c, int dst, int src1, int src2) {
    riscv_emit_u32(c, rv64_FP(RV_REG(dst), RV_REG(src1), RV_REG(src2), 0x04, 7));
}

static void riscv64_emit_vmulps(ZeroCCCompiler *c, int dst, int src1, int src2) {
    riscv_emit_u32(c, rv64_FP(RV_REG(dst), RV_REG(src1), RV_REG(src2), 0x08, 7));
}

static void riscv64_emit_vdivps(ZeroCCCompiler *c, int dst, int src1, int src2) {
    riscv_emit_u32(c, rv64_FP(RV_REG(dst), RV_REG(src1), RV_REG(src2), 0x0C, 7));
}

static void riscv64_emit_vmovaps(ZeroCCCompiler *c, int dst, int src) {
    if (dst == src) return;
    riscv_emit_u32(c, rv64_FP(RV_REG(dst), RV_REG(src), RV_REG(src), 0x10, 0));
}

static void riscv_resolve_labels(ZeroCCCompiler *c) {
    (void)c;
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
    } else if (op == 0x17u) {
        /* AUIPC + ADDI pair: 32-bit PC-relative load */
        int32_t hi20 = (rel + 0x800) >> 12;
        int32_t lo12 = rel - (hi20 << 12);
        *insn = (cur & 0xFFFu) | ((uint32_t)hi20 << 12);
        if (patch_offset + 8 <= c->text_section.length) {
            uint32_t *addi_insn = insn + 1;
            *addi_insn = (*addi_insn & 0xFFFFF) | ((uint32_t)(lo12 & 0xFFF) << 20);
        }
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
        int scratch = (base != RV64_T5 && dst != RV64_T5) ? RV64_T5 :
                      ((base != RV64_T0 && dst != RV64_T0) ? RV64_T0 : RV64_T1);
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

/* store_64 src, [base] - 64-bit store on RV64, 32-bit word store on RV32 (offset 0). */
static void riscv64_emit_store_64(ZeroCCCompiler *c, int src, int base) {
    int f3 = (c->isa == ISA_RISCV32) ? 2 : 3;
    riscv_emit_u32(c, rv64_S(RV_REG(base), RV_REG(src), f3, 0, 0x23));
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
        int scratch = (base != RV64_T5 && reg != RV64_T5) ? RV64_T5 :
                      ((base != RV64_T0 && reg != RV64_T0) ? RV64_T0 : RV64_T1);
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
        /* Sprint 115m: raw low-32 store - an external caller's float param
         * arrives as a float pattern in the low 32; the old fcvt-narrowing
         * store destroyed it. */
        riscv64_emit_vstoress_raw(c, src_reg, base, disp);
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
    /* sub sp, sp, size (sub x2, x2, size_reg): opcode 0x33, funct3 0, funct7 0x20 */
    riscv_emit_u32(c, 0x40000033 | (2 << 7) | (2 << 15) | ((RV_REG(size_reg) & 0x1f) << 20));
    if (dst_reg != 2) {
        riscv64_emit_mov_reg(c, dst_reg, 2 /* sp */);
    }
}

/* atomic_cmpxchg dst, addr, expected, desired, width - atomic compare and swap */
static void riscv64_emit_atomic_cmpxchg(ZeroCCCompiler *c, int dst, int addr, int expected, int desired, int width) {
    uint32_t f3 = (width == 8) ? 3 : 2;
    /* 1: lr.w.aq / lr.d.aq dst, (addr) */
    riscv_emit_u32(c, 0x1400002F | (f3 << 12) | ((RV_REG(addr) & 0x1F) << 15) | ((RV_REG(dst) & 0x1F) << 7));
    /* bne dst, expected, 2f (+12 bytes) */
    riscv_emit_u32(c, rv64_B(RV_REG(dst), RV_REG(expected), 1, 12));
    /* sc.w.rl / sc.d.rl t5, desired, (addr) */
    riscv_emit_u32(c, 0x1A00002F | (f3 << 12) | ((RV_REG(desired) & 0x1F) << 20) | ((RV_REG(addr) & 0x1F) << 15) | (RV64_T5 << 7));
    /* bne t5, zero, 1b (-12 bytes) */
    riscv_emit_u32(c, rv64_B(RV64_T5, 0, 1, -12));
    /* 2: */
}

/* atomic_xadd dst, addr, val, width - atomic add and swap */
static void riscv64_emit_atomic_xadd(ZeroCCCompiler *c, int dst, int addr, int val, int width) {
    uint32_t f3 = (width == 8) ? 3 : 2;
    riscv_emit_u32(c, 0x0600002F | (f3 << 12) | ((RV_REG(val) & 0x1F) << 20) | ((RV_REG(addr) & 0x1F) << 15) | ((RV_REG(dst) & 0x1F) << 7));
}

/* atomic_xchg dst, addr, val, width - atomic exchange */
static void riscv64_emit_atomic_xchg(ZeroCCCompiler *c, int dst, int addr, int val, int width) {
    uint32_t f3 = (width == 8) ? 3 : 2;
    riscv_emit_u32(c, 0x0E00002F | (f3 << 12) | ((RV_REG(val) & 0x1F) << 20) | ((RV_REG(addr) & 0x1F) << 15) | ((RV_REG(dst) & 0x1F) << 7));
}

static void riscv64_emit_atomic_or(ZeroCCCompiler *c, int dst, int addr, int val, int width) {
    uint32_t f3 = (width == 8) ? 3 : 2;
    riscv_emit_u32(c, 0x4600002F | (f3 << 12) | ((RV_REG(val) & 0x1F) << 20) | ((RV_REG(addr) & 0x1F) << 15) | ((RV_REG(dst) & 0x1F) << 7));
}

static void riscv64_emit_atomic_and(ZeroCCCompiler *c, int dst, int addr, int val, int width) {
    uint32_t f3 = (width == 8) ? 3 : 2;
    riscv_emit_u32(c, 0x6600002F | (f3 << 12) | ((RV_REG(val) & 0x1F) << 20) | ((RV_REG(addr) & 0x1F) << 15) | ((RV_REG(dst) & 0x1F) << 7));
}

static void riscv64_emit_atomic_xor(ZeroCCCompiler *c, int dst, int addr, int val, int width) {
    uint32_t f3 = (width == 8) ? 3 : 2;
    riscv_emit_u32(c, 0x2600002F | (f3 << 12) | ((RV_REG(val) & 0x1F) << 20) | ((RV_REG(addr) & 0x1F) << 15) | ((RV_REG(dst) & 0x1F) << 7));
}

static void riscv64_emit_atomic_cmpxchg_bool(ZeroCCCompiler *c, int dst, int addr, int expected, int desired, int width) {
    riscv64_emit_atomic_cmpxchg(c, dst, addr, expected, desired, width);
    /* xor dst, dst, expected */
    riscv_emit_u32(c, rv64_R(RV_REG(dst), RV_REG(dst), RV_REG(expected), 4, 0));
    /* sltiu dst, dst, 1 */
    riscv_emit_u32(c, rv64_I(RV_REG(dst), RV_REG(dst), 3, 1, 0x13));
}

/* mfence - memory fence */
static void riscv64_emit_mfence(ZeroCCCompiler *c) {
    /* fence iorw, iorw */
    riscv_emit_u32(c, 0x0000000F);
}

/* __builtin_trap / __builtin_unreachable */
static void riscv64_emit_trap(ZeroCCCompiler *c) {
    /* unimp (illegal instruction; verified against riscv64-linux-gnu-as =
     * 0xC0001073 -> SIGILL). The old 0x00002007 was a valid float-load
     * (flw x0,0(x0)) that SIGSEGV'd rather than trapping. */
    riscv_emit_u32(c, 0xC0001073);
}

static void riscv64_emit_trap_asm(ZeroCCCompiler *c, FILE *out) {
    (void)c;
    fprintf(out, "\tunimp\n");
}

/* zero_san (Sprint 163): compare-based signed-overflow check + unimp.
 * `res` = raw result of the preceding add/sub/mul; `a`, `b` = the two
 * operands. Encodings verified against riscv64-linux-gnu-as (Sprint 163):
 *   slli=rv64_I(rd,rs,1,sh,0x13)  srai=rv64_I(rd,rs,5,0x400|sh,0x13)
 *   mulh=rv64_R(rd,rs1,rs2,1,1)  xor/and=rv64_R(rd,rs1,rs2,4/7,0)
 *   beq/bge=rv64_B(rs1,rs2,0/5,off)  unimp=0xC0001073
 * RV64 width 4 uses the sign-extension round-trip (result fits in 32 bits);
 * RV32 width 4 (int is the natural word) uses the operand overflow formula
 * (the round-trip is a no-op there). Width 8 (RV64) uses the operand formula
 * too. The branch skips the unimp (offset +8) or, with -fsanitize-trap
 * (Sprint 167), the 4-instruction report block + unimp (offset +24).
 * Follow-on (Sprint 166): div-by-zero / shift-count checks. They test `res`
 * (temp_dest = divisor/count; src1/src2 are TEMP_NONE) directly and must
 * run before the width<8 RV64 round-trip early-return (which is
 * ADD/SUB/MUL-only).
 * Scratch discipline (Sprint 168 - f64loop fix): the check temporaries live
 * in t3/t4 (28/29 - never allocated to temps by LRA; reserved for 128-bit
 * spill scratch in the regalloc pool), NEVER in `a`/`b`. The original
 * "dead, clobbered in place" design miscompiled whenever the optimizer kept
 * an operand live across the check: -O2 LICM hoists a loop-invariant addend
 * out of `s = s + C`, and the check's `xor b, b, res` then destroyed the
 * hoisted constant every iteration (f64loop returned 3440441 instead of
 * 123450 at -O2/-O3 on aarch64/riscv64, 61725 on riscv32). t3/t4 are also
 * guaranteed distinct from {res,a,b}: those can only be allocated regs or
 * scratch_reg1/2 (t0/t1) when a source/dest was spill-reloaded - which is
 * also why the shift-count bound is materialized in t4, not t0 (a spilled
 * count sits in t0). */
static void riscv64_emit_san_report_block(ZeroCCCompiler *c, int res,
                                          int width, int op) {
    /* The value move comes first: `res` may be a0/a1, which the li's write.
     * mv a2, res = addi a2, res, 0; li a0, op = addi a0, zero, op. */
    riscv_emit_u32(c, rv64_I(RV_REG(12), RV_REG(res), 0, 0, 0x13));   /* mv a2, res */
    riscv_emit_u32(c, rv64_I(RV_REG(10), 0, 0, op, 0x13));            /* li a0, op */
    riscv_emit_u32(c, rv64_I(RV_REG(11), 0, 0, width, 0x13));         /* li a1, width */
    riscv64_emit_call(c, "__zerocsan_report");
}

static void riscv64_emit_ovf_trap(ZeroCCCompiler *c, int res, int a, int b,
                                  int width, int op) {
    bool is64 = (c->isa == ISA_RISCV64);
    int skip = c->sanitize_report ? 24 : 8;  /* -fsanitize-trap block + unimp */
    /* Follow-on (Sprint 166): div-by-zero / shift-count checks.
     * The divisor/count temp is carried in temp_dest (`res`; src1 is
     * TEMP_NONE for these ops); the check precedes the div/shift, so `res`
     * still holds the value. Must run before the width<8 result
     * round-trip below. */
    if (op == ZC_OVF_OP_DIV_ZERO || op == ZC_OVF_OP_REM_ZERO) {
        /* bne res, zero, skip; unimp  (trap if divisor == 0) */
        riscv_emit_u32(c, rv64_B(RV_REG(res), 0, 1, skip));
        if (c->sanitize_report) riscv64_emit_san_report_block(c, res, width, op);
        riscv_emit_u32(c, 0xC0001073u);
        return;
    }
    if (op == ZC_OVF_OP_SHIFT_COUNT) {
        /* li t4, W; bge res, zero, skip; unimp; blt res, t4, skip; unimp
         * (W = width*8 bits; t4 is the check scratch - never t0, which may
         * hold the spill-reloaded count).
         * Branch funct3: BEQ=0 BNE=1 BLT=4 BGE=5. */
        riscv_emit_u32(c, rv64_I(RV_REG(29), 0, 0, width * 8, 0x13));     /* li t4, W */
        riscv_emit_u32(c, rv64_B(RV_REG(res), 0, 5, skip));               /* bge res, zero: res >= 0 */
        if (c->sanitize_report) riscv64_emit_san_report_block(c, res, width, op);
        riscv_emit_u32(c, 0xC0001073u);                                    /* unimp: res < 0 */
        riscv_emit_u32(c, rv64_B(RV_REG(res), RV_REG(29), 4, skip));       /* blt res, t4: res < W */
        if (c->sanitize_report) riscv64_emit_san_report_block(c, res, width, op);
        riscv_emit_u32(c, 0xC0001073u);                                    /* unimp: res >= W */
        return;
    }
    if (width < 8 && is64) {
        /* RV64: sign-extension round-trip (result-only); t3 is the scratch. */
        riscv_emit_u32(c, rv64_I(RV_REG(28), RV_REG(res), 1, 32, 0x13));     /* slli t3, res, 32 */
        riscv_emit_u32(c, rv64_I(RV_REG(28), RV_REG(28), 5, 0x400 | 32, 0x13)); /* srai t3, t3, 32 */
        riscv_emit_u32(c, rv64_B(RV_REG(res), RV_REG(28), 0, skip));        /* beq res, t3: fits */
        if (c->sanitize_report) riscv64_emit_san_report_block(c, res, width, op);
        riscv_emit_u32(c, 0xC0001073u);                                     /* unimp */
        return;
    }
    /* width 8 (RV64) or width 4 (RV32): operand-based overflow formula,
     * computed in t3/t4 so live operands are left intact. */
    int asr_sh = is64 ? 63 : 31;
    if (op == ZC_OVF_OP_MUL) {
        /* ovf = (mulh(a,b) != asr(res,sh)): t3 = high(a*b); t4 = signext(res). */
        riscv_emit_u32(c, rv64_R(RV_REG(28), RV_REG(a), RV_REG(b), 1, 1));            /* mulh t3, a, b */
        riscv_emit_u32(c, rv64_I(RV_REG(29), RV_REG(res), 5, 0x400 | asr_sh, 0x13));  /* srai t4, res, sh */
        riscv_emit_u32(c, rv64_B(RV_REG(28), RV_REG(29), 0, skip));                   /* beq t3, t4: equal */
        if (c->sanitize_report) riscv64_emit_san_report_block(c, res, width, op);
        riscv_emit_u32(c, 0xC0001073u);                                              /* unimp */
        return;
    }
    if (op == ZC_OVF_OP_SUB) {
        /* ovf = signbit((a^res) & (a^b)); t3/t4 are the temporaries. */
        riscv_emit_u32(c, rv64_R(RV_REG(28), RV_REG(res), RV_REG(a), 4, 0));          /* xor t3, res, a */
        riscv_emit_u32(c, rv64_R(RV_REG(29), RV_REG(a), RV_REG(b), 4, 0));            /* xor t4, a, b */
    } else { /* ADD: ovf = signbit((a^res) & (b^res)) */
        riscv_emit_u32(c, rv64_R(RV_REG(28), RV_REG(a), RV_REG(res), 4, 0));          /* xor t3, a, res */
        riscv_emit_u32(c, rv64_R(RV_REG(29), RV_REG(b), RV_REG(res), 4, 0));          /* xor t4, b, res */
    }
    riscv_emit_u32(c, rv64_R(RV_REG(28), RV_REG(28), RV_REG(29), 7, 0));              /* and t3, t3, t4 */
    riscv_emit_u32(c, rv64_B(RV_REG(28), 0, 5, skip));                                /* bge t3, zero: no overflow */
    if (c->sanitize_report) riscv64_emit_san_report_block(c, res, width, op);
    riscv_emit_u32(c, 0xC0001073u);                                                  /* unimp */
}

/* =====================================================================
 * Phase 3b (cross-ISA): trampoline stub for a capturing nested function.
 *
 * Mirrors zero_cc_aarch64_emit_tramp_stub for the RISC-V 64-bit
 * calling convention: GPR units take a0..a7, FP units fa0..fa7 (complex
 * = 2), >16B struct/union = one GPR unit (zerocc's hidden-pointer
 * convention - the >16B by-value struct capture is copied from ctx into
 * the stub frame and its address is passed). A >16B struct RETURN needs
 * nothing: the sret pointer takes the FIRST integer slot (a0) at the
 * USER call, and the stub's stream starts at a0 as well, so it passes
 * straight through untouched.
 *
 * Frame layout (s0 = entry_sp - 16, after `addi sp,-16; sd ra,8(sp);
 * sd s0,0(sp); mv s0,sp`):
 *   [s0+8]              stub's own ra (the caller's jal linked into RA,
 *                        the stack was untouched - like x30 on AArch64)
 *   [s0+16+8m]          entry stack unit m (stack arg 0 sits at [entry_sp])
 *   [s0-8-8g]           entry GPR arg saves (a0..a7)
 *   [s0-80-16f]         entry FP arg saves (fa0..fa7)
 *   [s0-208-8s]         entry stack unit saves
 *   [s0-208-8m-32ci-8w] >16B by-value capture copies
 * Scratch: t0 = ctx base, t1/t2 = GPR temps, ft8 = FP temp.
 * ===================================================================== */

void zero_cc_riscv_emit_tramp_stub(ZeroCCCompiler *c, const ZeroCCFunction *nf) {
    /* RV64 only - the RV32 FP/arg model predates this work. */
    if (c->isa != ISA_RISCV64) return;
    size_t C = nf->nested_captured_count;
    size_t U = nf->param_count - C;

    /* ---- pass 1: entry geometry of the USER call's units (same shared
     * stack-stream model as the AArch64 stub; register slot and shared
     * stack index are SEPARATE namespaces - see the AArch64 notes) ---- */
    bool ue_isfp[ZERO_CC_TRAMP_MAX_UNITS];
    int  ue_reg[ZERO_CC_TRAMP_MAX_UNITS];
    int  ue_stk[ZERO_CC_TRAMP_MAX_UNITS];
    int ue_n = 0, e_gpr = 0, e_fp = 0, e_shared = 0;
    for (size_t ui = 0; ui < U && ue_n < ZERO_CC_TRAMP_MAX_UNITS; ui++) {
        const ZeroCCType *t = &nf->param_types[C + ui];
        int slots = zero_cc_tramp_unit_slots(t);
        bool isfp = zero_cc_tramp_unit_is_fp(t);
        for (int w = 0; w < slots; w++) {
            int sidx = isfp ? e_fp : e_gpr;
            ue_isfp[ue_n] = isfp;
            if (sidx < 8) { ue_reg[ue_n] = sidx; ue_stk[ue_n] = -1; }
            else          { ue_reg[ue_n] = -1;  ue_stk[ue_n] = e_shared++; }
            ue_n++;
        }
        if (isfp) e_fp += slots; else e_gpr += slots;
    }
    int entry_stack_n = e_shared;

    /* ---- pass 2: final unit list (captures first, then user params) ---- */
    ZeroCCTrampUnit u[ZERO_CC_TRAMP_MAX_UNITS];
    int n = 0, gpr_run = 0, fp_run = 0, decl_stack_run = 0;
    int copy_words = 0;

    for (size_t ci = 0; ci < C; ci++) {
        if (nf->nested_capture_byref[ci]) {
            if (n >= ZERO_CC_TRAMP_MAX_UNITS) return;
            u[n].is_fp = false;
            u[n].fin_slot = gpr_run < 8 ? gpr_run++ : -1;
            u[n].fin_stack = u[n].fin_slot < 0 ? decl_stack_run++ : -1;
            u[n].src = 0;
            u[n].src_off = (int)(32 * ci);
            n++;
            continue;
        }
        const ZeroCCType *t = &nf->nested_capture_types[ci];
        int slots = zero_cc_tramp_unit_slots(t);
        bool isfp = zero_cc_tramp_unit_is_fp(t);
        bool big_struct = (t->tag == TYPE_STRUCT || t->tag == TYPE_UNION) &&
                          !zero_cc_type_is_vector(t) && t->size > 16;
        int words = (int)((t->size + 7) / 8);
        for (int w = 0; w < slots; w++) {
            if (n >= ZERO_CC_TRAMP_MAX_UNITS) return;
            u[n].is_fp = isfp;
            u[n].fin_slot = isfp ? (fp_run < 8 ? fp_run++ : -1)
                                 : (gpr_run < 8 ? gpr_run++ : -1);
            u[n].fin_stack = u[n].fin_slot < 0 ? decl_stack_run++ : -1;
            if (big_struct) {
                if (w == 0) {
                    u[n].src = 5;
                    u[n].src_off = 208 + 8 * entry_stack_n + 32 * (int)ci;
                    copy_words += words;
                }
            } else {
                u[n].src = 0;
                u[n].src_off = (int)(32 * ci) + 8 * w;
            }
            n++;
        }
    }
    /* Sprint 99: nonlocal goto pairs, between captures and user params
     * (ctx words at 32*C + 16*i (rbp) and +8 (rsp)). */
    for (int pi = 0; pi < nf->goto_pair_count; pi++) {
        for (int w = 0; w < 2; w++) {
            if (n >= ZERO_CC_TRAMP_MAX_UNITS) return;
            u[n].is_fp = false;
            u[n].fin_slot = gpr_run < 8 ? gpr_run++ : -1;
            u[n].fin_stack = u[n].fin_slot < 0 ? decl_stack_run++ : -1;
            u[n].src = 0;
            u[n].src_off = (int)(32 * C + 16 * pi) + 8 * w;
            n++;
        }
    }
    int entry_gpr_off = 8;    /* first entry GPR save: [s0-8] */
    int entry_fp_off = 80;    /* first entry FP save: [s0-80] */
    int entry_st_off = 208;   /* first entry stack save: [s0-208] */

    int j = 0;
    for (size_t ui = 0; ui < U; ui++) {
        const ZeroCCType *t = &nf->param_types[C + ui];
        int slots = zero_cc_tramp_unit_slots(t);
        bool isfp = zero_cc_tramp_unit_is_fp(t);
        for (int w = 0; w < slots; w++) {
            if (n >= ZERO_CC_TRAMP_MAX_UNITS) return;
            u[n].is_fp = isfp;
            u[n].fin_slot = isfp ? (fp_run < 8 ? fp_run++ : -1)
                                 : (gpr_run < 8 ? gpr_run++ : -1);
            u[n].fin_stack = u[n].fin_slot < 0 ? decl_stack_run++ : -1;
            if (ue_isfp[j]) {
                if (ue_reg[j] >= 0) {
                    u[n].src = 2;
                    u[n].src_off = entry_fp_off + 16 * ue_reg[j];
                } else {
                    u[n].src = 3;
                    u[n].src_off = entry_st_off + 8 * ue_stk[j];
                }
            } else if (ue_reg[j] >= 0) {
                u[n].src = 1;
                u[n].src_off = entry_gpr_off + 8 * ue_reg[j];
            } else {
                u[n].src = 3;
                u[n].src_off = entry_st_off + 8 * ue_stk[j];
            }
            n++;
            j++;
        }
    }
    int fin_stack_n = decl_stack_run;

    size_t D = 208 + 8 * (size_t)entry_stack_n + 8 * (size_t)copy_words;
    if (decl_stack_run & 1) D += 8; /* pre-push alignment pad (CALL model) */
    D = (D + 15) & ~(size_t)15;

    /* a0..a7 = 10..17, fa0..fa7 = 10..17 (FP-numbering) */
    /* ---- prologue ---- */
    riscv_emit_u32(c, rv64_I(RV64_SP, RV64_SP, 0, -16, 0x13));    /* addi sp, sp, -16 */
    riscv_emit_u32(c, rv64_S(RV64_SP, RV64_RA, 3, 8, 0x23));      /* sd ra, 8(sp) */
    riscv_emit_u32(c, rv64_S(RV64_SP, RV64_S0, 3, 0, 0x23));      /* sd s0, 0(sp) */
    riscv_emit_u32(c, rv64_I(RV64_S0, RV64_SP, 0, 0, 0x13));      /* mv s0, sp */
    /* allocate the stub frame - WITHOUT this the entry saves below
     * write into the CALLER's live stack. D <= 2047 in practice; use
     * t0 for the (theoretical) large case. */
    if (D <= 2047) {
        riscv_emit_u32(c, rv64_I(RV64_SP, RV64_SP, 0, -(int32_t)D, 0x13));  /* addi sp, sp, -D */
    } else {
        riscv64_emit_mov_imm(c, RV64_T0, (int64_t)D); /* li t0, D (t0 free at entry) */
        riscv_emit_u32(c, rv64_R(RV64_SP, RV64_SP, RV64_T0, 0, 0x20));  /* sub sp, sp, t0 (f7=0x20) */
    }

    /* save entry GPR arg regs a0..a7 */
    for (int g = 0; g < 8; g++)
        riscv_emit_u32(c, rv64_S(RV64_S0, 10 + g, 3, -(int32_t)(entry_gpr_off + 8 * g), 0x23));
    /* save entry FP arg regs fa0..fa7 */
    for (int f = 0; f < 8; f++)
        riscv_emit_u32(c, rv64_S(RV64_S0, 10 + f, 3, -(int32_t)(entry_fp_off + 16 * f), 0x27));
    /* save entry stack units: [s0+16+8m] -> [s0-208-8m] (via t1) */
    for (int s = 0; s < entry_stack_n; s++) {
        riscv_emit_u32(c, rv64_I(RV64_T1, RV64_S0, 3, 16 + 8 * s, 0x03)); /* ld t1, (16+8s)(s0) */
        riscv_emit_u32(c, rv64_S(RV64_S0, RV64_T1, 3, -(int32_t)(entry_st_off + 8 * s), 0x23));
    }

    /* ctx base in t0: auipc+addi pair with the data reloc (same shape
     * as OP_MOV_GLOBAL - the ELF writer patches the addi's imm). */
    riscv_emit_u32(c, (uint32_t)(RV64_T0 << 7) | 0x17u);          /* auipc t0, 0 */
    riscv_emit_u32(c, (uint32_t)(RV64_T0 << 15) | (uint32_t)(RV64_T0 << 7) | 0x13u); /* addi t0, t0, 0 */
    if (c->reloc_count < ZERO_CC_MAX_RELOCS) {
        ZeroCCReloc *r = &c->relocs[c->reloc_count++];
        r->text_patch_offset = me_get_offset(c) - 4;
        snprintf(r->target_symbol, sizeof(r->target_symbol), "%s", nf->tramp_ctx_name);
        r->is_call = false;
    } else {
        zero_cc_reloc_overflow_note(c);
    }

    /* copy >16B by-value struct captures from ctx into the frame
     * (storage for capture ci at [s0 - (208 + 8m + 32ci + 8w)]). */
    for (size_t ci = 0; ci < C; ci++) {
        if (nf->nested_capture_byref[ci]) continue;
        const ZeroCCType *t = &nf->nested_capture_types[ci];
        bool big_struct = (t->tag == TYPE_STRUCT || t->tag == TYPE_UNION) &&
                          !zero_cc_type_is_vector(t) && t->size > 16;
        if (!big_struct) continue;
        int words = (int)((t->size + 7) / 8);
        for (int w = 0; w < words; w++) {
            riscv_emit_u32(c, rv64_I(RV64_T1, RV64_T0, 3, 32 * (int)ci + 8 * w, 0x03)); /* ld t1, (32ci+8w)(t0) */
            riscv_emit_u32(c, rv64_S(RV64_S0, RV64_T1, 3,
                                     -(int32_t)(entry_st_off + 8 * entry_stack_n + 32 * (int)ci + 8 * w), 0x23));
        }
    }

    /* ---- place final REGISTER units ---- */
    for (int i = 0; i < n; i++) {
        ZeroCCTrampUnit *a = &u[i];
        if (a->fin_slot < 0) continue;
        int dst = 10 + a->fin_slot;   /* a0..a7 / fa0..fa7 */
        if (a->is_fp) {
            switch (a->src) {
            case 0:  /* ctx: fld dst, off(t0) */
                riscv_emit_u32(c, rv64_I(dst, RV64_T0, 3, a->src_off, 0x07));
                break;
            case 2:  /* entry FP save */
            case 3:  /* entry stack save */
                riscv_emit_u32(c, rv64_I(dst, RV64_S0, 3, -a->src_off, 0x07));
                break;
            default: break;
            }
        } else {
            switch (a->src) {
            case 0:  /* ctx: ld dst, off(t0) */
                riscv_emit_u32(c, rv64_I(dst, RV64_T0, 3, a->src_off, 0x03));
                break;
            case 1:  /* entry GPR save */
            case 3:  /* entry stack save */
                riscv_emit_u32(c, rv64_I(dst, RV64_S0, 3, -a->src_off, 0x03));
                break;
            case 5:  /* address of the big-capture frame storage */
                riscv64_emit_mov_imm(c, RV64_T5, -(int64_t)a->src_off);
                riscv_emit_u32(c, rv64_R(dst, RV64_S0, RV64_T5, 0, 0)); /* add dst, s0, t5 */
                break;
            default: break;
            }
        }
    }

    /* ---- final STACK units: push in DESCENDING index order so unit 0
     * ends up at the lowest address = [sp] just before the jal = the
     * real fn's stack arg 0 (jal leaves SP untouched) ---- */
    for (int idx = fin_stack_n - 1; idx >= 0; idx--) {
        for (int i = 0; i < n; i++) {
            ZeroCCTrampUnit *a = &u[i];
            if (a->fin_stack != idx) continue;
            if (a->is_fp) {
                switch (a->src) {
                case 0:
                    riscv_emit_u32(c, rv64_I(28 /* ft8 */, RV64_T0, 3, a->src_off, 0x07));
                    break;
                case 2:
                case 3:
                    riscv_emit_u32(c, rv64_I(28, RV64_S0, 3, -a->src_off, 0x07));
                    break;
                default: break;
                }
                riscv_emit_u32(c, rv64_I(RV64_SP, RV64_SP, 0, -8, 0x13)); /* addi sp, sp, -8 */
                riscv_emit_u32(c, rv64_S(RV64_SP, 28, 3, 0, 0x27));       /* fsd ft8, 0(sp) */
            } else {
                switch (a->src) {
                case 0:
                    riscv_emit_u32(c, rv64_I(RV64_T1, RV64_T0, 3, a->src_off, 0x03));
                    break;
                case 1:
                case 3:
                    riscv_emit_u32(c, rv64_I(RV64_T1, RV64_S0, 3, -a->src_off, 0x03));
                    break;
                case 5:
                    riscv64_emit_mov_imm(c, RV64_T1, -(int64_t)a->src_off);
                    riscv_emit_u32(c, rv64_R(RV64_T1, RV64_S0, RV64_T1, 0, 0)); /* t1 = s0 - off */
                    break;
                default: break;
                }
                riscv_emit_u32(c, rv64_I(RV64_SP, RV64_SP, 0, -8, 0x13)); /* addi sp, sp, -8 */
                riscv_emit_u32(c, rv64_S(RV64_SP, RV64_T1, 3, 0, 0x23));  /* sd t1, 0(sp) */
            }
        }
    }

    /* 16-byte stack alignment at the jal: handled by the pre-push pad
     * in the prologue frame (same model as zerocc's OP_CALL lowering:
     * pad 8 when fin_stack_n is ODD, BEFORE the pushes - a post-push
     * pad would shift the stack units away from [entry_sp], where the
     * real fn's prologue reads the shared stream). jal leaves SP
     * untouched (the return address goes in ra). */

    /* jal ra, real-nested-fn - same text image, exact J-type offset
     * (jal is PC-relative with PC = the jal's own address). */
    size_t call_pos = me_get_offset(c);
    riscv_emit_u32(c, rv64_J(1 /* ra */, (int32_t)(nf->text_offset - call_pos)));

    /* the target's `ret` lands here: return to the original caller */
    riscv_emit_u32(c, rv64_I(RV64_SP, RV64_S0, 0, 0, 0x13));  /* addi sp, s0, 0 */
    riscv_emit_u32(c, rv64_I(RV64_RA, RV64_SP, 3, 8, 0x03));  /* ld ra, 8(sp) */
    riscv_emit_u32(c, rv64_I(RV64_S0, RV64_SP, 3, 0, 0x03));  /* ld s0, 0(sp) */
    riscv_emit_u32(c, rv64_I(RV64_SP, RV64_SP, 0, 16, 0x13)); /* addi sp, sp, 16 */
    riscv_emit_u32(c, 0x00008067u);                           /* ret (jalr zero, 0(ra)) */
}

/* ---- GNU __int128 (Sprint 109): (lo,hi) GPR-pair decompositions ----
 * On RISC-V a REG_CLASS_GPR128 value always spills (regalloc returns -1)
 * and no current test exercises 128-bit integers on this ISA, so these are
 * best-effort (lo,hi) decompositions built from the 64-bit M/D-ext
 * primitives (RISC-V has no carry-flag add, so carry/borrow are computed
 * with sltu). mul128 uses the t0/t1 scratch regs (assumed free - the
 * spill-only path never keeps them live as 128-bit operand halves). */
static void riscv64_emit_add128(ZeroCCCompiler *c, int dlo, int dhi, int alo, int ahi, int blo, int bhi) {
    if (dlo == alo) {
        riscv64_emit_mov_reg(c, 5, alo);                 /* t0 = alo */
        riscv64_emit_add(c, dlo, alo, blo);              /* dlo = alo + blo */
        riscv_emit_u32(c, rv64_R(RV_REG(5), RV_REG(dlo), RV_REG(5), 3, 0)); /* sltu t0, dlo, t0 (carry: sum < addend) */
    } else {
        riscv64_emit_add(c, dlo, alo, blo);              /* dlo = alo + blo */
        riscv_emit_u32(c, rv64_R(RV_REG(5), RV_REG(dlo), RV_REG(alo), 3, 0)); /* sltu t0, dlo, alo (carry: sum < addend; funct3=3 = SLTU, NOT 2=SLT) */
    }
    riscv64_emit_add(c, dhi, ahi, bhi);
    riscv64_emit_add(c, dhi, dhi, 5);                 /* dhi += carry */
}
static void riscv64_emit_sub128(ZeroCCCompiler *c, int dlo, int dhi, int alo, int ahi, int blo, int bhi) {
    riscv_emit_u32(c, rv64_R(RV_REG(5), RV_REG(alo), RV_REG(blo), 3, 0)); /* sltu t0, alo, blo (borrow: minuend < subtrahend; must precede sub in case dlo == alo) */
    riscv64_emit_sub(c, dlo, alo, blo);              /* dlo = alo - blo */
    riscv64_emit_sub(c, dhi, ahi, bhi);
    riscv64_emit_sub(c, dhi, dhi, 5);                 /* dhi -= borrow */
}
static void riscv64_emit_mul128(ZeroCCCompiler *c, int dlo, int dhi, int alo, int ahi, int blo, int bhi) {
    /* low 128 bits of a 128x128 product (unsigned): */
    riscv64_emit_mul(c, dlo, alo, blo);              /* dlo = lo(alo*blo) */
    riscv_emit_u32(c, rv64_R(RV_REG(dhi), RV_REG(alo), RV_REG(blo), 3, 1)); /* mulhu dhi, alo, blo */
    riscv64_emit_mul(c, 5, alo, bhi);                /* t0 = lo(alo*bhi) */
    riscv64_emit_add(c, dhi, dhi, 5);
    riscv64_emit_mul(c, 5, ahi, blo);                /* t0 = lo(ahi*blo) */
    riscv64_emit_add(c, dhi, dhi, 5);
}

static void riscv64_emit_mul128_spilled(ZeroCCCompiler *c,
                                        int32_t d_lo, int32_t d_hi,
                                        int32_t a_lo, int32_t a_hi,
                                        int32_t b_lo, int32_t b_hi) {
    /* Spilled MUL128 (Sprint 109): all operands live in frame slots. Uses
     * only the reserved 128-bit scratch registers (never hold a live
     * value): pair0 (plo,phi) and pair1.lo (b1). */
    int base = zero_cc_frame_base_reg(c);
    int plo = BE(c)->g128_scratch_lo[0], phi = BE(c)->g128_scratch_hi[0];
    int b1 = BE(c)->g128_scratch_lo[1];
    /* P0 = a_lo * b_lo */
    BE(c)->emit_load(c, plo, base, a_lo);
    BE(c)->emit_load(c, b1, base, b_lo);
    riscv_emit_u32(c, rv64_R(RV_REG(phi), RV_REG(plo), RV_REG(b1), 3, 1)); /* mulhu phi, plo, b1 */
    riscv64_emit_mul(c, b1, plo, b1);                    /* b1 = P0.lo */
    BE(c)->emit_store(c, b1, base, d_lo);
    BE(c)->emit_store(c, phi, base, d_hi);
    /* P1 = a_lo * b_hi; dhi += P1.lo */
    BE(c)->emit_load(c, b1, base, b_hi);
    riscv64_emit_mul(c, phi, plo, b1);                   /* phi = P1.lo */
    BE(c)->emit_load(c, b1, base, d_hi);
    riscv64_emit_add(c, b1, b1, phi);
    BE(c)->emit_store(c, b1, base, d_hi);
    /* P2 = a_hi * b_lo; dhi += P2.lo */
    BE(c)->emit_load(c, plo, base, a_hi);
    BE(c)->emit_load(c, b1, base, b_lo);
    riscv64_emit_mul(c, phi, plo, b1);                   /* phi = P2.lo */
    BE(c)->emit_load(c, b1, base, d_hi);
    riscv64_emit_add(c, b1, b1, phi);
    BE(c)->emit_store(c, b1, base, d_hi);
}
/* MUL128 with an arbitrary mix of register-pair / frame-slot operands
 * (Sprint 109 1-scratch-pair fix). Uses only the reserved 128-bit scratch
 * registers (t1=pair0.lo, t3=pair0.hi, t4=pair1.lo, t5=pair1.hi); d is
 * written from scratch, so d may alias a or b. */
static void riscv64_emit_mul128_mixed(ZeroCCCompiler *c,
                                      int d_mem, int d_lo, int d_hi,
                                      int a_mem, int a_lo, int a_hi,
                                      int b_mem, int b_lo, int b_hi) {
    int base = zero_cc_frame_base_reg(c);
    int t1 = BE(c)->g128_scratch_lo[0], t3 = BE(c)->g128_scratch_hi[0];
    int t4 = BE(c)->g128_scratch_lo[1], t5 = BE(c)->g128_scratch_hi[1];
    /* mv dst, src (add rd, rs, x0) */
    #define RV_MV(dst, src) riscv_emit_u32(c, rv64_R(RV_REG(dst), 0, RV_REG(src), 0, 0))
    #define RV_LOAD(reg, home, is_mem) \
        do { if (is_mem) BE(c)->emit_load(c, (reg), base, (home)); \
             else RV_MV(reg, home); } while (0)
    /* P0 = a_lo * b_lo */
    RV_LOAD(t1, a_lo, a_mem);
    RV_LOAD(t4, b_lo, b_mem);
    riscv_emit_u32(c, rv64_R(RV_REG(t3), RV_REG(t1), RV_REG(t4), 3, 1)); /* mulhu t3, t1, t4 */
    riscv64_emit_mul(c, t5, t1, t4);                    /* t5 = P0.lo */
    if (d_mem) {
        BE(c)->emit_store(c, t5, base, d_lo);
        BE(c)->emit_store(c, t3, base, d_hi);
    } else {
        RV_MV(d_lo, t5);
        RV_MV(d_hi, t3);
    }
    /* P1 = a_lo * b_hi; d_hi += P1.lo (t1 still holds a_lo) */
    RV_LOAD(t4, b_hi, b_mem);
    riscv64_emit_mul(c, t3, t1, t4);                    /* t3 = P1.lo */
    if (d_mem) {
        BE(c)->emit_load(c, t4, base, d_hi);
        riscv64_emit_add(c, t4, t4, t3);
        BE(c)->emit_store(c, t4, base, d_hi);
    } else {
        riscv64_emit_add(c, d_hi, d_hi, t3);
    }
    /* P2 = a_hi * b_lo; d_hi += P2.lo */
    RV_LOAD(t1, a_hi, a_mem);
    RV_LOAD(t4, b_lo, b_mem);
    riscv64_emit_mul(c, t3, t1, t4);                    /* t3 = P2.lo */
    if (d_mem) {
        BE(c)->emit_load(c, t4, base, d_hi);
        riscv64_emit_add(c, t4, t4, t3);
        BE(c)->emit_store(c, t4, base, d_hi);
    } else {
        riscv64_emit_add(c, d_hi, d_hi, t3);
    }
    #undef RV_MV
    #undef RV_LOAD
}
static void riscv64_emit_and128(ZeroCCCompiler *c, int dlo, int dhi, int alo, int ahi, int blo, int bhi) {
    riscv64_emit_and(c, dlo, alo, blo); riscv64_emit_and(c, dhi, ahi, bhi);
}
static void riscv64_emit_or128(ZeroCCCompiler *c, int dlo, int dhi, int alo, int ahi, int blo, int bhi) {
    riscv64_emit_or(c, dlo, alo, blo); riscv64_emit_or(c, dhi, ahi, bhi);
}
static void riscv64_emit_xor128(ZeroCCCompiler *c, int dlo, int dhi, int alo, int ahi, int blo, int bhi) {
    riscv64_emit_xor(c, dlo, alo, blo); riscv64_emit_xor(c, dhi, ahi, bhi);
}
static void riscv64_emit_not128(ZeroCCCompiler *c, int dlo, int dhi, int slo, int shi) {
    riscv_emit_u32(c, rv64_I(RV_REG(dlo), RV_REG(slo), 4, -1, 0x13));  /* xori dlo, slo, -1 */
    riscv_emit_u32(c, rv64_I(RV_REG(dhi), RV_REG(shi), 4, -1, 0x13));  /* xori dhi, shi, -1 */
}
static void riscv64_emit_neg128(ZeroCCCompiler *c, int dlo, int dhi, int slo, int shi) {
    riscv_emit_u32(c, rv64_R(RV_REG(5), RV_REG(0), RV_REG(slo), 3, 0)); /* sltu t0, 0, slo (borrow of 0-slo = slo!=0; funct3=3 = SLTU; must precede sub) */
    riscv64_emit_sub(c, dlo, 0, slo);                /* dlo = 0 - slo */
    riscv64_emit_sub(c, dhi, 0, shi);
    riscv64_emit_sub(c, dhi, dhi, 5);                /* dhi -= borrow */
}
static void riscv64_emit_mov128(ZeroCCCompiler *c, int dlo, int dhi, int slo, int shi) {
    riscv64_emit_mov_reg(c, dlo, slo); riscv64_emit_mov_reg(c, dhi, shi);
}
static void riscv64_emit_load128(ZeroCCCompiler *c, int dlo, int dhi, int base, int32_t offset) {
    /* Aliasing: if dlo == base, load the hi half first; the self-referential
     * lo load `ld dlo, 0(base)` is safe on RISC-V. */
    if (dlo == base) {
        riscv64_emit_load(c, dhi, base, offset + 8);
        riscv64_emit_load(c, dlo, base, offset);
    } else {
        riscv64_emit_load(c, dlo, base, offset);
        riscv64_emit_load(c, dhi, base, offset + 8);
    }
}
static void riscv64_emit_store128(ZeroCCCompiler *c, int slo, int shi, int base, int32_t offset) {
    riscv64_emit_store(c, slo, base, offset);
    riscv64_emit_store(c, shi, base, offset + 8);
}
static void riscv64_emit_zext64_128(ZeroCCCompiler *c, int dlo, int dhi, int src) {
    riscv64_emit_mov_reg(c, dlo, src);
    riscv64_emit_mov_imm(c, dhi, 0);
}
static void riscv64_emit_sext64_128(ZeroCCCompiler *c, int dlo, int dhi, int src) {
    riscv64_emit_mov_reg(c, dlo, src);
    riscv_emit_u32(c, rv64_I(RV_REG(dhi), RV_REG(src), 5, 63, 0x13) | (1u << 30)); /* srai dhi, src, 63 */
}
static void riscv64_emit_trunc128_64(ZeroCCCompiler *c, int dst, int slo) {
    riscv64_emit_mov_reg(c, dst, slo);
}
static void riscv64_emit_mov128_imm(ZeroCCCompiler *c, int dlo, int dhi, int64_t imm) {
    riscv64_emit_mov_imm(c, dlo, imm);
    riscv64_emit_mov_imm(c, dhi, 0);
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
    /* 128-bit spill scratch (reserved; excluded from the 64-bit pool, and NOT t0
     * (5) which RISC-V add128/sub128/neg128/mul128 use as carry/intermediate
     * scratch): pair0 = (t1, t3), pair1 = (t4, t5). */
    .g128_scratch_lo = {6, 29},
    .g128_scratch_hi = {28, 31},
    .g128_scratch_pair_count = 2,
    .callee_save_frame_reserve = 96,           /* 12 callee-saved regs: s0-s11 */

    .emit_prologue = riscv64_emit_prologue,
    .emit_leave = riscv64_emit_leave,
    .emit_epilogue = riscv64_emit_epilogue,
    .get_code_offset = riscv64_get_code_offset,
    .emit_stack_guard_init = riscv_emit_stack_guard_init,
    .emit_stack_guard_check = riscv_emit_stack_guard_check,

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

    /* GNU __int128 (Sprint 109) */
    .emit_add128 = riscv64_emit_add128,
    .emit_sub128 = riscv64_emit_sub128,
    .emit_mul128 = riscv64_emit_mul128,
    .emit_mul128_spilled = riscv64_emit_mul128_spilled,
    .emit_mul128_mixed = riscv64_emit_mul128_mixed,
    .emit_and128 = riscv64_emit_and128,
    .emit_or128 = riscv64_emit_or128,
    .emit_xor128 = riscv64_emit_xor128,
    .emit_not128 = riscv64_emit_not128,
    .emit_neg128 = riscv64_emit_neg128,
    .emit_mov128 = riscv64_emit_mov128,
    .emit_load128 = riscv64_emit_load128,
    .emit_store128 = riscv64_emit_store128,
    .emit_zext64_128 = riscv64_emit_zext64_128,
    .emit_sext64_128 = riscv64_emit_sext64_128,
    .emit_trunc128_64 = riscv64_emit_trunc128_64,
    .emit_mov128_imm = riscv64_emit_mov128_imm,

    .patch_jmp = riscv64_patch_jmp,
    .resolve_labels = riscv_resolve_labels,

    .emit_load = riscv64_emit_load,
    .emit_store = riscv64_emit_store,
    .emit_vload = riscv64_emit_vload,
    .emit_vstore = riscv64_emit_vstore,
    .emit_vloadss = riscv64_emit_vloadss,
    .emit_vloadsd = riscv64_emit_load_fp,
    .emit_vstoress = riscv64_emit_vstoress,
    .emit_vstoredsd = riscv64_emit_store_fp,
    .emit_vloadss_raw = riscv64_emit_vloadss_raw,
    .emit_vstoress_raw = riscv64_emit_vstoress_raw,
    .emit_vaddps = riscv64_emit_vaddps,
    .emit_vsubps = riscv64_emit_vsubps,
    .emit_vmulps = riscv64_emit_vmulps,
    .emit_vdivps = riscv64_emit_vdivps,
    .emit_vaddpd = riscv64_emit_fp_add,
    .emit_vsubpd = riscv64_emit_fp_sub,
    .emit_vmulpd = riscv64_emit_fp_mul,
    .emit_vdivpd = riscv64_emit_fp_div,
    .emit_vpaddd = riscv64_emit_add,
    .emit_vpsubd = riscv64_emit_sub,
    .emit_vpmulld = riscv64_emit_mul,
    .emit_vmovaps = riscv64_emit_vmovaps,
    .emit_cvtss2sd = riscv64_emit_fp_cvtss2sd,
    .emit_cvttsd2ss = riscv64_emit_fp_cvttsd2ss,
    .emit_cvtsi2ss = riscv64_emit_fp_cvtsi2ss,
    .emit_cvtsd2ss = riscv64_emit_fp_cvtsd2ss,
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
    .emit_jmp_reg = riscv64_emit_jmp_reg,
    .emit_lea_label = riscv64_emit_lea_label,
    .emit_ret = riscv64_emit_ret,
    .emit_syscall = riscv_emit_syscall,
    .emit_clone = riscv_emit_clone,

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
    .emit_atomic_cmpxchg_bool = riscv64_emit_atomic_cmpxchg_bool,
    .emit_atomic_xadd = riscv64_emit_atomic_xadd,
    .emit_atomic_or = riscv64_emit_atomic_or,
    .emit_atomic_and = riscv64_emit_atomic_and,
    .emit_atomic_xor = riscv64_emit_atomic_xor,
    .emit_atomic_xchg = riscv64_emit_atomic_xchg,
    .emit_mfence = riscv64_emit_mfence,
    .emit_trap = riscv64_emit_trap,
    .emit_ovf_trap = riscv64_emit_ovf_trap,
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

/* RISC-V 32-bit backend (conv table, frame fns, vtable) moved to
 * riscv32.c - Sprint 114. The vtable is copy-initialized from the RV64
 * table by riscv32_init_backend(). */

/* Sprint 99: OP_NONLOCAL_GOTO absolute jump (AOT).
 * auipc t0, 0   (R_RISCV_PCREL_HI20 against a local label symbol at
 * text_target_offset, attached by the .o writer)
 * jalr zero, 0(t0) (R_RISCV_PCREL_LO12_I against the auipc position)
 * t0 is a caller-saved scratch, dead after the jump. */
void zero_cc_riscv_emit_nonlocal_goto_jump(ZeroCCCompiler *c, size_t text_offset) {
    if (c->isa != ISA_RISCV64) return;
    size_t patch_pos = me_get_offset(c);
    uint32_t auipc = (uint32_t)(((uint32_t)RV64_T0 << 7) | 0x17u);
    uint32_t jalr  = (uint32_t)(((uint32_t)RV64_T0 << 15) | 0x67u);
    riscv_emit_u32(c, auipc);
    riscv_emit_u32(c, jalr);
    if (c->nl_goto_reloc_count < 64) {
        c->nl_goto_relocs[c->nl_goto_reloc_count].text_patch_offset = patch_pos;
        c->nl_goto_relocs[c->nl_goto_reloc_count].text_target_offset = text_offset;
        c->nl_goto_reloc_count++;
    }
}
