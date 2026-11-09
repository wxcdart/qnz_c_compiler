/*
 * RISC-V Assembly Emitter
 * ISA-specific assembly emission for GNU Assembler (GAS) syntax
 */

#include "zero_cc/isa/isa_backend.h"
#include "zero_cc/ir/ir.h"
#include "zero_cc/ir/regalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Hardware register names for RISC-V (indexed by hardware number) */
static const char *riscv_hw_reg_names[] = {
    "zero", "ra", "sp", "gp", "tp", "t0", "t1", "t2",
    "s0", "s1", "a0", "a1", "a2", "a3", "a4", "a5",
    "a6", "a7", "s2", "s3", "s4", "s5", "s6", "s7",
    "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6"
};

/* Get register name by hardware number */
static const char *riscv_get_hw_reg_name(int phys_reg) {
    if (phys_reg >= 0 && phys_reg < 32) {
        return riscv_hw_reg_names[phys_reg];
    }
    return "a0";  /* Default */
}

/* Get vector register name by index */
static const char *riscv_get_vec_reg_name(int vec_reg_idx) {
    static const char *const riscv_vec_names[] = {
        "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7",
        "v8", "v9", "v10", "v11", "v12", "v13", "v14", "v15",
        "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23",
        "v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31"
    };
    if (vec_reg_idx < 0 || vec_reg_idx >= 32) {
        return "v0";  /* Default to v0 for invalid */
    }
    return riscv_vec_names[vec_reg_idx];
}

/* Prologue: allocate frame with s0 (fp) at top of local frame so negative offsets -off(s0) lie above sp */
static void riscv_emit_prologue_asm(ZeroCCCompiler *c, size_t stack_size) {
    if (c->lowering.is_frameless) return;
    FILE *out = c->asm_out ? c->asm_out : stdout;
    size_t aligned_sz = (stack_size + 15) & ~((size_t)15);
    size_t total_alloc = aligned_sz + 16; /* 16 bytes for ra and s0 above s0 */

    if (c->lowering.frame_align > 16) {
        /* C11 _Alignas(N) realign mode: save ra/s0, s0 = pre-realign base,
         * andi sp to N, allocate the N-rounded frame below SP'. CSR slots
         * are SP-anchored at frame-offset(sp). Epilogue = FP-mode leave. */
        const char *st_op = (c->isa == ISA_RISCV32) ? "sw" : "sd";
        size_t N = (size_t)c->lowering.frame_align;
        size_t frame = (size_t)c->lowering.frame_delta;
        fprintf(out, "\taddi\tsp, sp, -16\n");
        fprintf(out, "\t%s\tra, %d(sp)\n", st_op, (c->isa == ISA_RISCV32) ? 4 : 8);
        fprintf(out, "\t%s\ts0, 0(sp)\n", st_op);
        fprintf(out, "\taddi\ts0, sp, 0\n");
        if (N < 4096) {
            fprintf(out, "\tandi\tsp, sp, -%zu\n", N);
        } else {
            /* N == 4096: ~(N-1) needs bits above the 12-bit andi immediate.
             * Use a register mask (t0 is free at function entry). */
            fprintf(out, "\tli\tt0, -%zu\n", N);
            fprintf(out, "\tand\tsp, sp, t0\n");
        }
        if (frame <= 2047) {
            fprintf(out, "\taddi\tsp, sp, -%zu\n", frame);
        } else {
            /* addi's 12-bit immediate caps the frame at 2047 */
            fprintf(out, "\tli\tt0, %zu\n", frame);
            fprintf(out, "\tsub\tsp, sp, t0\n");
        }

        const int *callee_saved = BE(c)->conv->callee_saved_gpr;
        int count = BE(c)->conv->callee_saved_gpr_count;
        uint32_t mask = c->regalloc.used_callee_saved_gpr_mask;

        for (int i = 0; i < count; i++) {
            int reg = callee_saved[i];
            if (reg == 8) continue; /* s0 already saved above */
            if (!((mask >> reg) & 1u)) continue;
            const char *reg_name = BE(c)->get_hw_reg_name(reg);
            int offset = BE(c)->conv->callee_save_slot_offsets[i];
            int disp = (int)(frame - (size_t)offset);
            if (disp >= -2048 && disp <= 2047) {
                fprintf(out, "\t%s\t%s, %d(sp)\n", st_op, reg_name, disp);
            } else {
                fprintf(out, "\tli\tt0, %d\n", disp);
                fprintf(out, "\tadd\tt0, sp, t0\n");
                fprintf(out, "\t%s\t%s, 0(t0)\n", st_op, reg_name);
            }
        }
        /* Sprint 99: SP snapshot for nonlocal-goto pair marshalling. */
        if (c->lowering.sp_snap_off > 0) {
            fprintf(out, "\t%s\tsp, %d(sp)\n", st_op,
                    (int)(frame - (size_t)c->lowering.sp_snap_off));
        }
        return;
    }

    if (c->lowering.omit_frame_pointer) {
        /* Option 4: no frame pointer (s0). The decision in ast_lowering.c
         * guarantees total_alloc <= 2047. The s0 slot is left unused, so
         * slot geometry (and the delta) match the FP layout. */
        const char *s = (c->isa == ISA_RISCV32) ? "sw" : "sd";
        fprintf(out, "\taddi\tsp, sp, -%zu\n", total_alloc);
        fprintf(out, "\t%s\tra, %zu(sp)\n", s, aligned_sz + 8);

        /* Save callee-saved registers s1..s11 at (aligned_sz-offset)(sp) */
        const int *callee_saved = BE(c)->conv->callee_saved_gpr;
        int count = BE(c)->conv->callee_saved_gpr_count;
        uint32_t mask = c->regalloc.used_callee_saved_gpr_mask;

        for (int i = 0; i < count; i++) {
            int reg = callee_saved[i];
            if (!((mask >> reg) & 1u)) continue;
            const char *reg_name = BE(c)->get_hw_reg_name(reg);
            int offset = BE(c)->conv->callee_save_slot_offsets[i];
            fprintf(out, "\t%s\t%s, %d(sp)\n", s, reg_name,
                    (int)(aligned_sz - (size_t)offset));
        }
        /* Sprint 99: SP snapshot + entry-s0 snapshot (s0 is untouched in
         * OFP mode, so it still holds the caller's value). */
        if (c->lowering.sp_snap_off > 0) {
            fprintf(out, "\t%s\tsp, %d(sp)\n", s,
                    (int)(aligned_sz - (size_t)c->lowering.sp_snap_off));
            if (c->lowering.fp_snap_off > 0) {
                fprintf(out, "\t%s\ts0, %d(sp)\n", s,
                        (int)(aligned_sz - (size_t)c->lowering.fp_snap_off));
            }
        }
        return;
    }

    const char *st_op = (c->isa == ISA_RISCV32) ? "sw" : "sd";
    if (total_alloc > 2047 || aligned_sz > 2047) {
        /* Large frame (many spilled 128-bit temps): addi/12-bit store offsets
         * cannot express the frame; build the addresses in scratch regs and
         * save ra/s0 relative to the freshly computed s0. (Sprint 109) */
        fprintf(out, "\tli\tt0, %zu\n", total_alloc);
        fprintf(out, "\tsub\tsp, sp, t0\n");
        fprintf(out, "\tli\tt1, %zu\n", aligned_sz);
        fprintf(out, "\tadd\ts0, sp, t1\n");
        fprintf(out, "\t%s\tra, %d(s0)\n", st_op, (c->isa == ISA_RISCV32) ? 4 : 8);
        fprintf(out, "\t%s\ts0, 0(s0)\n", st_op);
    } else {
        fprintf(out, "\taddi\tsp, sp, -%zu\n", total_alloc);
        fprintf(out, "\t%s\tra, %zu(sp)\n", st_op, aligned_sz + ((c->isa == ISA_RISCV32) ? 4 : 8));
        fprintf(out, "\t%s\ts0, %zu(sp)\n", st_op, aligned_sz);
        fprintf(out, "\taddi\ts0, sp, %zu\n", aligned_sz);
    }

    /* Save callee-saved registers s1..s11 at -offset(s0) (only allocated ones) */
    const int *callee_saved = BE(c)->conv->callee_saved_gpr;
    int count = BE(c)->conv->callee_saved_gpr_count;
    uint32_t mask = c->regalloc.used_callee_saved_gpr_mask;

    for (int i = 0; i < count; i++) {
        int reg = callee_saved[i];
        if (reg == 8) continue; /* s0 already saved above */
        if (!((mask >> reg) & 1u)) continue;
        const char *reg_name = BE(c)->get_hw_reg_name(reg);
        int offset = BE(c)->conv->callee_save_slot_offsets[i];
        if (c->isa == ISA_RISCV32) {
            fprintf(out, "\tsw\t%s, -%d(s0)\n", reg_name, offset);
        } else {
            fprintf(out, "\tsd\t%s, -%d(s0)\n", reg_name, offset);
        }
    }
    /* Sprint 99: SP snapshot for nonlocal-goto pair marshalling. */
    if (c->lowering.sp_snap_off > 0) {
        if (c->isa == ISA_RISCV32) {
            fprintf(out, "\tsw\tsp, -%d(s0)\n", (int)c->lowering.sp_snap_off);
        } else {
            fprintf(out, "\tsd\tsp, -%d(s0)\n", (int)c->lowering.sp_snap_off);
        }
    }
}

/* Epilogue: restore callee-saved, restore ra/s0, deallocate frame, ret */
static void riscv_emit_epilogue_asm(ZeroCCCompiler *c) {
    FILE *out = c->asm_out ? c->asm_out : stdout;
    if (c->lowering.is_frameless) {
        fprintf(out, "\tret\n");
        return;
    }

    if (c->lowering.omit_frame_pointer && c->lowering.frame_align <= 16) {
        /* Option 4: no s0. aligned_sz = frame_delta. */
        size_t aligned_sz = (size_t)c->lowering.frame_delta;
        const char *l = (c->isa == ISA_RISCV32) ? "lw" : "ld";

        /* Restore callee-saved registers (only allocated ones) */
        const int *callee_saved = BE(c)->conv->callee_saved_gpr;
        int count = BE(c)->conv->callee_saved_gpr_count;
        uint32_t mask = c->regalloc.used_callee_saved_gpr_mask;

        for (int i = 0; i < count; i++) {
            int reg = callee_saved[i];
            if (!((mask >> reg) & 1u)) continue;
            const char *reg_name = BE(c)->get_hw_reg_name(reg);
            int offset = BE(c)->conv->callee_save_slot_offsets[i];
            fprintf(out, "\t%s\t%s, %d(sp)\n", l, reg_name,
                    (int)(aligned_sz - (size_t)offset));
        }

        fprintf(out, "\t%s\tra, %zu(sp)\n", l, aligned_sz + 8);
        fprintf(out, "\taddi\tsp, sp, %zu\n", aligned_sz + 16);
        fprintf(out, "\tret\n");
        return;
    }

    /* Restore callee-saved registers s1..s11 (only allocated ones);
     * _Alignas realign mode: SP-anchored at frame-offset(sp). */
    const int *callee_saved = BE(c)->conv->callee_saved_gpr;
    int count = BE(c)->conv->callee_saved_gpr_count;
    uint32_t mask = c->regalloc.used_callee_saved_gpr_mask;

    for (int i = 0; i < count; i++) {
        int reg = callee_saved[i];
        if (reg == 8) continue;
        if (!((mask >> reg) & 1u)) continue;
        const char *reg_name = BE(c)->get_hw_reg_name(reg);
        int offset = BE(c)->conv->callee_save_slot_offsets[i];
        const char *ld_op2 = (c->isa == ISA_RISCV32) ? "lw" : "ld";
        if (c->lowering.frame_align > 16) {
            int disp = (int)c->lowering.frame_delta - offset;
            if (disp >= -2048 && disp <= 2047) {
                fprintf(out, "\t%s\t%s, %d(sp)\n", ld_op2, reg_name, disp);
            } else {
                fprintf(out, "\tli\tt0, %d\n", disp);
                fprintf(out, "\tadd\tt0, sp, t0\n");
                fprintf(out, "\t%s\t%s, 0(t0)\n", ld_op2, reg_name);
            }
        } else if (c->isa == ISA_RISCV32) {
            fprintf(out, "\tlw\t%s, -%d(s0)\n", reg_name, offset);
        } else {
            fprintf(out, "\tld\t%s, -%d(s0)\n", reg_name, offset);
        }
    }

    const char *ld_op = (c->isa == ISA_RISCV32) ? "lw" : "ld";
    fprintf(out, "\taddi\tsp, s0, 0\n");
    fprintf(out, "\t%s\tra, %d(sp)\n", ld_op, (c->isa == ISA_RISCV32) ? 4 : 8);
    fprintf(out, "\t%s\ts0, 0(sp)\n", ld_op);
    fprintf(out, "\taddi\tsp, sp, 16\n");
    fprintf(out, "\tret\n");
}

/* MOV_IMM: li dst, imm */
static void riscv_emit_mov_imm_asm(ZeroCCCompiler *c, int dst, int64_t imm, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    fprintf(out, "\tli\t%s, %ld\n", dst_name, (long)imm);
}

/* MOV_REG: mv dst, src */
static void riscv_emit_mov_reg_asm(ZeroCCCompiler *c, int dst, int src, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *src_name = BE(c)->get_hw_reg_name(src);
    fprintf(out, "\tmv\t%s, %s\n", dst_name, src_name);
}

/* LOAD: ld/lw dst, offset(base) */
static void riscv_emit_load_asm(ZeroCCCompiler *c, int dst, int base, int32_t offset, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *base_name = BE(c)->get_hw_reg_name(base);
    const char *op = (c->isa == ISA_RISCV32) ? "lw" : "ld";
    if (offset >= -2048 && offset <= 2047) {
        fprintf(out, "\t%s\t%s, %d(%s)\n", op, dst_name, offset, base_name);
    } else {
        /* S/I-type immediates are 12-bit; large frames need a register base.
         * t5 (x30) is the reserved address scratch (Sprint 109): t0/t1 may
         * hold a live 64-bit spill reload at this point. */
        int scratch = (dst != 30 && base != 30) ? 30 : ((dst != 5 && base != 5) ? 5 : 6);
        const char *sc_name = BE(c)->get_hw_reg_name(scratch);
        fprintf(out, "\tli\t%s, %d\n", sc_name, (int)offset);
        fprintf(out, "\tadd\t%s, %s, %s\n", sc_name, base_name, sc_name);
        fprintf(out, "\t%s\t%s, 0(%s)\n", op, dst_name, sc_name);
    }
}

/* STORE: sd/sw src, offset(base) */
static void riscv_emit_store_asm(ZeroCCCompiler *c, int src, int base, int32_t offset, FILE *out) {
    const char *src_name = BE(c)->get_hw_reg_name(src);
    const char *base_name = BE(c)->get_hw_reg_name(base);
    const char *op = (c->isa == ISA_RISCV32) ? "sw" : "sd";
    if (offset >= -2048 && offset <= 2047) {
        fprintf(out, "\t%s\t%s, %d(%s)\n", op, src_name, offset, base_name);
    } else {
        /* S/I-type immediates are 12-bit; large frames need a register base
         * (t5 = reserved address scratch; see riscv_emit_load_asm). */
        int scratch = (src != 30 && base != 30) ? 30 : ((src != 5 && base != 5) ? 5 : 6);
        const char *sc_name = BE(c)->get_hw_reg_name(scratch);
        fprintf(out, "\tli\t%s, %d\n", sc_name, (int)offset);
        fprintf(out, "\tadd\t%s, %s, %s\n", sc_name, base_name, sc_name);
        fprintf(out, "\t%s\t%s, 0(%s)\n", op, src_name, sc_name);
    }
}

/* ADD: add dst, src1, src2 */
static void riscv_emit_add_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *src1_name = BE(c)->get_hw_reg_name(src1);
    const char *src2_name = BE(c)->get_hw_reg_name(src2);
    fprintf(out, "\tadd\t%s, %s, %s\n", dst_name, src1_name, src2_name);
}

/* SUB: sub dst, src1, src2 */
static void riscv_emit_sub_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *src1_name = BE(c)->get_hw_reg_name(src1);
    const char *src2_name = BE(c)->get_hw_reg_name(src2);
    fprintf(out, "\tsub\t%s, %s, %s\n", dst_name, src1_name, src2_name);
}

/* MUL: mul dst, src1, src2 */
static void riscv_emit_mul_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *src1_name = BE(c)->get_hw_reg_name(src1);
    const char *src2_name = BE(c)->get_hw_reg_name(src2);
    fprintf(out, "\tmul\t%s, %s, %s\n", dst_name, src1_name, src2_name);
}

/* DIV: div/divu dst, src1, src2 */
static void riscv_emit_div_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *src1_name = BE(c)->get_hw_reg_name(src1);
    const char *src2_name = BE(c)->get_hw_reg_name(src2);
    const char *op = c->lowering.div_is_unsigned ? "divu" : "div";
    fprintf(out, "\t%s\t%s, %s, %s\n", op, dst_name, src1_name, src2_name);
}

/* MOD: rem/remu dst, src1, src2 */
static void riscv_emit_mod_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *src1_name = BE(c)->get_hw_reg_name(src1);
    const char *src2_name = BE(c)->get_hw_reg_name(src2);
    const char *op = c->lowering.div_is_unsigned ? "remu" : "rem";
    fprintf(out, "\t%s\t%s, %s, %s\n", op, dst_name, src1_name, src2_name);
}

/* AND: and dst, src1, src2 */
static void riscv_emit_and_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *src1_name = BE(c)->get_hw_reg_name(src1);
    const char *src2_name = BE(c)->get_hw_reg_name(src2);
    fprintf(out, "\tand\t%s, %s, %s\n", dst_name, src1_name, src2_name);
}

/* OR: or dst, src1, src2 */
static void riscv_emit_or_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *src1_name = BE(c)->get_hw_reg_name(src1);
    const char *src2_name = BE(c)->get_hw_reg_name(src2);
    fprintf(out, "\tor\t%s, %s, %s\n", dst_name, src1_name, src2_name);
}

/* XOR: xor dst, src1, src2 */
static void riscv_emit_xor_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *src1_name = BE(c)->get_hw_reg_name(src1);
    const char *src2_name = BE(c)->get_hw_reg_name(src2);
    fprintf(out, "\txor\t%s, %s, %s\n", dst_name, src1_name, src2_name);
}

/* SLL: sll dst, src, shift */
static void riscv_emit_shl_asm(ZeroCCCompiler *c, int dst, int src, int shift, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *src_name = BE(c)->get_hw_reg_name(src);
    const char *shift_name = BE(c)->get_hw_reg_name(shift);
    fprintf(out, "\tsll\t%s, %s, %s\n", dst_name, src_name, shift_name);
}

/* SRL: srl dst, src, shift */
static void riscv_emit_shr_asm(ZeroCCCompiler *c, int dst, int src, int shift, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *src_name = BE(c)->get_hw_reg_name(src);
    const char *shift_name = BE(c)->get_hw_reg_name(shift);
    fprintf(out, "\tsrl\t%s, %s, %s\n", dst_name, src_name, shift_name);
}

/* SRA: sra dst, src, shift */
static void riscv_emit_sar_asm(ZeroCCCompiler *c, int dst, int src, int shift, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *src_name = BE(c)->get_hw_reg_name(src);
    const char *shift_name = BE(c)->get_hw_reg_name(shift);
    fprintf(out, "\tsra\t%s, %s, %s\n", dst_name, src_name, shift_name);
}

/* SEXT/ZEXT: in-place, via slli+srai (sext) or andi / slli+srli (zext). */
static void riscv_emit_extend_asm(ZeroCCCompiler *c, int dst, int src, int width, int sign, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *src_name = BE(c)->get_hw_reg_name(src);
    if (width >= 8 || width == 0) {
        /* Not a narrow extension (width 0 = "no mark"; 8+ = zero-extend
         * flag): at most a move is needed. */
        if (dst != src)
            fprintf(out, "\tmv\t%s, %s\n", dst_name, src_name);
        return;
    }
    int shamt = (width == 1) ? 56 : (width == 2) ? 48 : 32;
    if (c->isa == ISA_RISCV32 && width == 4) {
        /* 32-bit values already fill a RV32 register: extension is a no-op
         * (slli x, x, 32 is illegal on RV32 - shamt field is 5 bits). */
        return;
    }
    if (c->isa == ISA_RISCV32) {
        shamt = (width == 1) ? 31 : 30;
    }
    if (sign) {
        fprintf(out, "\tslli\t%s, %s, %d\n", dst_name, src_name, shamt);
        fprintf(out, "\tsrai\t%s, %s, %d\n", dst_name, dst_name, shamt);
    } else if (width == 1) {
        fprintf(out, "\tandi\t%s, %s, 0xff\n", dst_name, src_name);
    } else if (width == 2) {
        fprintf(out, "\tandi\t%s, %s, 0xffff\n", dst_name, src_name);
    } else {
        fprintf(out, "\tslli\t%s, %s, 32\n", dst_name, src_name);
        fprintf(out, "\tsrli\t%s, %s, 32\n", dst_name, dst_name);
    }
}

/* CMP: sub t0, src1, src2; mv t1, src1; mv t2, src2 */
static void riscv_emit_cmp_asm(ZeroCCCompiler *c, int src1, int src2, FILE *out) {
    const char *src1_name = BE(c)->get_hw_reg_name(src1);
    const char *src2_name = BE(c)->get_hw_reg_name(src2);
    fprintf(out, "\tsub\tt0, %s, %s\n", src1_name, src2_name);
    fprintf(out, "\tmv\tt1, %s\n", src1_name);
    fprintf(out, "\tmv\tt2, %s\n", src2_name);
}

/* COND_SET: set dst to 1/0 based on t0 (src1 - src2) or t1/t2 (unsigned) */
static void riscv_emit_cond_set_asm(ZeroCCCompiler *c, int dst, int cond, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    switch (cond) {
        case 0: /* CC_LT: t0 < 0 */
            fprintf(out, "\tslt\t%s, t0, zero\n", dst_name);
            break;
        case 1: /* CC_LE: !(t0 > 0) */
            fprintf(out, "\tsgt\t%s, t0, zero\n", dst_name);
            fprintf(out, "\txori\t%s, %s, 1\n", dst_name, dst_name);
            break;
        case 2: /* CC_GT: t0 > 0 */
            fprintf(out, "\tsgt\t%s, t0, zero\n", dst_name);
            break;
        case 3: /* CC_GE: !(t0 < 0) */
            fprintf(out, "\tslt\t%s, t0, zero\n", dst_name);
            fprintf(out, "\txori\t%s, %s, 1\n", dst_name, dst_name);
            break;
        case 4: /* CC_EQ: t0 == 0 */
            fprintf(out, "\tseqz\t%s, t0\n", dst_name);
            break;
        case 6: /* CC_ULT: t1 <u t2 */
            fprintf(out, "\tsltu\t%s, t1, t2\n", dst_name);
            break;
        case 7: /* CC_ULE: !(t2 <u t1) */
            fprintf(out, "\tsltu\t%s, t2, t1\n", dst_name);
            fprintf(out, "\txori\t%s, %s, 1\n", dst_name, dst_name);
            break;
        case 8: /* CC_UGT: t2 <u t1 */
            fprintf(out, "\tsltu\t%s, t2, t1\n", dst_name);
            break;
        case 9: /* CC_UGE: !(t1 <u t2) */
            fprintf(out, "\tsltu\t%s, t1, t2\n", dst_name);
            fprintf(out, "\txori\t%s, %s, 1\n", dst_name, dst_name);
            break;
        case 5: /* CC_NE: t0 != 0 */
        default:
            fprintf(out, "\tsnez\t%s, t0\n", dst_name);
            break;
    }
}

/* NEG: sub dst, zero, src */
static void riscv_emit_neg_asm(ZeroCCCompiler *c, int dst, int src, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *src_name = BE(c)->get_hw_reg_name(src);
    fprintf(out, "\tsub\t%s, zero, %s\n", dst_name, src_name);
}

/* ADD_IMM: addi dst, dst, imm (Sprint 40: was missing from -S) */
static void riscv_emit_add_imm_asm(ZeroCCCompiler *c, int dst, int64_t imm, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    fprintf(out, "\taddi\t%s, %s, %ld\n", dst_name, dst_name, (long)imm);
}

/* SUB_IMM: addi dst, dst, -imm */
static void riscv_emit_sub_imm_asm(ZeroCCCompiler *c, int dst, int64_t imm, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    fprintf(out, "\taddi\t%s, %s, %ld\n", dst_name, dst_name, (long)(-imm));
}

/* INC: addi dst, dst, 1 */
static void riscv_emit_inc_asm(ZeroCCCompiler *c, int dst, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    fprintf(out, "\taddi\t%s, %s, 1\n", dst_name, dst_name);
}

/* DEC: addi dst, dst, -1 */
static void riscv_emit_dec_asm(ZeroCCCompiler *c, int dst, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    fprintf(out, "\taddi\t%s, %s, -1\n", dst_name, dst_name);
}

/* NOT: xori dst, src, -1 (copy src first when distinct) */
static void riscv_emit_not_asm(ZeroCCCompiler *c, int dst, int src, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    if (dst != src) {
        const char *src_name = BE(c)->get_hw_reg_name(src);
        fprintf(out, "\tmv\t%s, %s\n", dst_name, src_name);
    }
    fprintf(out, "\txori\t%s, %s, -1\n", dst_name, dst_name);
}

/* LEA: addi dst, base, offset */
static void riscv_emit_lea_asm(ZeroCCCompiler *c, int dst, int base, int32_t offset, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *base_name = BE(c)->get_hw_reg_name(base);
    if (offset >= -2048 && offset <= 2047) {
        fprintf(out, "\taddi\t%s, %s, %d\n", dst_name, base_name, offset);
    } else {
        /* addi's 12-bit immediate caps offsets at +/-2047 (t5 = reserved
         * address scratch; t0 may hold a live spill reload). */
        int scratch = (dst != 30 && base != 30) ? 30 : ((dst != 5 && base != 5) ? 5 : 6);
        const char *sc_name = BE(c)->get_hw_reg_name(scratch);
        fprintf(out, "\tli\t%s, %d\n", sc_name, offset);
        fprintf(out, "\tadd\t%s, %s, %s\n", dst_name, base_name, sc_name);
    }
}

/* DEREF: ld/lw dst, 0(src) */
static void riscv_emit_deref_asm(ZeroCCCompiler *c, int dst, int src, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *src_name = BE(c)->get_hw_reg_name(src);
    const char *op = (c->isa == ISA_RISCV32) ? "lw" : "ld";
    fprintf(out, "\t%s\t%s, 0(%s)\n", op, dst_name, src_name);
}

/* PARAM_SAVE_INT: save incoming integer parameter a0..a7 to -offset(s0) (or SP+delta) */
static void riscv_emit_param_save_int_asm(ZeroCCCompiler *c, int reg_idx, int32_t offset, FILE *out) {
    if (reg_idx < 0 || reg_idx >= 8) return;
    const char *base_name = BE(c)->get_hw_reg_name(zero_cc_frame_base_reg(c));
    const char *s = (c->isa == ISA_RISCV32) ? "sw" : "sd";
    int32_t disp = zero_cc_frame_disp(c, -offset);
    if (disp >= -2048 && disp <= 2047) {
        fprintf(out, "\t%s\ta%d, %d(%s)\n", s, reg_idx, (int)disp, base_name);
    } else {
        /* S-type immediate is 12-bit; large frames need a register base */
        fprintf(out, "\tli\tt0, %d\n", (int)disp);
        fprintf(out, "\tadd\tt0, %s, t0\n", base_name);
        fprintf(out, "\t%s\ta%d, 0(t0)\n", s, reg_idx);
    }
}

/* PARAM_SAVE_FP: save incoming FP parameter fa0..fa7 to -offset(s0) (or SP+delta) */
static void riscv_emit_param_save_fp_asm(ZeroCCCompiler *c, int reg_idx, int32_t offset, int is_single, FILE *out) {
    const char *base_name = BE(c)->get_hw_reg_name(zero_cc_frame_base_reg(c));
    int32_t disp = zero_cc_frame_disp(c, -offset);
    if (is_single && c->isa != ISA_RISCV32) {
        /* Sprint 115m: raw low-32 store - the incoming register's low 32
         * bits ARE the ABI float pattern (no fcvt.s.d narrow). */
        if (disp >= -2048 && disp <= 2047) {
            fprintf(out, "\tfsw\tfa%d, %d(%s)\n", reg_idx, (int)disp, base_name);
        } else {
            fprintf(out, "\tli\tt0, %d\n", (int)disp);
            fprintf(out, "\tadd\tt0, %s, t0\n", base_name);
            fprintf(out, "\tfsw\tfa%d, 0(t0)\n", reg_idx);
        }
    } else {
        const char *s = (c->isa == ISA_RISCV32) ? "fsw" : "fsd";
        if (disp >= -2048 && disp <= 2047) {
            fprintf(out, "\t%s\tfa%d, %d(%s)\n", s, reg_idx, (int)disp, base_name);
        } else {
            fprintf(out, "\tli\tt0, %d\n", (int)disp);
            fprintf(out, "\tadd\tt0, %s, t0\n", base_name);
            fprintf(out, "\t%s\tfa%d, 0(t0)\n", s, reg_idx);
        }
    }
}

/* CALL - Full RISC-V LP64D ABI compliance (including sret in a0 and indirect jalr calls) */
static void riscv_emit_call_asm(ZeroCCCompiler *c, const char *func_name, int arg_count, int *arg_temps, int *is_fp, int *stack_pos, FILE *out) {
    static const char *int_arg_regs[] = {"a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7"};
    static const char *fp_arg_regs[] = {"fa0", "fa1", "fa2", "fa3", "fa4", "fa5", "fa6", "fa7"};

    int int_arg_idx = 0;
    int fp_arg_idx = 0;
    int indirect_temp = -1;
    int int_src_regs[8];
    int int_arg_temps[IR_CALL_MAX_ARGS];
    int total_int_count = 0;

    for (int i = 0; i < arg_count; i++) {
        int temp = arg_temps[i];
        int phys_reg = regalloc_get_reg_for_temp(&c->regalloc, temp);
        const char *reg_name = (phys_reg >= 0 && phys_reg < BE(c)->regs->num_fp_regs)
                                   ? BE(c)->regs->fp_regs[phys_reg]
                                   : BE(c)->get_vec_reg_name(phys_reg);
        int arg_is_fp = is_fp ? is_fp[i] : 0;

        if (arg_is_fp == 2) {
            indirect_temp = temp;
        } else if (arg_is_fp == 1 && (stack_pos ? stack_pos[i] : 255) >= 255 && fp_arg_idx < 8) {
            fprintf(out, "\tfmv.d\t%s, %s\n", fp_arg_regs[fp_arg_idx], reg_name);
            fp_arg_idx++;
        } else if (arg_is_fp == 0) {
            int_arg_temps[total_int_count++] = temp;
            if (int_arg_idx < 8) {
                int_src_regs[int_arg_idx++] = phys_reg;
            }
        }
    }

    /* Save indirect callee into t1 before clobbering argument registers */
    int32_t spill_base_offset = c->regalloc.spill_base_offset;
    if (!func_name && indirect_temp >= 0) {
        int sp = regalloc_get_spill_slot_for_temp(&c->regalloc, indirect_temp);
        if (sp >= 0) {
            int32_t off = spill_base_offset + (sp + 1) * 8;
            BE(c)->emit_load_asm(c, 6 /* t1 */, zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -off), out);
        } else {
            int r_callee = regalloc_get_reg_for_temp(&c->regalloc, indirect_temp);
            fprintf(out, "\tmv\tt1, %s\n", BE(c)->get_hw_reg_name(r_callee));
        }
    }

    /* Stack args (from stack_pos): integer args beyond a0..a7 AND FP args
     * beyond fa0..fa7, in original argument order. Allocate 16-byte aligned,
     * slot k at k(sp). FP slots use the 64-bit fsd store (float args read
     * back as their low 32 bits by the callee's flw). */
    int n_stack = 0;
    int stack_pos_list[IR_CALL_MAX_ARGS];
    for (int i = 0; i < arg_count && i < IR_CALL_MAX_ARGS; i++) {
        int arg_pos = stack_pos ? stack_pos[i] : 255;
        if (arg_pos < 255 && arg_pos < IR_CALL_MAX_ARGS) {
            stack_pos_list[arg_pos] = i;
            if (arg_pos + 1 > n_stack) n_stack = arg_pos + 1;
        }
    }
    int legacy_n_stack = (!stack_pos && total_int_count > 8) ? total_int_count - 8 : 0;
    int stack_alloc = 0;
    int ptr_slot = (c->isa == ISA_RISCV32) ? 4 : 8;
    const char *st_ptr = (c->isa == ISA_RISCV32) ? "sw" : "sd";
    const char *ld_ptr = (c->isa == ISA_RISCV32) ? "lw" : "ld";
    if (stack_pos) {
        stack_alloc = ((n_stack * 8) + 15) & ~15;
        if (n_stack > 0) {
            fprintf(out, "\taddi\tsp, sp, -%d\n", stack_alloc);
            for (int pos = 0; pos < n_stack; pos++) {
                int i = stack_pos_list[pos];
                int arg_is_fp = is_fp ? is_fp[i] : 0;
                if (arg_is_fp == 1) {
                    int phys_reg = regalloc_get_reg_for_temp(&c->regalloc, arg_temps[i]);
                    const char *vname = (phys_reg >= 0 && phys_reg < BE(c)->regs->num_fp_regs)
                                             ? BE(c)->regs->fp_regs[phys_reg]
                                             : BE(c)->get_vec_reg_name(phys_reg);
                    fprintf(out, "\tfsd\t%s, %d(sp)\n", vname, pos * 8);
                } else {
                    int sp = regalloc_get_spill_slot_for_temp(&c->regalloc, arg_temps[i]);
                    if (sp >= 0) {
                        int32_t off = spill_base_offset + (sp + 1) * 8;
                        BE(c)->emit_load_asm(c, 5 /* t0 */, zero_cc_frame_base_reg(c), zero_cc_frame_disp_at(c, -off, stack_alloc), out);
                        fprintf(out, "\t%s\tt0, %d(sp)\n", st_ptr, pos * ptr_slot);
                    } else {
                        int phys_reg = regalloc_get_reg_for_temp(&c->regalloc, arg_temps[i]);
                        fprintf(out, "\t%s\t%s, %d(sp)\n", st_ptr, BE(c)->get_hw_reg_name(phys_reg), pos * ptr_slot);
                    }
                }
            }
        }
    } else {
        /* Legacy: int args beyond 8, in order (old behaviour) */
        n_stack = legacy_n_stack;
        stack_alloc = ((n_stack * ptr_slot) + 15) & ~15;
        if (n_stack > 0) {
            fprintf(out, "\taddi\tsp, sp, -%d\n", stack_alloc);
            for (int a = 8; a < total_int_count; a++) {
                int sp = regalloc_get_spill_slot_for_temp(&c->regalloc, int_arg_temps[a]);
                if (sp >= 0) {
                    int32_t off = spill_base_offset + (sp + 1) * 8;
                    BE(c)->emit_load_asm(c, 5 /* t0 */, zero_cc_frame_base_reg(c), zero_cc_frame_disp_at(c, -off, stack_alloc), out);
                    fprintf(out, "\t%s\tt0, %d(sp)\n", st_ptr, (a - 8) * ptr_slot);
                } else {
                    int phys_reg = regalloc_get_reg_for_temp(&c->regalloc, int_arg_temps[a]);
                    fprintf(out, "\t%s\t%s, %d(sp)\n", st_ptr, BE(c)->get_hw_reg_name(phys_reg), (a - 8) * ptr_slot);
                }
            }
        }
    }

    /* Parallel move of integer arguments via stack so a0..a7 don't clobber each other */
    if (int_arg_idx > 0) {
        int alloc_bytes = ((int_arg_idx * ptr_slot) + 15) & ~15;
        fprintf(out, "\taddi\tsp, sp, -%d\n", alloc_bytes);
        for (int a = 0; a < int_arg_idx; a++) {
            int sp = regalloc_get_spill_slot_for_temp(&c->regalloc, int_arg_temps[a]);
            if (sp >= 0) {
                int32_t off = spill_base_offset + (sp + 1) * 8;
                BE(c)->emit_load_asm(c, 5 /* t0 */, zero_cc_frame_base_reg(c), zero_cc_frame_disp_at(c, -off, stack_alloc + alloc_bytes), out);
                fprintf(out, "\t%s\tt0, %d(sp)\n", st_ptr, a * ptr_slot);
            } else {
                fprintf(out, "\t%s\t%s, %d(sp)\n", st_ptr, BE(c)->get_hw_reg_name(int_src_regs[a]), a * ptr_slot);
            }
        }
        for (int a = 0; a < int_arg_idx; a++) {
            fprintf(out, "\t%s\t%s, %d(sp)\n", ld_ptr, int_arg_regs[a], a * ptr_slot);
        }
        fprintf(out, "\taddi\tsp, sp, %d\n", alloc_bytes);
    }

    if (!func_name && indirect_temp >= 0) {
        fprintf(out, "\tjalr\tra, 0(t1)\n");
    } else if (func_name) {
        fprintf(out, "\tcall\t%s\n", func_name);
    }

    /* Clean up stack arguments after call */
    if (n_stack > 0) {
        fprintf(out, "\taddi\tsp, sp, %d\n", stack_alloc);
    }
}

/* RET: ret (jalr zero, ra) */
static void riscv_emit_ret_asm(ZeroCCCompiler *, FILE *out) {
    fprintf(out, "\tret\n");
}

/* OS syscall (GAS text form). Same ABI and clobber logic as
 * riscv_emit_syscall in riscv.c: number in a7, args 1-6 in a0-a5, result
 * in a0 (= VREG_0). ecall clobbers the caller-saved set, so the
 * allocatable clobbered registers that may hold live values (t2, a1-a7,
 * t3-t6) are saved in a 16-byte-aligned stack region before `ecall` and
 * restored after. Store/load width follows the target (sd/ld vs sw/lw).
 * Spill reloads use t1 (scratch_reg2), the same idiom as call_asm. */
static void riscv_emit_syscall_asm(ZeroCCCompiler *c, int arg_count, const int *arg_temps, FILE *out) {
    static const int linux_syscall_regs[7] = {17, 10, 11, 12, 13, 14, 15}; /* a7, a0-a5 */
    static const int clobbered_live_regs[] = {7, 11, 12, 13, 14, 15, 16, 17, 28, 29, 30, 31};
    static const int n_save = 12;
    int slot = (BE(c)->isa == ISA_RISCV32) ? 4 : 8;
    const char *st = (BE(c)->isa == ISA_RISCV32) ? "sw" : "sd";
    const char *ld = (BE(c)->isa == ISA_RISCV32) ? "lw" : "ld";
    int32_t spill_base_offset = c->regalloc.spill_base_offset;
    int n = (arg_count < 7) ? arg_count : 7;

    /* Save the live values the kernel would clobber. */
    fprintf(out, "\taddi\tsp, sp, -%d\n", n_save * slot);
    for (int r = 0; r < n_save; r++) {
        fprintf(out, "\t%s\t%s, %d(sp)\n", st,
                BE(c)->get_hw_reg_name(clobbered_live_regs[r]), r * slot);
    }
    int32_t extra_sp = n_save * slot;

    /* Parallel move of the syscall arguments. */
    if (n > 0) {
        int alloc_bytes = ((n * slot) + 15) & ~15;
        fprintf(out, "\taddi\tsp, sp, -%d\n", alloc_bytes);
        for (int a = 0; a < n; a++) {
            int temp = arg_temps[a];
            int sp = regalloc_get_spill_slot_for_temp(&c->regalloc, temp);
            if (sp >= 0) {
                int32_t off = spill_base_offset + (sp + 1) * 8;
                BE(c)->emit_load_asm(c, 6 /* t1 */, zero_cc_frame_base_reg(c),
                                     zero_cc_frame_disp_at(c, -off, extra_sp + alloc_bytes), out);
                fprintf(out, "\t%s\tt1, %d(sp)\n", st, a * slot);
            } else {
                fprintf(out, "\t%s\t%s, %d(sp)\n", st,
                        BE(c)->get_hw_reg_name(regalloc_get_reg_for_temp(&c->regalloc, temp)), a * slot);
            }
        }
        for (int a = n - 1; a >= 0; a--) {
            /* Read back into the syscall register (LIFO). */
            fprintf(out, "\t%s\t%s, %d(sp)\n", ld,
                    BE(c)->get_hw_reg_name(linux_syscall_regs[a]), a * slot);
        }
        fprintf(out, "\taddi\tsp, sp, %d\n", alloc_bytes);
    }

    fprintf(out, "\tecall\n");

    /* Restore the saved registers (reverse order) and free the region. */
    for (int r = n_save - 1; r >= 0; r--) {
        fprintf(out, "\t%s\t%s, %d(sp)\n", ld,
                BE(c)->get_hw_reg_name(clobbered_live_regs[r]), r * slot);
    }
    fprintf(out, "\taddi\tsp, sp, %d\n", n_save * slot);
}

/* OS clone (SYS_clone = 220). Arguments: flags, child_sp, ptid, tls, ctid */
static void riscv_emit_clone_asm(ZeroCCCompiler *c, int arg_count, const int *arg_temps, FILE *out) {
    static const int clone_regs[5] = {10, 11, 12, 13, 14}; /* a0-a4 */
    int is_rv32 = (BE(c)->isa == ISA_RISCV32);
    int slot = is_rv32 ? 4 : 8;
    const char *st = is_rv32 ? "sw" : "sd";
    const char *ld = is_rv32 ? "lw" : "ld";
    int32_t spill_base_offset = c->regalloc.spill_base_offset;
    int n = (arg_count < 5) ? arg_count : 5;
    if (n > 0) {
        int alloc_bytes = ((n * slot) + 15) & ~15;
        fprintf(out, "\taddi\tsp, sp, -%d\n", alloc_bytes);
        for (int a = 0; a < n; a++) {
            int temp = arg_temps[a];
            int sp = regalloc_get_spill_slot_for_temp(&c->regalloc, temp);
            if (sp >= 0) {
                int32_t off = spill_base_offset + (sp + 1) * 8;
                BE(c)->emit_load_asm(c, 6 /* t1 */, zero_cc_frame_base_reg(c),
                                     zero_cc_frame_disp_at(c, -off, alloc_bytes), out);
                fprintf(out, "\t%s\tt1, %d(sp)\n", st, a * slot);
            } else {
                fprintf(out, "\t%s\t%s, %d(sp)\n", st,
                        BE(c)->get_hw_reg_name(regalloc_get_reg_for_temp(&c->regalloc, temp)), a * slot);
            }
        }
        for (int a = n - 1; a >= 0; a--) {
            fprintf(out, "\t%s\t%s, %d(sp)\n", ld,
                    BE(c)->get_hw_reg_name(clone_regs[a]), a * slot);
        }
        fprintf(out, "\taddi\tsp, sp, %d\n", alloc_bytes);
    }
    fprintf(out, "\tli\ta7, 220\n");
    fprintf(out, "\tecall\n");
    fprintf(out, "\tbnez\ta0, 1f\n");
    fprintf(out, "\tmv\ts0, zero\n");
    if (is_rv32) {
        fprintf(out, "\tlw\ta1, 0(sp)\n");
        fprintf(out, "\tlw\ta0, 4(sp)\n");
    } else {
        fprintf(out, "\tld\ta1, 0(sp)\n");
        fprintf(out, "\tld\ta0, 8(sp)\n");
    }
    fprintf(out, "\taddi\tsp, sp, 16\n");
    fprintf(out, "\tjalr\ta1\n");
    fprintf(out, "\tli\ta7, 93\n");
    fprintf(out, "\tecall\n");
    fprintf(out, "\tebreak\n");
    fprintf(out, "1:\n");
}

/* LABEL: label: */
static void riscv_emit_label_asm(ZeroCCCompiler *, const char *label, FILE *out) {
    fprintf(out, "%s:\n", label);
}

/* JMP: j label */
static void riscv_emit_jmp_asm(ZeroCCCompiler *, const char *label, FILE *out) {
    fprintf(out, "\tj\t%s\n", label);
}

/* JEQ: beq t0, zero, label */
static void riscv_emit_jeq_asm(ZeroCCCompiler *, const char *label, FILE *out) {
    fprintf(out, "\tbeq\tt0, zero, %s\n", label);
}

/* JNE: bne t0, zero, label */
static void riscv_emit_jne_asm(ZeroCCCompiler *, const char *label, FILE *out) {
    fprintf(out, "\tbne\tt0, zero, %s\n", label);
}

/* JLT: blt t0, zero, label */
static void riscv_emit_jlt_asm(ZeroCCCompiler *, const char *label, FILE *out) {
    fprintf(out, "\tblt\tt0, zero, %s\n", label);
}

/* JLE: ble t0, zero, label */
static void riscv_emit_jle_asm(ZeroCCCompiler *, const char *label, FILE *out) {
    fprintf(out, "\tble\tt0, zero, %s\n", label);
}

/* JGT: bgt t0, zero, label */
static void riscv_emit_jgt_asm(ZeroCCCompiler *, const char *label, FILE *out) {
    fprintf(out, "\tbgt\tt0, zero, %s\n", label);
}

/* JGE: bge t0, zero, label */
static void riscv_emit_jge_asm(ZeroCCCompiler *, const char *label, FILE *out) {
    fprintf(out, "\tbge\tt0, zero, %s\n", label);
}

/* FP LOAD: fld fa, offset(base) */
static void riscv_emit_load_fp_asm(ZeroCCCompiler *c, int dst, int base, int32_t offset, FILE *out) {
    const char *dst_name = BE(c)->regs->fp_regs[dst];
    const char *base_name = BE(c)->get_hw_reg_name(base);
    if (offset >= -2048 && offset <= 2047) {
        fprintf(out, "\tfld\t%s, %d(%s)\n", dst_name, offset, base_name);
    } else {
        fprintf(out, "\tli\tt5, %d\n", (int)offset);
        fprintf(out, "\tadd\tt5, %s, t5\n", base_name);
        fprintf(out, "\tfld\t%s, 0(t5)\n", dst_name);
    }
}

/* FP STORE: fsd fa, offset(base) */
static void riscv_emit_store_fp_asm(ZeroCCCompiler *c, int src, int base, int32_t offset, FILE *out) {
    const char *src_name = BE(c)->regs->fp_regs[src];
    const char *base_name = BE(c)->get_hw_reg_name(base);
    if (offset >= -2048 && offset <= 2047) {
        fprintf(out, "\tfsd\t%s, %d(%s)\n", src_name, offset, base_name);
    } else {
        fprintf(out, "\tli\tt5, %d\n", (int)offset);
        fprintf(out, "\tadd\tt5, %s, t5\n", base_name);
        fprintf(out, "\tfsd\t%s, 0(t5)\n", src_name);
    }
}

/* FP ADD: fadd.s/d dst, src1, src2 */
static void riscv_emit_fp_add_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->regs->fp_regs[dst];
    const char *src1_name = BE(c)->regs->fp_regs[src1];
    const char *src2_name = BE(c)->regs->fp_regs[src2];
    fprintf(out, "\tfadd.d\t%s, %s, %s\n", dst_name, src1_name, src2_name);
}

/* FP SUB: fsub.s/d dst, src1, src2 */
static void riscv_emit_fp_sub_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->regs->fp_regs[dst];
    const char *src1_name = BE(c)->regs->fp_regs[src1];
    const char *src2_name = BE(c)->regs->fp_regs[src2];
    fprintf(out, "\tfsub.d\t%s, %s, %s\n", dst_name, src1_name, src2_name);
}

/* FP MUL: fmul.s/d dst, src1, src2 */
static void riscv_emit_fp_mul_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->regs->fp_regs[dst];
    const char *src1_name = BE(c)->regs->fp_regs[src1];
    const char *src2_name = BE(c)->regs->fp_regs[src2];
    fprintf(out, "\tfmul.d\t%s, %s, %s\n", dst_name, src1_name, src2_name);
}

/* FP DIV: fdiv.s/d dst, src1, src2 */
static void riscv_emit_fp_div_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->regs->fp_regs[dst];
    const char *src1_name = BE(c)->regs->fp_regs[src1];
    const char *src2_name = BE(c)->regs->fp_regs[src2];
    fprintf(out, "\tfdiv.d\t%s, %s, %s\n", dst_name, src1_name, src2_name);
}

/* FP MOV: fmv.s/d dst, src */
static void riscv_emit_fp_mov_asm(ZeroCCCompiler *c, int dst, int src, FILE *out) {
    const char *dst_name = BE(c)->regs->fp_regs[dst];
    const char *src_name = BE(c)->regs->fp_regs[src];
    fprintf(out, "\tfmv.d\t%s, %s\n", dst_name, src_name);
}

/* FP CMP: three-way sign into t0 (-1 if src1 < src2, +1 if src1 > src2, 0 if eq) */
static void riscv_emit_fp_cmp_asm(ZeroCCCompiler *c, int src1, int src2, FILE *out) {
    const char *s1 = (src1 >= 0 && src1 < 32) ? BE(c)->regs->fp_regs[src1] : "fa0";
    const char *s2 = (src2 >= 0 && src2 < 32) ? BE(c)->regs->fp_regs[src2] : "fa1";
    fprintf(out, "\tflt.d\tt1, %s, %s\n", s1, s2);
    fprintf(out, "\tflt.d\tt0, %s, %s\n", s2, s1);
    fprintf(out, "\tsub\tt0, t0, t1\n");
}

/* VARIADIC_SPILL: spill incoming a0..a7 and fa0..fa7 into the 16-slot save area */
static void riscv_emit_variadic_spill_asm(ZeroCCCompiler *c, int param_count, int32_t base_offset, FILE *out) {
    (void)param_count;
    if (c->isa == ISA_RISCV32) return;
    int base = zero_cc_frame_base_reg(c);
    int gp_regs = BE(c)->conv->int_arg_reg_count;
    int fp_regs = BE(c)->conv->fp_arg_reg_count;
    for (int i = 0; i < gp_regs; i++) {
        int r = BE(c)->conv->int_arg_regs[i];
        riscv_emit_store_asm(c, r, base, zero_cc_frame_disp(c, -(base_offset + (int32_t)i * 8)), out);
    }
    for (int i = 0; i < fp_regs; i++) {
        int r = BE(c)->conv->fp_arg_regs[i];
        riscv_emit_store_fp_asm(c, r, base, zero_cc_frame_disp(c, -(base_offset + (int32_t)(gp_regs + i) * 8)), out);
    }
}

/* FP CVTSF: fcvt.d.w / fcvt.d.l fd, rs (int -> double) */
static void riscv_emit_fp_cvtsf_asm(ZeroCCCompiler *c, int fp_dst, int int_src, FILE *out) {
    const char *dst_name = (fp_dst >= 0 && fp_dst < 32) ? BE(c)->regs->fp_regs[fp_dst] : "fa0";
    const char *src_name = BE(c)->get_hw_reg_name(int_src);
    fprintf(out, "\tfcvt.d.w\t%s, %s\n", dst_name, src_name);
}

/* FP CVTSI: fcvt.w.d rd, fs, rtz (double -> int, truncate) */
static void riscv_emit_fp_cvtsi_asm(ZeroCCCompiler *c, int int_dst, int fp_src, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(int_dst);
    const char *src_name = (fp_src >= 0 && fp_src < 32) ? BE(c)->regs->fp_regs[fp_src] : "fa0";
    fprintf(out, "\tfcvt.w.d\t%s, %s, rtz\n", dst_name, src_name);
}

/* VLA_ALLOC: sub dst, sp, size */
static void riscv_emit_vla_alloc_asm(ZeroCCCompiler *c, int dst, int size, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *size_name = BE(c)->get_hw_reg_name(size);
    fprintf(out, "\tsub\t%s, sp, %s\n", dst_name, size_name);
}

/* =========================================================================
 * Vector Instruction Emission (RVV)
 * ========================================================================= */

/* Vector load - vle32.v */
static void riscv_emit_vload_asm(ZeroCCCompiler *c, int dst, int base, int32_t offset, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *base_name = BE(c)->get_hw_reg_name(base);
    fprintf(out, "\tvsetivli\tzero, 4, e32, m1, ta, ma\n");
    fprintf(out, "\tvle32.v\t%s, %d(%s)\n", dst_name, offset, base_name);
}

/* Vector store - vse32.v */
static void riscv_emit_vstore_asm(ZeroCCCompiler *c, int src, int base, int32_t offset, FILE *out) {
    const char *src_name = BE(c)->get_vec_reg_name(src);
    const char *base_name = BE(c)->get_hw_reg_name(base);
    fprintf(out, "\tvsetivli\tzero, 4, e32, m1, ta, ma\n");
    fprintf(out, "\tvse32.v\t%s, %d(%s)\n", src_name, offset, base_name);
}

/* Vector scalar single add - fadd.s */
static void riscv_emit_vaddss_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = (dst >= 0 && dst < 32) ? BE(c)->regs->fp_regs[dst] : "fa0";
    const char *src1_name = (src1 >= 0 && src1 < 32) ? BE(c)->regs->fp_regs[src1] : "fa0";
    const char *src2_name = (src2 >= 0 && src2 < 32) ? BE(c)->regs->fp_regs[src2] : "fa1";
    fprintf(out, "\tfadd.s\t%s, %s, %s\n", dst_name, src1_name, src2_name);
}

/* Vector scalar single sub - fsub.s */
static void riscv_emit_vsubss_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = (dst >= 0 && dst < 32) ? BE(c)->regs->fp_regs[dst] : "fa0";
    const char *src1_name = (src1 >= 0 && src1 < 32) ? BE(c)->regs->fp_regs[src1] : "fa0";
    const char *src2_name = (src2 >= 0 && src2 < 32) ? BE(c)->regs->fp_regs[src2] : "fa1";
    fprintf(out, "\tfsub.s\t%s, %s, %s\n", dst_name, src1_name, src2_name);
}

/* Vector scalar single mul - fmul.s */
static void riscv_emit_vmulss_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = (dst >= 0 && dst < 32) ? BE(c)->regs->fp_regs[dst] : "fa0";
    const char *src1_name = (src1 >= 0 && src1 < 32) ? BE(c)->regs->fp_regs[src1] : "fa0";
    const char *src2_name = (src2 >= 0 && src2 < 32) ? BE(c)->regs->fp_regs[src2] : "fa1";
    fprintf(out, "\tfmul.s\t%s, %s, %s\n", dst_name, src1_name, src2_name);
}

/* Vector scalar single div - fdiv.s */
static void riscv_emit_vdivss_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = (dst >= 0 && dst < 32) ? BE(c)->regs->fp_regs[dst] : "fa0";
    const char *src1_name = (src1 >= 0 && src1 < 32) ? BE(c)->regs->fp_regs[src1] : "fa0";
    const char *src2_name = (src2 >= 0 && src2 < 32) ? BE(c)->regs->fp_regs[src2] : "fa1";
    fprintf(out, "\tfdiv.s\t%s, %s, %s\n", dst_name, src1_name, src2_name);
}

/* Vector packed add - vfadd.vv */
static void riscv_emit_vaddps_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src1_name = BE(c)->get_vec_reg_name(src1);
    const char *src2_name = BE(c)->get_vec_reg_name(src2);
    fprintf(out, "\tvsetivli\tzero, 4, e32, m1, ta, ma\n");
    fprintf(out, "\tvfadd.vv\t%s, %s, %s\n", dst_name, src1_name, src2_name);
}

/* Vector packed sub - vfsub.vv */
static void riscv_emit_vsubps_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src1_name = BE(c)->get_vec_reg_name(src1);
    const char *src2_name = BE(c)->get_vec_reg_name(src2);
    fprintf(out, "\tvsetivli\tzero, 4, e32, m1, ta, ma\n");
    fprintf(out, "\tvfsub.vv\t%s, %s, %s\n", dst_name, src1_name, src2_name);
}

/* Vector packed mul - vfmul.vv */
static void riscv_emit_vmulps_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src1_name = BE(c)->get_vec_reg_name(src1);
    const char *src2_name = BE(c)->get_vec_reg_name(src2);
    fprintf(out, "\tvsetivli\tzero, 4, e32, m1, ta, ma\n");
    fprintf(out, "\tvfmul.vv\t%s, %s, %s\n", dst_name, src1_name, src2_name);
}

/* Vector packed div - vfdiv.vv */
static void riscv_emit_vdivps_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src1_name = BE(c)->get_vec_reg_name(src1);
    const char *src2_name = BE(c)->get_vec_reg_name(src2);
    fprintf(out, "\tvsetivli\tzero, 4, e32, m1, ta, ma\n");
    fprintf(out, "\tvfdiv.vv\t%s, %s, %s\n", dst_name, src1_name, src2_name);
}

/* Vector scalar double add - fadd.d */
static void riscv_emit_vaddsd_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = (dst >= 0 && dst < 32) ? BE(c)->regs->fp_regs[dst] : "fa0";
    const char *src1_name = (src1 >= 0 && src1 < 32) ? BE(c)->regs->fp_regs[src1] : "fa0";
    const char *src2_name = (src2 >= 0 && src2 < 32) ? BE(c)->regs->fp_regs[src2] : "fa1";
    fprintf(out, "\tfadd.d\t%s, %s, %s\n", dst_name, src1_name, src2_name);
}

/* Vector scalar double sub - fsub.d */
static void riscv_emit_vsubsd_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = (dst >= 0 && dst < 32) ? BE(c)->regs->fp_regs[dst] : "fa0";
    const char *src1_name = (src1 >= 0 && src1 < 32) ? BE(c)->regs->fp_regs[src1] : "fa0";
    const char *src2_name = (src2 >= 0 && src2 < 32) ? BE(c)->regs->fp_regs[src2] : "fa1";
    fprintf(out, "\tfsub.d\t%s, %s, %s\n", dst_name, src1_name, src2_name);
}

/* Vector scalar double mul - fmul.d */
static void riscv_emit_vmulsd_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = (dst >= 0 && dst < 32) ? BE(c)->regs->fp_regs[dst] : "fa0";
    const char *src1_name = (src1 >= 0 && src1 < 32) ? BE(c)->regs->fp_regs[src1] : "fa0";
    const char *src2_name = (src2 >= 0 && src2 < 32) ? BE(c)->regs->fp_regs[src2] : "fa1";
    fprintf(out, "\tfmul.d\t%s, %s, %s\n", dst_name, src1_name, src2_name);
}

/* Vector scalar double div - fdiv.d */
static void riscv_emit_vdivsd_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = (dst >= 0 && dst < 32) ? BE(c)->regs->fp_regs[dst] : "fa0";
    const char *src1_name = (src1 >= 0 && src1 < 32) ? BE(c)->regs->fp_regs[src1] : "fa0";
    const char *src2_name = (src2 >= 0 && src2 < 32) ? BE(c)->regs->fp_regs[src2] : "fa1";
    fprintf(out, "\tfdiv.d\t%s, %s, %s\n", dst_name, src1_name, src2_name);
}

/* Vector packed double add - vfadd.vv (double) */
static void riscv_emit_vaddpd_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src1_name = BE(c)->get_vec_reg_name(src1);
    const char *src2_name = BE(c)->get_vec_reg_name(src2);
    fprintf(out, "\tvsetivli\tzero, 2, e64, m1, ta, ma\n");
    fprintf(out, "\tvfadd.vv\t%s, %s, %s\n", dst_name, src1_name, src2_name);
}

/* Vector packed double sub - vfsub.vv (double) */
static void riscv_emit_vsubpd_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src1_name = BE(c)->get_vec_reg_name(src1);
    const char *src2_name = BE(c)->get_vec_reg_name(src2);
    fprintf(out, "\tvsetivli\tzero, 2, e64, m1, ta, ma\n");
    fprintf(out, "\tvfsub.vv\t%s, %s, %s\n", dst_name, src1_name, src2_name);
}

/* Vector packed double mul - vfmul.vv (double) */
static void riscv_emit_vmulpd_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src1_name = BE(c)->get_vec_reg_name(src1);
    const char *src2_name = BE(c)->get_vec_reg_name(src2);
    fprintf(out, "\tvsetivli\tzero, 2, e64, m1, ta, ma\n");
    fprintf(out, "\tvfmul.vv\t%s, %s, %s\n", dst_name, src1_name, src2_name);
}

/* Vector packed double div - vfdiv.vv (double) */
static void riscv_emit_vdivpd_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src1_name = BE(c)->get_vec_reg_name(src1);
    const char *src2_name = BE(c)->get_vec_reg_name(src2);
    fprintf(out, "\tvsetivli\tzero, 2, e64, m1, ta, ma\n");
    fprintf(out, "\tvfdiv.vv\t%s, %s, %s\n", dst_name, src1_name, src2_name);
}

/* Vector packed 32-bit int add - vadd.vv */
static void riscv_emit_vpaddd_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src1_name = BE(c)->get_vec_reg_name(src1);
    const char *src2_name = BE(c)->get_vec_reg_name(src2);
    fprintf(out, "\tvsetivli\tzero, 4, e32, m1, ta, ma\n");
    fprintf(out, "\tvadd.vv\t%s, %s, %s\n", dst_name, src1_name, src2_name);
}

/* Vector packed 32-bit int sub - vsub.vv */
static void riscv_emit_vpsubd_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src1_name = BE(c)->get_vec_reg_name(src1);
    const char *src2_name = BE(c)->get_vec_reg_name(src2);
    fprintf(out, "\tvsetivli\tzero, 4, e32, m1, ta, ma\n");
    fprintf(out, "\tvsub.vv\t%s, %s, %s\n", dst_name, src1_name, src2_name);
}

/* Vector packed 32-bit int mul - vmul.vv */
static void riscv_emit_vpmulld_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src1_name = BE(c)->get_vec_reg_name(src1);
    const char *src2_name = BE(c)->get_vec_reg_name(src2);
    fprintf(out, "\tvsetivli\tzero, 4, e32, m1, ta, ma\n");
    fprintf(out, "\tvmul.vv\t%s, %s, %s\n", dst_name, src1_name, src2_name);
}

/* Vector move - fmv.s */
static void riscv_emit_vmovss_asm(ZeroCCCompiler *c, int dst, int src, FILE *out) {
    const char *dst_name = (dst >= 0 && dst < 32) ? BE(c)->regs->fp_regs[dst] : "fa0";
    const char *src_name = (src >= 0 && src < 32) ? BE(c)->regs->fp_regs[src] : "fa0";
    fprintf(out, "\tfmv.s\t%s, %s\n", dst_name, src_name);
}

/* Vector scalar double move - fmv.d */
static void riscv_emit_vmovsd_asm(ZeroCCCompiler *c, int dst, int src, FILE *out) {
    const char *dst_name = (dst >= 0 && dst < 32) ? BE(c)->regs->fp_regs[dst] : "fa0";
    const char *src_name = (src >= 0 && src < 32) ? BE(c)->regs->fp_regs[src] : "fa0";
    fprintf(out, "\tfmv.d\t%s, %s\n", dst_name, src_name);
}

/* Vector move packed - vmv.v.v */
static void riscv_emit_vmovaps_asm(ZeroCCCompiler *c, int dst, int src, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src_name = BE(c)->get_vec_reg_name(src);
    fprintf(out, "\tvsetivli\tzero, 4, e32, m1, ta, ma\n");
    fprintf(out, "\tvmv.v.v\t%s, %s\n", dst_name, src_name);
}

/* Vector move packed double - vmv.v.v */
static void riscv_emit_vmovapd_asm(ZeroCCCompiler *c, int dst, int src, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src_name = BE(c)->get_vec_reg_name(src);
    fprintf(out, "\tvsetivli\tzero, 2, e64, m1, ta, ma\n");
    fprintf(out, "\tvmv.v.v\t%s, %s\n", dst_name, src_name);
}

/* Vector bitwise and - vand.vv */
static void riscv_emit_vandps_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src1_name = BE(c)->get_vec_reg_name(src1);
    const char *src2_name = BE(c)->get_vec_reg_name(src2);
    fprintf(out, "\tvsetivli\tzero, 4, e32, m1, ta, ma\n");
    fprintf(out, "\tvand.vv\t%s, %s, %s\n", dst_name, src1_name, src2_name);
}

/* Vector bitwise or - vor.vv */
static void riscv_emit_vorps_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src1_name = BE(c)->get_vec_reg_name(src1);
    const char *src2_name = BE(c)->get_vec_reg_name(src2);
    fprintf(out, "\tvsetivli\tzero, 4, e32, m1, ta, ma\n");
    fprintf(out, "\tvor.vv\t%s, %s, %s\n", dst_name, src1_name, src2_name);
}

/* Vector bitwise xor - vxor.vv */
static void riscv_emit_vxorps_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src1_name = BE(c)->get_vec_reg_name(src1);
    const char *src2_name = BE(c)->get_vec_reg_name(src2);
    fprintf(out, "\tvsetivli\tzero, 4, e32, m1, ta, ma\n");
    fprintf(out, "\tvxor.vv\t%s, %s, %s\n", dst_name, src1_name, src2_name);
}

/* Vector shuffle - Not directly supported in RVV, use vmv.v.v */
static void riscv_emit_vshufps_asm(ZeroCCCompiler *c, int dst, int, int src2, int imm, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src2_name = BE(c)->get_vec_reg_name(src2);
    fprintf(out, "\t# RVV shuffle (imm=%d): %s <- %s\n", imm, dst_name, src2_name);
    fprintf(out, "\tvsetivli\tzero, 4, e32, m1, ta, ma\n");
    fprintf(out, "\tvmv.v.v\t%s, %s\n", dst_name, src2_name);
}

/* Convert scalar int to scalar single - fcvt.s.w */
static void riscv_emit_vcvt_si2ss_asm(ZeroCCCompiler *c, int dst, int src, FILE *out) {
    const char *dst_name = (dst >= 0 && dst < 32) ? BE(c)->regs->fp_regs[dst] : "fa0";
    const char *src_name = BE(c)->get_hw_reg_name(src);
    fprintf(out, "\tfcvt.s.w\t%s, %s\n", dst_name, src_name);
}

/* Convert scalar single to scalar int - fcvt.w.s */
static void riscv_emit_vcvt_ss2si_asm(ZeroCCCompiler *c, int dst, int src, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *src_name = (src >= 0 && src < 32) ? BE(c)->regs->fp_regs[src] : "fa0";
    fprintf(out, "\tfcvt.w.s\t%s, %s\n", dst_name, src_name);
}

/* Convert scalar single to scalar double - fcvt.d.s */
static void riscv_emit_cvtss2sd_asm(ZeroCCCompiler *c, int dst, int src, FILE *out) {
    const char *dst_name = (dst >= 0 && dst < 32) ? BE(c)->regs->fp_regs[dst] : "fa0";
    const char *src_name = (src >= 0 && src < 32) ? BE(c)->regs->fp_regs[src] : "fa1";
    fprintf(out, "\tfcvt.d.s\t%s, %s\n", dst_name, src_name);
}

/* Convert scalar double to scalar single - fcvt.s.d */
static void riscv_emit_cvtsd2ss_asm(ZeroCCCompiler *c, int dst, int src, FILE *out) {
    const char *dst_name = (dst >= 0 && dst < 32) ? BE(c)->regs->fp_regs[dst] : "fa0";
    const char *src_name = (src >= 0 && src < 32) ? BE(c)->regs->fp_regs[src] : "fa1";
    fprintf(out, "\tfcvt.s.d\t%s, %s\n", dst_name, src_name);
}

/* Vector compare - feq.s */
static void riscv_emit_vcmpss_asm(ZeroCCCompiler *c, int, int src1, int src2, int, FILE *out) {
    const char *src1_name = (src1 >= 0 && src1 < 32) ? BE(c)->regs->fp_regs[src1] : "fa0";
    const char *src2_name = (src2 >= 0 && src2 < 32) ? BE(c)->regs->fp_regs[src2] : "fa1";
    fprintf(out, "\tfeq.s\tt0, %s, %s\n", src1_name, src2_name);
}

/* Vector scalar double comparison - feq.d */
static void riscv_emit_vcmpsd_asm(ZeroCCCompiler *c, int, int src1, int src2, int, FILE *out) {
    const char *src1_name = (src1 >= 0 && src1 < 32) ? BE(c)->regs->fp_regs[src1] : "fa0";
    const char *src2_name = (src2 >= 0 && src2 < 32) ? BE(c)->regs->fp_regs[src2] : "fa1";
    fprintf(out, "\tfeq.d\tt0, %s, %s\n", src1_name, src2_name);
}

/* =========================================================================
 * GNU __int128 (Sprint 109): 128-bit ops, RISC-V text forms. RISC-V has no
 * carry flag, so carry/borrow are computed with sltu into t0 (reg 5), mirroring
 * the raw emitters in riscv.c. mul128 also uses t0 as its working scratch.
 * ========================================================================= */
static void riscv_emit_add128_asm(ZeroCCCompiler *c, int dlo, int dhi, int alo, int ahi, int blo, int bhi, FILE *out) {
    const char *dlo_n = BE(c)->get_hw_reg_name(dlo), *dhi_n = BE(c)->get_hw_reg_name(dhi);
    const char *alo_n = BE(c)->get_hw_reg_name(alo), *ahi_n = BE(c)->get_hw_reg_name(ahi);
    const char *blo_n = BE(c)->get_hw_reg_name(blo), *bhi_n = BE(c)->get_hw_reg_name(bhi);
    const char *t0_n = BE(c)->get_hw_reg_name(5);
    if (dlo == alo) {
        fprintf(out, "\tmv\t%s, %s\n", t0_n, alo_n);
        fprintf(out, "\tadd\t%s, %s, %s\n", dlo_n, alo_n, blo_n);
        fprintf(out, "\tsltu\t%s, %s, %s\n", t0_n, dlo_n, t0_n); /* carry: sum < addend */
    } else {
        fprintf(out, "\tadd\t%s, %s, %s\n", dlo_n, alo_n, blo_n);
        fprintf(out, "\tsltu\t%s, %s, %s\n", t0_n, dlo_n, alo_n); /* carry: sum < addend */
    }
    fprintf(out, "\tadd\t%s, %s, %s\n", dhi_n, ahi_n, bhi_n);
    fprintf(out, "\tadd\t%s, %s, %s\n", dhi_n, dhi_n, t0_n);
}
static void riscv_emit_sub128_asm(ZeroCCCompiler *c, int dlo, int dhi, int alo, int ahi, int blo, int bhi, FILE *out) {
    const char *dlo_n = BE(c)->get_hw_reg_name(dlo), *dhi_n = BE(c)->get_hw_reg_name(dhi);
    const char *alo_n = BE(c)->get_hw_reg_name(alo), *ahi_n = BE(c)->get_hw_reg_name(ahi);
    const char *blo_n = BE(c)->get_hw_reg_name(blo), *bhi_n = BE(c)->get_hw_reg_name(bhi);
    const char *t0_n = BE(c)->get_hw_reg_name(5);
    fprintf(out, "\tsltu\t%s, %s, %s\n", t0_n, alo_n, blo_n); /* borrow: minuend < subtrahend (must precede sub in case dlo == alo) */
    fprintf(out, "\tsub\t%s, %s, %s\n", dlo_n, alo_n, blo_n);
    fprintf(out, "\tsub\t%s, %s, %s\n", dhi_n, ahi_n, bhi_n);
    fprintf(out, "\tsub\t%s, %s, %s\n", dhi_n, dhi_n, t0_n);
}
static void riscv_emit_mul128_asm(ZeroCCCompiler *c, int dlo, int dhi, int alo, int ahi, int blo, int bhi, FILE *out) {
    const char *dlo_n = BE(c)->get_hw_reg_name(dlo), *dhi_n = BE(c)->get_hw_reg_name(dhi);
    const char *alo_n = BE(c)->get_hw_reg_name(alo), *ahi_n = BE(c)->get_hw_reg_name(ahi);
    const char *blo_n = BE(c)->get_hw_reg_name(blo), *bhi_n = BE(c)->get_hw_reg_name(bhi);
    const char *t0_n = BE(c)->get_hw_reg_name(5);
    fprintf(out, "\tmul\t%s, %s, %s\n", dlo_n, alo_n, blo_n);
    fprintf(out, "\tmulhu\t%s, %s, %s\n", dhi_n, alo_n, blo_n);
    fprintf(out, "\tmul\t%s, %s, %s\n", t0_n, alo_n, bhi_n);
    fprintf(out, "\tadd\t%s, %s, %s\n", dhi_n, dhi_n, t0_n);
    fprintf(out, "\tmul\t%s, %s, %s\n", t0_n, ahi_n, blo_n);
    fprintf(out, "\tadd\t%s, %s, %s\n", dhi_n, dhi_n, t0_n);
}
static void riscv_emit_mul128_spilled_asm(ZeroCCCompiler *c,
                                          int32_t d_lo, int32_t d_hi,
                                          int32_t a_lo, int32_t a_hi,
                                          int32_t b_lo, int32_t b_hi, FILE *out) {
    /* Spilled MUL128 (Sprint 109): all operands in frame slots. Uses only the
     * reserved 128-bit scratch registers (never hold a live value): pair0
     * (plo,phi) and pair1.lo (b1). */
    int plo = BE(c)->g128_scratch_lo[0], phi = BE(c)->g128_scratch_hi[0];
    int b1 = BE(c)->g128_scratch_lo[1];
    int base = zero_cc_frame_base_reg(c);
    const char *plo_n = BE(c)->get_hw_reg_name(plo), *phi_n = BE(c)->get_hw_reg_name(phi);
    const char *b1_n = BE(c)->get_hw_reg_name(b1);
    /* P0 = a_lo * b_lo */
    riscv_emit_load_asm(c, plo, base, a_lo, out);
    riscv_emit_load_asm(c, b1, base, b_lo, out);
    fprintf(out, "\tmulhu\t%s, %s, %s\n", phi_n, plo_n, b1_n);
    fprintf(out, "\tmul\t%s, %s, %s\n", b1_n, plo_n, b1_n);
    riscv_emit_store_asm(c, b1, base, d_lo, out);
    riscv_emit_store_asm(c, phi, base, d_hi, out);
    /* P1 = a_lo * b_hi; dhi += P1.lo */
    riscv_emit_load_asm(c, b1, base, b_hi, out);
    fprintf(out, "\tmul\t%s, %s, %s\n", phi_n, plo_n, b1_n);
    riscv_emit_load_asm(c, b1, base, d_hi, out);
    fprintf(out, "\tadd\t%s, %s, %s\n", b1_n, b1_n, phi_n);
    riscv_emit_store_asm(c, b1, base, d_hi, out);
    /* P2 = a_hi * b_lo; dhi += P2.lo */
    riscv_emit_load_asm(c, plo, base, a_hi, out);
    riscv_emit_load_asm(c, b1, base, b_lo, out);
    fprintf(out, "\tmul\t%s, %s, %s\n", phi_n, plo_n, b1_n);
    riscv_emit_load_asm(c, b1, base, d_hi, out);
    fprintf(out, "\tadd\t%s, %s, %s\n", b1_n, b1_n, phi_n);
    riscv_emit_store_asm(c, b1, base, d_hi, out);
}
static void riscv_emit_and128_asm(ZeroCCCompiler *c, int dlo, int dhi, int alo, int ahi, int blo, int bhi, FILE *out) {
    const char *dlo_n = BE(c)->get_hw_reg_name(dlo), *dhi_n = BE(c)->get_hw_reg_name(dhi);
    const char *alo_n = BE(c)->get_hw_reg_name(alo), *ahi_n = BE(c)->get_hw_reg_name(ahi);
    const char *blo_n = BE(c)->get_hw_reg_name(blo), *bhi_n = BE(c)->get_hw_reg_name(bhi);
    fprintf(out, "\tand\t%s, %s, %s\n", dlo_n, alo_n, blo_n);
    fprintf(out, "\tand\t%s, %s, %s\n", dhi_n, ahi_n, bhi_n);
}
static void riscv_emit_or128_asm(ZeroCCCompiler *c, int dlo, int dhi, int alo, int ahi, int blo, int bhi, FILE *out) {
    const char *dlo_n = BE(c)->get_hw_reg_name(dlo), *dhi_n = BE(c)->get_hw_reg_name(dhi);
    const char *alo_n = BE(c)->get_hw_reg_name(alo), *ahi_n = BE(c)->get_hw_reg_name(ahi);
    const char *blo_n = BE(c)->get_hw_reg_name(blo), *bhi_n = BE(c)->get_hw_reg_name(bhi);
    fprintf(out, "\tor\t%s, %s, %s\n", dlo_n, alo_n, blo_n);
    fprintf(out, "\tor\t%s, %s, %s\n", dhi_n, ahi_n, bhi_n);
}
static void riscv_emit_xor128_asm(ZeroCCCompiler *c, int dlo, int dhi, int alo, int ahi, int blo, int bhi, FILE *out) {
    const char *dlo_n = BE(c)->get_hw_reg_name(dlo), *dhi_n = BE(c)->get_hw_reg_name(dhi);
    const char *alo_n = BE(c)->get_hw_reg_name(alo), *ahi_n = BE(c)->get_hw_reg_name(ahi);
    const char *blo_n = BE(c)->get_hw_reg_name(blo), *bhi_n = BE(c)->get_hw_reg_name(bhi);
    fprintf(out, "\txor\t%s, %s, %s\n", dlo_n, alo_n, blo_n);
    fprintf(out, "\txor\t%s, %s, %s\n", dhi_n, ahi_n, bhi_n);
}
static void riscv_emit_not128_asm(ZeroCCCompiler *c, int dlo, int dhi, int slo, int shi, FILE *out) {
    const char *dlo_n = BE(c)->get_hw_reg_name(dlo), *dhi_n = BE(c)->get_hw_reg_name(dhi);
    const char *slo_n = BE(c)->get_hw_reg_name(slo), *shi_n = BE(c)->get_hw_reg_name(shi);
    if (dlo != slo) fprintf(out, "\tmv\t%s, %s\n", dlo_n, slo_n);
    fprintf(out, "\txori\t%s, %s, -1\n", dlo_n, dlo_n);
    if (dhi != shi) fprintf(out, "\tmv\t%s, %s\n", dhi_n, shi_n);
    fprintf(out, "\txori\t%s, %s, -1\n", dhi_n, dhi_n);
}
static void riscv_emit_neg128_asm(ZeroCCCompiler *c, int dlo, int dhi, int slo, int shi, FILE *out) {
    const char *dlo_n = BE(c)->get_hw_reg_name(dlo), *dhi_n = BE(c)->get_hw_reg_name(dhi);
    const char *slo_n = BE(c)->get_hw_reg_name(slo), *shi_n = BE(c)->get_hw_reg_name(shi);
    const char *t0_n = BE(c)->get_hw_reg_name(5);
    /* -x = 0 - x. borrow of 0-slo = (slo != 0), computed with sltu into t0 before slo clobber. */
    fprintf(out, "\tsltu\t%s, zero, %s\n", t0_n, slo_n);
    fprintf(out, "\tsub\t%s, zero, %s\n", dlo_n, slo_n);
    fprintf(out, "\tsub\t%s, zero, %s\n", dhi_n, shi_n);
    fprintf(out, "\tsub\t%s, %s, %s\n", dhi_n, dhi_n, t0_n);
}
static void riscv_emit_mov128_asm(ZeroCCCompiler *c, int dlo, int dhi, int slo, int shi, FILE *out) {
    const char *dlo_n = BE(c)->get_hw_reg_name(dlo), *dhi_n = BE(c)->get_hw_reg_name(dhi);
    const char *slo_n = BE(c)->get_hw_reg_name(slo), *shi_n = BE(c)->get_hw_reg_name(shi);
    if (dlo != slo) fprintf(out, "\tmv\t%s, %s\n", dlo_n, slo_n);
    if (dhi != shi) fprintf(out, "\tmv\t%s, %s\n", dhi_n, shi_n);
}
static void riscv_emit_mov128_imm_asm(ZeroCCCompiler *c, int dlo, int dhi, int64_t imm, FILE *out) {
    const char *dlo_n = BE(c)->get_hw_reg_name(dlo), *dhi_n = BE(c)->get_hw_reg_name(dhi);
    fprintf(out, "\tli\t%s, %ld\n", dlo_n, (long)imm);
    fprintf(out, "\tli\t%s, 0\n", dhi_n);
}
static void riscv_emit_load128_asm(ZeroCCCompiler *c, int dlo, int dhi, int base, int32_t offset, FILE *out) {
    /* Aliasing: if dlo == base, load the hi half first (see riscv64_emit_load128). */
    if (BE(c)->emit_load_asm) {
        if (dlo == base) {
            BE(c)->emit_load_asm(c, dhi, base, offset + 8, out);
            BE(c)->emit_load_asm(c, dlo, base, offset, out);
        } else {
            BE(c)->emit_load_asm(c, dlo, base, offset, out);
            BE(c)->emit_load_asm(c, dhi, base, offset + 8, out);
        }
    }
}
static void riscv_emit_store128_asm(ZeroCCCompiler *c, int slo, int shi, int base, int32_t offset, FILE *out) {
    if (BE(c)->emit_store_asm) {
        BE(c)->emit_store_asm(c, slo, base, offset, out);
        BE(c)->emit_store_asm(c, shi, base, offset + 8, out);
    }
}
static void riscv_emit_zext64_128_asm(ZeroCCCompiler *c, int dlo, int dhi, int src, FILE *out) {
    const char *dlo_n = BE(c)->get_hw_reg_name(dlo), *dhi_n = BE(c)->get_hw_reg_name(dhi);
    const char *src_n = BE(c)->get_hw_reg_name(src);
    fprintf(out, "\tmv\t%s, %s\n", dlo_n, src_n);
    fprintf(out, "\tli\t%s, 0\n", dhi_n);
}
static void riscv_emit_sext64_128_asm(ZeroCCCompiler *c, int dlo, int dhi, int src, FILE *out) {
    const char *dlo_n = BE(c)->get_hw_reg_name(dlo), *dhi_n = BE(c)->get_hw_reg_name(dhi);
    const char *src_n = BE(c)->get_hw_reg_name(src);
    fprintf(out, "\tmv\t%s, %s\n", dlo_n, src_n);
    fprintf(out, "\tsrai\t%s, %s, 63\n", dhi_n, src_n);
}
static void riscv_emit_trunc128_64_asm(ZeroCCCompiler *c, int dst, int slo, FILE *out) {
    if (dst != slo)
        fprintf(out, "\tmv\t%s, %s\n", BE(c)->get_hw_reg_name(dst), BE(c)->get_hw_reg_name(slo));
}

static void riscv_emit_inline_asm_asm(ZeroCCCompiler *c, size_t idx, FILE *out) {
    const Instr *instr = ir_get(c, idx);
    if (!instr) return;
    const IRAsmArgs *aa = ir_asm_args_find(c, idx);
    if (!aa || !aa->template_str) return;

    int first_out = -1;
    for (int o = 0; o < aa->op_count; o++) {
        if (aa->ops[o].kind == ZC_ASM_OP_OUT_R || aa->ops[o].kind == ZC_ASM_OP_INOUT_R ||
            aa->ops[o].kind == ZC_ASM_OP_OUT_M) {
            first_out = o;
            break;
        }
    }
    for (int o = 0; o < aa->op_count; o++) {
        const IRAsmOperand *op = &aa->ops[o];
        if (op->digit < 0 || first_out < 0) continue;
        int out_pos = first_out + op->digit;
        if (out_pos < 0 || out_pos >= aa->op_count) continue;
        const IRAsmOperand *oop = &aa->ops[out_pos];
        int out_phys = regalloc_get_reg_for_temp(&c->regalloc, oop->temp);
        int in_phys = regalloc_get_reg_for_temp(&c->regalloc, op->temp);
        const char *in_n = riscv_get_hw_reg_name(in_phys);
        const char *out_n = riscv_get_hw_reg_name(out_phys);
        if (strcmp(in_n, out_n) != 0) {
            fprintf(out, "\tmv\t%s, %s\n", out_n, in_n);
        }
    }

    fprintf(out, "#APP\n\t/* GNU asm */\n\t");
    const char *p = aa->template_str;
    while (*p) {
        if (*p == '%' && p[1]) {
            p++;
            if (*p == '%') {
                fputc('%', out);
                p++;
                continue;
            }
            if (*p == 'l') {
                if (p[1] == '[') {
                    p += 2;
                    char lname[64];
                    int llen = 0;
                    while (*p && *p != ']' && llen < 63) lname[llen++] = *p++;
                    lname[llen] = '\0';
                    if (*p == ']') p++;
                    int target_lbl = -1;
                    for (int li = 0; li < aa->goto_label_count; li++) {
                        if (strcmp(aa->goto_label_names[li], lname) == 0) {
                            target_lbl = (int)aa->goto_labels[li];
                            break;
                        }
                    }
                    if (target_lbl >= 0) {
                        fprintf(out, ".L%zu_%u", (size_t)c->lowering.current_func_idx, (unsigned)target_lbl);
                    } else {
                        fprintf(out, "%%bad_label");
                    }
                    continue;
                } else if (p[1] >= '0' && p[1] <= '9') {
                    int n = p[1] - '0';
                    p += 2;
                    int target_lbl = -1;
                    if (n >= aa->op_count && (n - aa->op_count) < aa->goto_label_count) {
                        target_lbl = (int)aa->goto_labels[n - aa->op_count];
                    } else if (n < aa->goto_label_count) {
                        target_lbl = (int)aa->goto_labels[n];
                    }
                    if (target_lbl >= 0) {
                        fprintf(out, ".L%zu_%u", (size_t)c->lowering.current_func_idx, (unsigned)target_lbl);
                    } else {
                        fprintf(out, "%%bad_label");
                    }
                    continue;
                }
            }

            int op_idx = -1;
            if (*p >= '0' && *p <= '9') {
                op_idx = *p - '0';
                p++;
            }

            if (op_idx >= 0 && op_idx < aa->op_count) {
                const IRAsmOperand *op = &aa->ops[op_idx];
                int target_op = (op->digit >= 0 && first_out >= 0) ? (first_out + op->digit) : op_idx;
                const IRAsmOperand *top = &aa->ops[target_op];
                if (top->kind == ZC_ASM_OP_OUT_M || top->kind == ZC_ASM_OP_IN_M) {
                    int phys = regalloc_get_reg_for_temp(&c->regalloc, top->temp);
                    fprintf(out, "0(%s)", riscv_get_hw_reg_name(phys));
                } else {
                    int phys = regalloc_get_reg_for_temp(&c->regalloc, top->temp);
                    fprintf(out, "%s", riscv_get_hw_reg_name(phys));
                }
            } else {
                fputc('%', out);
            }
            continue;
        }
        if (*p == '\n') {
            fprintf(out, "\n\t");
            p++;
            continue;
        }
        fputc(*p, out);
        p++;
    }
    fprintf(out, "\n#NO_APP\n");
}

/* Initialize RISC-V assembly emission functions in backend */
void riscv64_init_asm_emission(ZeroCCISABackend *be) {
    be->emit_inline_asm_asm = riscv_emit_inline_asm_asm;
    be->emit_prologue_asm = riscv_emit_prologue_asm;
    be->emit_epilogue_asm = riscv_emit_epilogue_asm;
    be->get_hw_reg_name = riscv_get_hw_reg_name;
    be->get_vec_reg_name = riscv_get_vec_reg_name;

    be->emit_mov_imm_asm = riscv_emit_mov_imm_asm;
    be->emit_mov_reg_asm = riscv_emit_mov_reg_asm;
    be->emit_load_asm = riscv_emit_load_asm;
    be->emit_store_asm = riscv_emit_store_asm;
    be->emit_add_asm = riscv_emit_add_asm;
    be->emit_sub_asm = riscv_emit_sub_asm;
    be->emit_mul_asm = riscv_emit_mul_asm;
    be->emit_div_asm = riscv_emit_div_asm;
    be->emit_mod_asm = riscv_emit_mod_asm;
    be->emit_and_asm = riscv_emit_and_asm;
    be->emit_or_asm = riscv_emit_or_asm;
    be->emit_xor_asm = riscv_emit_xor_asm;
    be->emit_shl_asm = riscv_emit_shl_asm;
    be->emit_shr_asm = riscv_emit_shr_asm;
    be->emit_sar_asm = riscv_emit_sar_asm;
    be->emit_extend_asm = riscv_emit_extend_asm;
    be->emit_cmp_asm = riscv_emit_cmp_asm;
    be->emit_cond_set_asm = riscv_emit_cond_set_asm;
    be->emit_neg_asm = riscv_emit_neg_asm;
    be->emit_add_imm_asm = riscv_emit_add_imm_asm;
    be->emit_sub_imm_asm = riscv_emit_sub_imm_asm;
    be->emit_inc_asm = riscv_emit_inc_asm;
    be->emit_dec_asm = riscv_emit_dec_asm;
    be->emit_not_asm = riscv_emit_not_asm;
    be->emit_add128_asm = riscv_emit_add128_asm;
    be->emit_sub128_asm = riscv_emit_sub128_asm;
    be->emit_mul128_asm = riscv_emit_mul128_asm;
    be->emit_mul128_spilled_asm = riscv_emit_mul128_spilled_asm;
    be->emit_and128_asm = riscv_emit_and128_asm;
    be->emit_or128_asm = riscv_emit_or128_asm;
    be->emit_xor128_asm = riscv_emit_xor128_asm;
    be->emit_not128_asm = riscv_emit_not128_asm;
    be->emit_neg128_asm = riscv_emit_neg128_asm;
    be->emit_mov128_asm = riscv_emit_mov128_asm;
    be->emit_mov128_imm_asm = riscv_emit_mov128_imm_asm;
    be->emit_load128_asm = riscv_emit_load128_asm;
    be->emit_store128_asm = riscv_emit_store128_asm;
    be->emit_zext64_128_asm = riscv_emit_zext64_128_asm;
    be->emit_sext64_128_asm = riscv_emit_sext64_128_asm;
    be->emit_trunc128_64_asm = riscv_emit_trunc128_64_asm;
    be->emit_lea_asm = riscv_emit_lea_asm;
    be->emit_deref_asm = riscv_emit_deref_asm;
    be->emit_call_asm = riscv_emit_call_asm;
    be->emit_ret_asm = riscv_emit_ret_asm;
    be->emit_syscall_asm = riscv_emit_syscall_asm;
    be->emit_clone_asm = riscv_emit_clone_asm;
    be->emit_label_asm = riscv_emit_label_asm;
    be->emit_jmp_asm = riscv_emit_jmp_asm;
    be->emit_jeq_asm = riscv_emit_jeq_asm;
    be->emit_jne_asm = riscv_emit_jne_asm;
    be->emit_jlt_asm = riscv_emit_jlt_asm;
    be->emit_jle_asm = riscv_emit_jle_asm;
    be->emit_jgt_asm = riscv_emit_jgt_asm;
    be->emit_jge_asm = riscv_emit_jge_asm;
    be->emit_load_fp_asm = riscv_emit_load_fp_asm;
    be->emit_store_fp_asm = riscv_emit_store_fp_asm;
    be->emit_fp_add_asm = riscv_emit_fp_add_asm;
    be->emit_fp_sub_asm = riscv_emit_fp_sub_asm;
    be->emit_fp_mul_asm = riscv_emit_fp_mul_asm;
    be->emit_fp_div_asm = riscv_emit_fp_div_asm;
    be->emit_fp_cmp_asm = riscv_emit_fp_cmp_asm;
    be->emit_fp_mov_asm = riscv_emit_fp_mov_asm;
    be->emit_fp_cvtsf_asm = riscv_emit_fp_cvtsf_asm;
    be->emit_fp_cvtsi_asm = riscv_emit_fp_cvtsi_asm;
    be->emit_vla_alloc_asm = riscv_emit_vla_alloc_asm;
    be->emit_param_save_int_asm = riscv_emit_param_save_int_asm;
    be->emit_param_save_fp_asm = riscv_emit_param_save_fp_asm;
    be->emit_variadic_spill_asm = riscv_emit_variadic_spill_asm;

    /* Vector instruction emission (RVV) */
    be->emit_vload_asm = riscv_emit_vload_asm;
    be->emit_vstore_asm = riscv_emit_vstore_asm;
    be->emit_vaddss_asm = riscv_emit_vaddss_asm;
    be->emit_vsubss_asm = riscv_emit_vsubss_asm;
    be->emit_vmulss_asm = riscv_emit_vmulss_asm;
    be->emit_vdivss_asm = riscv_emit_vdivss_asm;
    be->emit_vaddps_asm = riscv_emit_vaddps_asm;
    be->emit_vsubps_asm = riscv_emit_vsubps_asm;
    be->emit_vmulps_asm = riscv_emit_vmulps_asm;
    be->emit_vdivps_asm = riscv_emit_vdivps_asm;
    be->emit_vaddsd_asm = riscv_emit_vaddsd_asm;
    be->emit_vsubsd_asm = riscv_emit_vsubsd_asm;
    be->emit_vmulsd_asm = riscv_emit_vmulsd_asm;
    be->emit_vdivsd_asm = riscv_emit_vdivsd_asm;
    be->emit_vaddpd_asm = riscv_emit_vaddpd_asm;
    be->emit_vsubpd_asm = riscv_emit_vsubpd_asm;
    be->emit_vmulpd_asm = riscv_emit_vmulpd_asm;
    be->emit_vdivpd_asm = riscv_emit_vdivpd_asm;
    be->emit_vpaddd_asm = riscv_emit_vpaddd_asm;
    be->emit_vpsubd_asm = riscv_emit_vpsubd_asm;
    be->emit_vpmulld_asm = riscv_emit_vpmulld_asm;
    be->emit_vmovss_asm = riscv_emit_vmovss_asm;
    be->emit_vmovsd_asm = riscv_emit_vmovsd_asm;
    be->emit_vmovaps_asm = riscv_emit_vmovaps_asm;
    be->emit_vmovapd_asm = riscv_emit_vmovapd_asm;
    be->emit_vandps_asm = riscv_emit_vandps_asm;
    be->emit_vorps_asm = riscv_emit_vorps_asm;
    be->emit_vxorps_asm = riscv_emit_vxorps_asm;
    be->emit_vshufps_asm = riscv_emit_vshufps_asm;
    be->emit_vcvt_si2ss_asm = riscv_emit_vcvt_si2ss_asm;
    be->emit_vcvt_ss2si_asm = riscv_emit_vcvt_ss2si_asm;
    be->emit_cvtss2sd_asm = riscv_emit_cvtss2sd_asm;
    be->emit_cvtsd2ss_asm = riscv_emit_cvtsd2ss_asm;
    be->emit_vcmpss_asm = riscv_emit_vcmpss_asm;
    be->emit_vcmpsd_asm = riscv_emit_vcmpsd_asm;
}
