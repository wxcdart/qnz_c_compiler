/*
 * AArch64 Assembly Emitter
 * ISA-specific assembly emission for GNU Assembler (GAS) syntax
 */

#include "zero_cc/isa/isa_backend.h"
#include "zero_cc/ir/ir.h"
#include "zero_cc/ir/regalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Hardware register names for AArch64 (indexed by hardware number) */
static const char *aarch64_hw_reg_names[] = {
    "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7",
    "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15",
    "x16", "x17", "x18", "x19", "x20", "x21", "x22", "x23",
    "x24", "x25", "x26", "x27", "x28", "x29", "x30", "sp"
};

/* Get register name by hardware number */
static const char *aarch64_get_hw_reg_name(int phys_reg) {
    if (phys_reg >= 0 && phys_reg < 32) {
        return aarch64_hw_reg_names[phys_reg];
    }
    return "x0";  /* Default */
}

/* Get vector register name by index */
static const char *aarch64_get_vec_reg_name(int vec_reg_idx) {
    static const char *const aarch64_vec_names[] = {
        "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7",
        "v8", "v9", "v10", "v11", "v12", "v13", "v14", "v15",
        "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23",
        "v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31"
    };
    if (vec_reg_idx < 0 || vec_reg_idx >= 32) {
        return "v0";  /* Default to v0 for invalid */
    }
    return aarch64_vec_names[vec_reg_idx];
}

/* Prologue: stp x29, x30, [sp, #-16]!, mov x29, sp, sub sp, sp, #frame_size, save callee-saved */
static void aarch64_emit_prologue_asm(ZeroCCCompiler *c, size_t stack_size) {
    FILE *out = c->asm_out ? c->asm_out : stdout;

    /* Frame setup with x29 at top of local frame so negative offsets [x29, #-off] lie above sp */
    fprintf(out, "\tstp\tx29, x30, [sp, #-16]!\n");
    fprintf(out, "\tmov\tx29, sp\n");
    size_t aligned_sz = (stack_size + 15) & ~((size_t)15);
    if (aligned_sz > 0) {
        fprintf(out, "\tsub\tsp, sp, #%zu\n", aligned_sz);
    }

    /* Save callee-saved registers x19..x28 at [x29, #-offset] */
    const int *callee_saved = BE(c)->conv->callee_saved_gpr;
    int count = BE(c)->conv->callee_saved_gpr_count;

    for (int i = 0; i < count; i++) {
        int reg = callee_saved[i];
        if (reg == 29 || reg == 30) continue; /* x29, x30 already saved by stp */
        const char *reg_name = BE(c)->get_hw_reg_name(reg);
        int offset = BE(c)->conv->callee_save_slot_offsets[i];
        fprintf(out, "\tstr\t%s, [x29, #-%d]\n", reg_name, offset);
    }
}

/* Epilogue: restore callee-saved, mov sp, x29, ldp x29, x30, [sp], #16, ret */
static void aarch64_emit_epilogue_asm(ZeroCCCompiler *c) {
    FILE *out = c->asm_out ? c->asm_out : stdout;

    /* Restore callee-saved registers */
    const int *callee_saved = BE(c)->conv->callee_saved_gpr;
    int count = BE(c)->conv->callee_saved_gpr_count;

    for (int i = 0; i < count; i++) {
        int reg = callee_saved[i];
        if (reg == 29 || reg == 30) continue;
        const char *reg_name = BE(c)->get_hw_reg_name(reg);
        int offset = BE(c)->conv->callee_save_slot_offsets[i];
        fprintf(out, "\tldr\t%s, [x29, #-%d]\n", reg_name, offset);
    }

    fprintf(out, "\tmov\tsp, x29\n");
    fprintf(out, "\tldp\tx29, x30, [sp], #16\n");
    fprintf(out, "\tret\n");
}

/* MOV_IMM: mov dst, #imm */
static void aarch64_emit_mov_imm_asm(ZeroCCCompiler *c, int dst, int64_t imm, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    /* A single `mov` only accepts a 12-bit value (optionally rotated by an even
     * number of bits). For an arbitrary 64-bit constant we must build it from
     * 16-bit chunks with MOVZ (bit 0) + MOVK (higher chunks). (Sprint 41) */
    uint64_t v = (uint64_t)imm;
    uint32_t w0 = (uint32_t)(v & 0xFFFF);
    uint32_t w1 = (uint32_t)((v >> 16) & 0xFFFF);
    uint32_t w2 = (uint32_t)((v >> 32) & 0xFFFF);
    uint32_t w3 = (uint32_t)((v >> 48) & 0xFFFF);
    fprintf(out, "\tmovz\t%s, #%u\n", dst_name, w0);
    if (w1 || w2 || w3) fprintf(out, "\tmovk\t%s, #%u, lsl #16\n", dst_name, w1);
    if (w2 || w3)       fprintf(out, "\tmovk\t%s, #%u, lsl #32\n", dst_name, w2);
    if (w3)             fprintf(out, "\tmovk\t%s, #%u, lsl #48\n", dst_name, w3);
}

/* MOV_REG: mov dst, src */
static void aarch64_emit_mov_reg_asm(ZeroCCCompiler *c, int dst, int src, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *src_name = BE(c)->get_hw_reg_name(src);
    fprintf(out, "\tmov\t%s, %s\n", dst_name, src_name);
}

/* LOAD: ldr dst, [base, #offset] */
static void aarch64_emit_load_asm(ZeroCCCompiler *c, int dst, int base, int32_t offset, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *base_name = BE(c)->get_hw_reg_name(base);
    if (offset < -256 || (offset > 255 && (offset % 8 != 0 || offset > 32760))) {
        if (offset < 0) {
            fprintf(out, "\tsub\tx9, %s, #%d\n", base_name, -offset);
        } else {
            fprintf(out, "\tadd\tx9, %s, #%d\n", base_name, offset);
        }
        fprintf(out, "\tldr\t%s, [x9]\n", dst_name);
    } else {
        fprintf(out, "\tldr\t%s, [%s, #%d]\n", dst_name, base_name, offset);
    }
}

/* STORE: str src, [base, #offset] */
static void aarch64_emit_store_asm(ZeroCCCompiler *c, int src, int base, int32_t offset, FILE *out) {
    const char *src_name = BE(c)->get_hw_reg_name(src);
    const char *base_name = BE(c)->get_hw_reg_name(base);
    if (offset < -256 || (offset > 255 && (offset % 8 != 0 || offset > 32760))) {
        if (offset < 0) {
            fprintf(out, "\tsub\tx9, %s, #%d\n", base_name, -offset);
        } else {
            fprintf(out, "\tadd\tx9, %s, #%d\n", base_name, offset);
        }
        fprintf(out, "\tstr\t%s, [x9]\n", src_name);
    } else {
        fprintf(out, "\tstr\t%s, [%s, #%d]\n", src_name, base_name, offset);
    }
}

/* ADD: add dst, src1, src2 */
static void aarch64_emit_add_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *src1_name = BE(c)->get_hw_reg_name(src1);
    const char *src2_name = BE(c)->get_hw_reg_name(src2);
    fprintf(out, "\tadd\t%s, %s, %s\n", dst_name, src1_name, src2_name);
}

/* SUB: sub dst, src1, src2 */
static void aarch64_emit_sub_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *src1_name = BE(c)->get_hw_reg_name(src1);
    const char *src2_name = BE(c)->get_hw_reg_name(src2);
    fprintf(out, "\tsub\t%s, %s, %s\n", dst_name, src1_name, src2_name);
}

/* MUL: mul dst, src1, src2 */
static void aarch64_emit_mul_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *src1_name = BE(c)->get_hw_reg_name(src1);
    const char *src2_name = BE(c)->get_hw_reg_name(src2);
    fprintf(out, "\tmul\t%s, %s, %s\n", dst_name, src1_name, src2_name);
}

/* DIV: sdiv dst, src1, src2 */
static void aarch64_emit_div_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *src1_name = BE(c)->get_hw_reg_name(src1);
    const char *src2_name = BE(c)->get_hw_reg_name(src2);
    fprintf(out, "\tsdiv\t%s, %s, %s\n", dst_name, src1_name, src2_name);
}

/* MOD: dst = src1 % src2 (signed)
 * tmp = src1 / src2 ;  dst = src1 - tmp * src2
 * Uses x16 (IP0), a caller-saved intra-procedural scratch register that the
 * register allocator does not assign to variables (mirrors aarch64.c emit_mod). */
static void aarch64_emit_mod_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *src1_name = BE(c)->get_hw_reg_name(src1);
    const char *src2_name = BE(c)->get_hw_reg_name(src2);
    fprintf(out, "\tsdiv\tx16, %s, %s\n", src1_name, src2_name);
    fprintf(out, "\tmsub\t%s, x16, %s, %s\n", dst_name, src2_name, src1_name);
}

/* AND: and dst, src1, src2 */
static void aarch64_emit_and_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *src1_name = BE(c)->get_hw_reg_name(src1);
    const char *src2_name = BE(c)->get_hw_reg_name(src2);
    fprintf(out, "\tand\t%s, %s, %s\n", dst_name, src1_name, src2_name);
}

/* OR: orr dst, src1, src2 */
static void aarch64_emit_or_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *src1_name = BE(c)->get_hw_reg_name(src1);
    const char *src2_name = BE(c)->get_hw_reg_name(src2);
    fprintf(out, "\torr\t%s, %s, %s\n", dst_name, src1_name, src2_name);
}

/* XOR: eor dst, src1, src2 */
static void aarch64_emit_xor_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *src1_name = BE(c)->get_hw_reg_name(src1);
    const char *src2_name = BE(c)->get_hw_reg_name(src2);
    fprintf(out, "\teor\t%s, %s, %s\n", dst_name, src1_name, src2_name);
}

/* LSL: lsl dst, src, shift */
static void aarch64_emit_shl_asm(ZeroCCCompiler *c, int dst, int src, int shift, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *src_name = BE(c)->get_hw_reg_name(src);
    const char *shift_name = BE(c)->get_hw_reg_name(shift);
    fprintf(out, "\tlsl\t%s, %s, %s\n", dst_name, src_name, shift_name);
}

/* LSR: lsr dst, src, shift */
static void aarch64_emit_shr_asm(ZeroCCCompiler *c, int dst, int src, int shift, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *src_name = BE(c)->get_hw_reg_name(src);
    const char *shift_name = BE(c)->get_hw_reg_name(shift);
    fprintf(out, "\tlsr\t%s, %s, %s\n", dst_name, src_name, shift_name);
}

/* ASR: asr dst, src, shift */
static void aarch64_emit_sar_asm(ZeroCCCompiler *c, int dst, int src, int shift, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *src_name = BE(c)->get_hw_reg_name(src);
    const char *shift_name = BE(c)->get_hw_reg_name(shift);
    fprintf(out, "\tasr\t%s, %s, %s\n", dst_name, src_name, shift_name);
}

/* SEXT/ZEXT: sxtb/sxth/sxtw, uxtb/uxth/uxtw.
 * SXTB/SXTH/UXTB/UXTH use 32-bit (w) registers for both operands.
 * SXTW/UXTW extend a 32-bit value into a 64-bit register:
 * the destination is an X register and the SOURCE is a W register
 * (AArch64: SXTW Xd, Wn / UXTW Xd, Wn) - using an X register for the
 * source is an assembler error. */
static void aarch64_emit_extend_asm(ZeroCCCompiler *c, int dst, int src, int width, int sign, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *src_name = BE(c)->get_hw_reg_name(src);
    const char *mnem;
    if (width == 1) mnem = sign ? "sxtb" : "uxtb";
    else if (width == 2) mnem = sign ? "sxth" : "uxth";
    else mnem = sign ? "sxtw" : "uxtw";
    if (width <= 2) {
        fprintf(out, "\t%s\tw%s, w%s\n", mnem, dst_name + 1, src_name + 1);
    } else {
        fprintf(out, "\t%s\tx%s, w%s\n", mnem, dst_name + 1, src_name + 1);
    }
}

/* CMP: cmp src1, src2 */
static void aarch64_emit_cmp_asm(ZeroCCCompiler *c, int src1, int src2, FILE *out) {
    const char *src1_name = BE(c)->get_hw_reg_name(src1);
    const char *src2_name = BE(c)->get_hw_reg_name(src2);
    fprintf(out, "\tcmp\t%s, %s\n", src1_name, src2_name);
}

/* COND_SET: cset dst, cond */
static void aarch64_emit_cond_set_asm(ZeroCCCompiler *c, int dst, int cond, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *cc[] = {"lt", "le", "gt", "ge", "eq", "ne"};
    fprintf(out, "\tcset\t%s, %s\n", dst_name, cc[cond]);
}

/* NEG: neg dst, src */
static void aarch64_emit_neg_asm(ZeroCCCompiler *c, int dst, int src, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *src_name = BE(c)->get_hw_reg_name(src);
    fprintf(out, "\tneg\t%s, %s\n", dst_name, src_name);
}

/* ADD_IMM: add dst, #imm (Sprint 40: was missing from -S) */
static void aarch64_emit_add_imm_asm(ZeroCCCompiler *c, int dst, int64_t imm, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    fprintf(out, "\tadd\t%s, %s, #%ld\n", dst_name, dst_name, (long)imm);
}

/* SUB_IMM: sub dst, #imm */
static void aarch64_emit_sub_imm_asm(ZeroCCCompiler *c, int dst, int64_t imm, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    fprintf(out, "\tsub\t%s, %s, #%ld\n", dst_name, dst_name, (long)imm);
}

/* INC: add dst, #1 */
static void aarch64_emit_inc_asm(ZeroCCCompiler *c, int dst, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    fprintf(out, "\tadd\t%s, %s, #1\n", dst_name, dst_name);
}

/* DEC: sub dst, #1 */
static void aarch64_emit_dec_asm(ZeroCCCompiler *c, int dst, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    fprintf(out, "\tsub\t%s, %s, #1\n", dst_name, dst_name);
}

/* NOT: mvn dst, src */
static void aarch64_emit_not_asm(ZeroCCCompiler *c, int dst, int src, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *src_name = BE(c)->get_hw_reg_name(src);
    fprintf(out, "\tmvn\t%s, %s\n", dst_name, src_name);
}

/* LEA: add/sub dst, base, #offset */
static void aarch64_emit_lea_asm(ZeroCCCompiler *c, int dst, int base, int32_t offset, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *base_name = BE(c)->get_hw_reg_name(base);
    if (offset < 0) {
        fprintf(out, "\tsub\t%s, %s, #%d\n", dst_name, base_name, -offset);
    } else {
        fprintf(out, "\tadd\t%s, %s, #%d\n", dst_name, base_name, offset);
    }
}

/* DEREF: ldr dst, [src] */
static void aarch64_emit_deref_asm(ZeroCCCompiler *c, int dst, int src, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *src_name = BE(c)->get_hw_reg_name(src);
    fprintf(out, "\tldr\t%s, [%s]\n", dst_name, src_name);
}

/* PARAM_SAVE_INT: save incoming integer parameter (or x8 sret pointer) to [x29, #-offset] */
static void aarch64_emit_param_save_int_asm(ZeroCCCompiler *c, int reg_idx, int32_t offset, FILE *out) {
    if (reg_idx < 0 || reg_idx > 8) return;
    const char *reg_name = aarch64_get_hw_reg_name(reg_idx); /* x0..x7, or x8 for sret */
    aarch64_emit_store_asm(c, reg_idx, 29, -offset, out);
    (void)reg_name;
}

/* PARAM_SAVE_FP: save incoming FP parameter d0..d7 to [x29, #-offset] */
static void aarch64_emit_param_save_fp_asm(ZeroCCCompiler *c, int reg_idx, int32_t offset, FILE *out) {
    (void)c;
    if (reg_idx < 0 || reg_idx >= 8) return;
    if (-offset < -256) {
        fprintf(out, "\tsub\tx9, x29, #%d\n", offset);
        fprintf(out, "\tstr\td%d, [x9]\n", reg_idx);
    } else {
        fprintf(out, "\tstr\td%d, [x29, #-%d]\n", reg_idx, offset);
    }
}

/* CALL - Full AArch64 AAPCS64 compliance (including x8 sret and indirect blr calls) */
static void aarch64_emit_call_asm(ZeroCCCompiler *c, const char *func_name, int arg_count, int *arg_temps, int *is_fp, FILE *out) {
    static const char *int_arg_regs[] = {"x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7"};
    static const char *fp_arg_regs[] = {"d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7"};

    int int_arg_idx = 0;
    int fp_arg_idx = 0;
    int indirect_temp = -1;
    int sret_temp = -1;
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
        } else if (arg_is_fp == 3) {
            sret_temp = temp;
        } else if (arg_is_fp == 1 && fp_arg_idx < 8) {
            /* FP args go in d0..d7 (the 64-bit view of v0..v7) per AAPCS64. */
            fprintf(out, "\tfmov\t%s, d%s\n", fp_arg_regs[fp_arg_idx], reg_name + 1);
            fp_arg_idx++;
        } else if (arg_is_fp == 0) {
            int_arg_temps[total_int_count++] = temp;
            if (int_arg_idx < 8) {
                int_src_regs[int_arg_idx++] = phys_reg;
            }
        }
    }

    /* Save indirect callee into x10 before clobbering argument registers */
    int32_t spill_base_offset = c->regalloc.spill_base_offset;
    if (!func_name && indirect_temp >= 0) {
        int sp = regalloc_get_spill_slot_for_temp(&c->regalloc, indirect_temp);
        if (sp >= 0) {
            int32_t off = spill_base_offset + (sp + 1) * 8;
            BE(c)->emit_load_asm(c, 10 /* x10 */, BE(c)->fp_reg, -off, out);
        } else {
            int r_callee = regalloc_get_reg_for_temp(&c->regalloc, indirect_temp);
            fprintf(out, "\tmov\tx10, %s\n", BE(c)->get_hw_reg_name(r_callee));
        }
    }

    /* Save AAPCS64 sret pointer into x8 before clobbering argument registers */
    if (sret_temp >= 0) {
        int sp = regalloc_get_spill_slot_for_temp(&c->regalloc, sret_temp);
        if (sp >= 0) {
            int32_t off = spill_base_offset + (sp + 1) * 8;
            BE(c)->emit_load_asm(c, 8 /* x8 */, BE(c)->fp_reg, -off, out);
        } else {
            int r_sret = regalloc_get_reg_for_temp(&c->regalloc, sret_temp);
            fprintf(out, "\tmov\tx8, %s\n", BE(c)->get_hw_reg_name(r_sret));
        }
    }

    /* Arguments beyond x0-x7 (8 args) go on the stack: 16-byte aligned */
    int n_stack = (total_int_count > 8) ? total_int_count - 8 : 0;
    int stack_alloc = ((n_stack * 8) + 15) & ~15;
    if (n_stack > 0) {
        fprintf(out, "\tsub\tsp, sp, #%d\n", stack_alloc);
        for (int a = 8; a < total_int_count; a++) {
            int sp = regalloc_get_spill_slot_for_temp(&c->regalloc, int_arg_temps[a]);
            if (sp >= 0) {
                int32_t off = spill_base_offset + (sp + 1) * 8;
                BE(c)->emit_load_asm(c, 9 /* x9 */, BE(c)->fp_reg, -off, out);
                fprintf(out, "\tstr\tx9, [sp, #%d]\n", (a - 8) * 8);
            } else {
                int phys_reg = regalloc_get_reg_for_temp(&c->regalloc, int_arg_temps[a]);
                fprintf(out, "\tstr\t%s, [sp, #%d]\n", BE(c)->get_hw_reg_name(phys_reg), (a - 8) * 8);
            }
        }
    }

    /* Parallel move of integer arguments via stack push/pop so x0..x7 don't clobber each other */
    if (int_arg_idx > 0) {
        int alloc_bytes = ((int_arg_idx * 8) + 15) & ~15;
        fprintf(out, "\tsub\tsp, sp, #%d\n", alloc_bytes);
        for (int a = 0; a < int_arg_idx; a++) {
            int sp = regalloc_get_spill_slot_for_temp(&c->regalloc, int_arg_temps[a]);
            if (sp >= 0) {
                int32_t off = spill_base_offset + (sp + 1) * 8;
                BE(c)->emit_load_asm(c, 9 /* x9 */, BE(c)->fp_reg, -off, out);
                fprintf(out, "\tstr\tx9, [sp, #%d]\n", a * 8);
            } else {
                fprintf(out, "\tstr\t%s, [sp, #%d]\n", BE(c)->get_hw_reg_name(int_src_regs[a]), a * 8);
            }
        }
        for (int a = 0; a < int_arg_idx; a++) {
            fprintf(out, "\tldr\t%s, [sp, #%d]\n", int_arg_regs[a], a * 8);
        }
        fprintf(out, "\tadd\tsp, sp, #%d\n", alloc_bytes);
    }

    if (!func_name && indirect_temp >= 0) {
        fprintf(out, "\tblr\tx10\n");
    } else if (func_name) {
        fprintf(out, "\tbl\t%s\n", func_name);
    }

    /* Clean up stack arguments after call */
    if (n_stack > 0) {
        fprintf(out, "\tadd\tsp, sp, #%d\n", stack_alloc);
    }
}

/* RET: ret */
static void aarch64_emit_ret_asm(ZeroCCCompiler *, FILE *out) {
    fprintf(out, "\tret\n");
}

/* LABEL: label: */
static void aarch64_emit_label_asm(ZeroCCCompiler *, const char *label, FILE *out) {
    fprintf(out, "%s:\n", label);
}

/* JMP: b label */
static void aarch64_emit_jmp_asm(ZeroCCCompiler *, const char *label, FILE *out) {
    fprintf(out, "\tb\t%s\n", label);
}

/* JEQ: b.eq label */
static void aarch64_emit_jeq_asm(ZeroCCCompiler *, const char *label, FILE *out) {
    fprintf(out, "\tb.eq\t%s\n", label);
}

/* JNE: b.ne label */
static void aarch64_emit_jne_asm(ZeroCCCompiler *, const char *label, FILE *out) {
    fprintf(out, "\tb.ne\t%s\n", label);
}

/* JLT: b.lt label */
static void aarch64_emit_jlt_asm(ZeroCCCompiler *, const char *label, FILE *out) {
    fprintf(out, "\tb.lt\t%s\n", label);
}

/* JLE: b.le label */
static void aarch64_emit_jle_asm(ZeroCCCompiler *, const char *label, FILE *out) {
    fprintf(out, "\tb.le\t%s\n", label);
}

/* JGT: b.gt label */
static void aarch64_emit_jgt_asm(ZeroCCCompiler *, const char *label, FILE *out) {
    fprintf(out, "\tb.gt\t%s\n", label);
}

/* JGE: b.ge label */
static void aarch64_emit_jge_asm(ZeroCCCompiler *, const char *label, FILE *out) {
    fprintf(out, "\tb.ge\t%s\n", label);
}

/* FP LOAD: ldr dN, [base, #offset] */
static void aarch64_emit_load_fp_asm(ZeroCCCompiler *c, int dst, int base, int32_t offset, FILE *out) {
    const char *dst_name = BE(c)->regs->fp_regs[dst];
    const char *base_name = BE(c)->get_hw_reg_name(base);
    if (offset < -256 || (offset > 255 && (offset % 8 != 0 || offset > 32760))) {
        if (offset < 0) {
            fprintf(out, "\tsub\tx9, %s, #%d\n", base_name, -offset);
        } else {
            fprintf(out, "\tadd\tx9, %s, #%d\n", base_name, offset);
        }
        fprintf(out, "\tldr\td%s, [x9]\n", dst_name + 1);
    } else {
        fprintf(out, "\tldr\td%s, [%s, #%d]\n", dst_name + 1, base_name, offset);
    }
}

/* FP STORE: str dN, [base, #offset] */
static void aarch64_emit_store_fp_asm(ZeroCCCompiler *c, int src, int base, int32_t offset, FILE *out) {
    const char *src_name = BE(c)->regs->fp_regs[src];
    const char *base_name = BE(c)->get_hw_reg_name(base);
    if (offset < -256 || (offset > 255 && (offset % 8 != 0 || offset > 32760))) {
        if (offset < 0) {
            fprintf(out, "\tsub\tx9, %s, #%d\n", base_name, -offset);
        } else {
            fprintf(out, "\tadd\tx9, %s, #%d\n", base_name, offset);
        }
        fprintf(out, "\tstr\td%s, [x9]\n", src_name + 1);
    } else {
        fprintf(out, "\tstr\td%s, [%s, #%d]\n", src_name + 1, base_name, offset);
    }
}

/* FP ADD: fadd dst, src1, src2 */
static void aarch64_emit_fp_add_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->regs->fp_regs[dst];
    const char *src1_name = BE(c)->regs->fp_regs[src1];
    const char *src2_name = BE(c)->regs->fp_regs[src2];
    fprintf(out, "\tfadd\td%s, d%s, d%s\n", dst_name + 1, src1_name + 1, src2_name + 1);
}

/* FP SUB: fsub dst, src1, src2 */
static void aarch64_emit_fp_sub_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->regs->fp_regs[dst];
    const char *src1_name = BE(c)->regs->fp_regs[src1];
    const char *src2_name = BE(c)->regs->fp_regs[src2];
    fprintf(out, "\tfsub\td%s, d%s, d%s\n", dst_name + 1, src1_name + 1, src2_name + 1);
}

/* FP MUL: fmul dst, src1, src2 */
static void aarch64_emit_fp_mul_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->regs->fp_regs[dst];
    const char *src1_name = BE(c)->regs->fp_regs[src1];
    const char *src2_name = BE(c)->regs->fp_regs[src2];
    fprintf(out, "\tfmul\td%s, d%s, d%s\n", dst_name + 1, src1_name + 1, src2_name + 1);
}

/* FP DIV: fdiv dst, src1, src2 */
static void aarch64_emit_fp_div_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->regs->fp_regs[dst];
    const char *src1_name = BE(c)->regs->fp_regs[src1];
    const char *src2_name = BE(c)->regs->fp_regs[src2];
    fprintf(out, "\tfdiv\td%s, d%s, d%s\n", dst_name + 1, src1_name + 1, src2_name + 1);
}

/* FP compare: fcmp dN, dM - sets NZCV like a subtract so the standard
 * cset-based OP_COND_SET path works unchanged. */
static void aarch64_emit_fp_cmp_asm(ZeroCCCompiler *c, int src1, int src2, FILE *out) {
    fprintf(out, "\tfcmp\td%s, d%s\n",
            BE(c)->regs->fp_regs[src1] + 1, BE(c)->regs->fp_regs[src2] + 1);
}

/* FP MOV: fmov dst, src */
static void aarch64_emit_fp_mov_asm(ZeroCCCompiler *c, int dst, int src, FILE *out) {
    const char *dst_name = BE(c)->regs->fp_regs[dst];
    const char *src_name = BE(c)->regs->fp_regs[src];
    fprintf(out, "\tfmov\td%s, d%s\n", dst_name + 1, src_name + 1);
}

/* FP CVTSF: scvtf dD, xN (int -> double) */
static void aarch64_emit_fp_cvtsf_asm(ZeroCCCompiler *c, int fp_dst, int int_src, FILE *out) {
    const char *dst_name = BE(c)->regs->fp_regs[fp_dst];
    const char *src_name = BE(c)->get_hw_reg_name(int_src);
    fprintf(out, "\tscvtf\td%s, %s\n", dst_name + 1, src_name);
}

/* FP CVTSI: fcvtzs xN, dN (double -> int, truncate) */
static void aarch64_emit_fp_cvtsi_asm(ZeroCCCompiler *c, int int_dst, int fp_src, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(int_dst);
    const char *src_name = BE(c)->regs->fp_regs[fp_src];
    fprintf(out, "\tfcvtzs\t%s, d%s\n", dst_name, src_name + 1);
}

/* VLA_ALLOC: sub dst, sp, size */
static void aarch64_emit_vla_alloc_asm(ZeroCCCompiler *c, int dst, int size, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *size_name = BE(c)->get_hw_reg_name(size);
    fprintf(out, "\tsub\t%s, sp, %s\n", dst_name, size_name);
}

/* =========================================================================
 * Vector Instruction Emission (NEON)
 * ========================================================================= */

/* Vector load (128-bit) - ldr q */
static void aarch64_emit_vload_asm(ZeroCCCompiler *c, int dst, int base, int32_t offset, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *base_name = BE(c)->get_hw_reg_name(base);
    if (offset != 0) {
        fprintf(out, "\tldr\tq%s, [%s, #%d]\n", dst_name + 1, base_name, offset);
    } else {
        fprintf(out, "\tldr\tq%s, [%s]\n", dst_name + 1, base_name);
    }
}

/* Vector store (128-bit) - str q */
static void aarch64_emit_vstore_asm(ZeroCCCompiler *c, int src, int base, int32_t offset, FILE *out) {
    const char *src_name = BE(c)->get_vec_reg_name(src);
    const char *base_name = BE(c)->get_hw_reg_name(base);
    if (offset != 0) {
        fprintf(out, "\tstr\tq%s, [%s, #%d]\n", src_name + 1, base_name, offset);
    } else {
        fprintf(out, "\tstr\tq%s, [%s]\n", src_name + 1, base_name);
    }
}

/* Vector scalar single add - fadd s */
static void aarch64_emit_vaddss_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src1_name = BE(c)->get_vec_reg_name(src1);
    const char *src2_name = BE(c)->get_vec_reg_name(src2);
    fprintf(out, "\tfadd\ts%s, s%s, s%s\n", dst_name + 1, src1_name + 1, src2_name + 1);
}

/* Vector scalar single sub - fsub s */
static void aarch64_emit_vsubss_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src1_name = BE(c)->get_vec_reg_name(src1);
    const char *src2_name = BE(c)->get_vec_reg_name(src2);
    fprintf(out, "\tfsub\ts%s, s%s, s%s\n", dst_name + 1, src1_name + 1, src2_name + 1);
}

/* Vector scalar single mul - fmul s */
static void aarch64_emit_vmulss_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src1_name = BE(c)->get_vec_reg_name(src1);
    const char *src2_name = BE(c)->get_vec_reg_name(src2);
    fprintf(out, "\tfmul\ts%s, s%s, s%s\n", dst_name + 1, src1_name + 1, src2_name + 1);
}

/* Vector scalar single div - fdiv s */
static void aarch64_emit_vdivss_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src1_name = BE(c)->get_vec_reg_name(src1);
    const char *src2_name = BE(c)->get_vec_reg_name(src2);
    fprintf(out, "\tfdiv\ts%s, s%s, s%s\n", dst_name + 1, src1_name + 1, src2_name + 1);
}

/* Vector packed single add - fadd.4s */
static void aarch64_emit_vaddps_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src1_name = BE(c)->get_vec_reg_name(src1);
    const char *src2_name = BE(c)->get_vec_reg_name(src2);
    fprintf(out, "\tfadd\t%s.4s, %s.4s, %s.4s\n", dst_name, src1_name, src2_name);
}

/* Vector packed single sub - fsub.4s */
static void aarch64_emit_vsubps_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src1_name = BE(c)->get_vec_reg_name(src1);
    const char *src2_name = BE(c)->get_vec_reg_name(src2);
    fprintf(out, "\tfsub\t%s.4s, %s.4s, %s.4s\n", dst_name, src1_name, src2_name);
}

/* Vector packed single mul - fmul.4s */
static void aarch64_emit_vmulps_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src1_name = BE(c)->get_vec_reg_name(src1);
    const char *src2_name = BE(c)->get_vec_reg_name(src2);
    fprintf(out, "\tfmul\t%s.4s, %s.4s, %s.4s\n", dst_name, src1_name, src2_name);
}

/* Vector packed single div - fdiv.4s */
static void aarch64_emit_vdivps_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src1_name = BE(c)->get_vec_reg_name(src1);
    const char *src2_name = BE(c)->get_vec_reg_name(src2);
    fprintf(out, "\tfdiv\t%s.4s, %s.4s, %s.4s\n", dst_name, src1_name, src2_name);
}

/* Vector scalar double add - fadd d */
static void aarch64_emit_vaddsd_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src1_name = BE(c)->get_vec_reg_name(src1);
    const char *src2_name = BE(c)->get_vec_reg_name(src2);
    fprintf(out, "\tfadd\td%s, d%s, d%s\n", dst_name + 1, src1_name + 1, src2_name + 1);
}

/* Vector scalar double sub - fsub d */
static void aarch64_emit_vsubsd_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src1_name = BE(c)->get_vec_reg_name(src1);
    const char *src2_name = BE(c)->get_vec_reg_name(src2);
    fprintf(out, "\tfsub\td%s, d%s, d%s\n", dst_name + 1, src1_name + 1, src2_name + 1);
}

/* Vector scalar double mul - fmul d */
static void aarch64_emit_vmulsd_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src1_name = BE(c)->get_vec_reg_name(src1);
    const char *src2_name = BE(c)->get_vec_reg_name(src2);
    fprintf(out, "\tfmul\td%s, d%s, d%s\n", dst_name + 1, src1_name + 1, src2_name + 1);
}

/* Vector scalar double div - fdiv d */
static void aarch64_emit_vdivsd_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src1_name = BE(c)->get_vec_reg_name(src1);
    const char *src2_name = BE(c)->get_vec_reg_name(src2);
    fprintf(out, "\tfdiv\td%s, d%s, d%s\n", dst_name + 1, src1_name + 1, src2_name + 1);
}

/* Vector packed double add - fadd.2d */
static void aarch64_emit_vaddpd_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src1_name = BE(c)->get_vec_reg_name(src1);
    const char *src2_name = BE(c)->get_vec_reg_name(src2);
    fprintf(out, "\tfadd\t%s.2d, %s.2d, %s.2d\n", dst_name, src1_name, src2_name);
}

/* Vector packed double sub - fsub.2d */
static void aarch64_emit_vsubpd_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src1_name = BE(c)->get_vec_reg_name(src1);
    const char *src2_name = BE(c)->get_vec_reg_name(src2);
    fprintf(out, "\tfsub\t%s.2d, %s.2d, %s.2d\n", dst_name, src1_name, src2_name);
}

/* Vector packed double mul - fmul.2d */
static void aarch64_emit_vmulpd_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src1_name = BE(c)->get_vec_reg_name(src1);
    const char *src2_name = BE(c)->get_vec_reg_name(src2);
    fprintf(out, "\tfmul\t%s.2d, %s.2d, %s.2d\n", dst_name, src1_name, src2_name);
}

/* Vector packed double div - fdiv.2d */
static void aarch64_emit_vdivpd_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src1_name = BE(c)->get_vec_reg_name(src1);
    const char *src2_name = BE(c)->get_vec_reg_name(src2);
    fprintf(out, "\tfdiv\t%s.2d, %s.2d, %s.2d\n", dst_name, src1_name, src2_name);
}

/* Vector packed 32-bit int add - add.4s */
static void aarch64_emit_vpaddd_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src1_name = BE(c)->get_vec_reg_name(src1);
    const char *src2_name = BE(c)->get_vec_reg_name(src2);
    fprintf(out, "\tadd\t%s.4s, %s.4s, %s.4s\n", dst_name, src1_name, src2_name);
}

/* Vector packed 32-bit int sub - sub.4s */
static void aarch64_emit_vpsubd_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src1_name = BE(c)->get_vec_reg_name(src1);
    const char *src2_name = BE(c)->get_vec_reg_name(src2);
    fprintf(out, "\tsub\t%s.4s, %s.4s, %s.4s\n", dst_name, src1_name, src2_name);
}

/* Vector packed 32-bit int mul - mul.4s */
static void aarch64_emit_vpmulld_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src1_name = BE(c)->get_vec_reg_name(src1);
    const char *src2_name = BE(c)->get_vec_reg_name(src2);
    fprintf(out, "\tmul\t%s.4s, %s.4s, %s.4s\n", dst_name, src1_name, src2_name);
}

/* Vector move scalar - fmov */
static void aarch64_emit_vmovss_asm(ZeroCCCompiler *c, int dst, int src, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src_name = BE(c)->get_vec_reg_name(src);
    fprintf(out, "\tfmov\ts%s, s%s\n", dst_name + 1, src_name + 1);
}

/* Vector scalar double move - fmov (double) */
static void aarch64_emit_vmovsd_asm(ZeroCCCompiler *c, int dst, int src, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src_name = BE(c)->get_vec_reg_name(src);
    fprintf(out, "\tfmov\td%s, d%s\n", dst_name + 1, src_name + 1);
}

/* Vector move packed single - mov */
static void aarch64_emit_vmovaps_asm(ZeroCCCompiler *c, int dst, int src, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src_name = BE(c)->get_vec_reg_name(src);
    fprintf(out, "\tmov\t%s.4s, %s.4s\n", dst_name, src_name);
}

/* Vector move packed double - mov */
static void aarch64_emit_vmovapd_asm(ZeroCCCompiler *c, int dst, int src, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src_name = BE(c)->get_vec_reg_name(src);
    fprintf(out, "\tmov\t%s.2d, %s.2d\n", dst_name, src_name);
}

/* Vector bitwise and - and */
static void aarch64_emit_vandps_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src1_name = BE(c)->get_vec_reg_name(src1);
    const char *src2_name = BE(c)->get_vec_reg_name(src2);
    fprintf(out, "\tand\t%s.16b, %s.16b, %s.16b\n", dst_name, src1_name, src2_name);
}

/* Vector bitwise or - orr */
static void aarch64_emit_vorps_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src1_name = BE(c)->get_vec_reg_name(src1);
    const char *src2_name = BE(c)->get_vec_reg_name(src2);
    fprintf(out, "\torr\t%s.16b, %s.16b, %s.16b\n", dst_name, src1_name, src2_name);
}

/* Vector bitwise xor - eor */
static void aarch64_emit_vxorps_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src1_name = BE(c)->get_vec_reg_name(src1);
    const char *src2_name = BE(c)->get_vec_reg_name(src2);
    fprintf(out, "\teor\t%s.16b, %s.16b, %s.16b\n", dst_name, src1_name, src2_name);
}

/* Vector shuffle - ext */
static void aarch64_emit_vshufps_asm(ZeroCCCompiler *c, int dst, int src1, int src2, int imm, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src1_name = BE(c)->get_vec_reg_name(src1);
    const char *src2_name = BE(c)->get_vec_reg_name(src2);
    fprintf(out, "\text\t%s.16b, %s.16b, %s.16b, #%d\n", dst_name, src1_name, src2_name, imm);
}

/* Convert scalar int to scalar single - scvtf s */
static void aarch64_emit_vcvt_si2ss_asm(ZeroCCCompiler *c, int dst, int src, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src_name = BE(c)->get_hw_reg_name(src);
    fprintf(out, "\tscvtf\ts%s, %s\n", dst_name + 1, src_name);
}

/* Convert scalar single to scalar int - fcvtzs */
static void aarch64_emit_vcvt_ss2si_asm(ZeroCCCompiler *c, int dst, int src, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *src_name = BE(c)->get_vec_reg_name(src);
    fprintf(out, "\tfcvtzs\t%s, s%s\n", dst_name, src_name + 1);
}

/* Vector compare scalar single - fcmp */
static void aarch64_emit_vcmpss_asm(ZeroCCCompiler *c, int dst, int src1, int src2, int pred, FILE *out) {
    (void)dst; (void)pred;
    const char *src1_name = BE(c)->get_vec_reg_name(src1);
    const char *src2_name = BE(c)->get_vec_reg_name(src2);
    fprintf(out, "\tfcmp\ts%s, s%s\n", src1_name + 1, src2_name + 1);
}

/* Vector scalar double comparison - fcmp (double) */
static void aarch64_emit_vcmpsd_asm(ZeroCCCompiler *c, int dst, int src1, int src2, int pred, FILE *out) {
    (void)dst; (void)pred;
    const char *src1_name = BE(c)->get_vec_reg_name(src1);
    const char *src2_name = BE(c)->get_vec_reg_name(src2);
    fprintf(out, "\tfcmp\td%s, d%s\n", src1_name + 1, src2_name + 1);
}

/* Initialize AArch64 assembly emission functions in backend */
void aarch64_init_asm_emission(ZeroCCISABackend *be) {
    be->emit_prologue_asm = aarch64_emit_prologue_asm;
    be->emit_epilogue_asm = aarch64_emit_epilogue_asm;
    be->get_hw_reg_name = aarch64_get_hw_reg_name;
    be->get_vec_reg_name = aarch64_get_vec_reg_name;

    be->emit_mov_imm_asm = aarch64_emit_mov_imm_asm;
    be->emit_mov_reg_asm = aarch64_emit_mov_reg_asm;
    be->emit_load_asm = aarch64_emit_load_asm;
    be->emit_store_asm = aarch64_emit_store_asm;
    be->emit_add_asm = aarch64_emit_add_asm;
    be->emit_sub_asm = aarch64_emit_sub_asm;
    be->emit_mul_asm = aarch64_emit_mul_asm;
    be->emit_div_asm = aarch64_emit_div_asm;
    be->emit_mod_asm = aarch64_emit_mod_asm;
    be->emit_and_asm = aarch64_emit_and_asm;
    be->emit_or_asm = aarch64_emit_or_asm;
    be->emit_xor_asm = aarch64_emit_xor_asm;
    be->emit_shl_asm = aarch64_emit_shl_asm;
    be->emit_shr_asm = aarch64_emit_shr_asm;
    be->emit_sar_asm = aarch64_emit_sar_asm;
    be->emit_extend_asm = aarch64_emit_extend_asm;
    be->emit_cmp_asm = aarch64_emit_cmp_asm;
    be->emit_cond_set_asm = aarch64_emit_cond_set_asm;
    be->emit_neg_asm = aarch64_emit_neg_asm;
    be->emit_add_imm_asm = aarch64_emit_add_imm_asm;
    be->emit_sub_imm_asm = aarch64_emit_sub_imm_asm;
    be->emit_inc_asm = aarch64_emit_inc_asm;
    be->emit_dec_asm = aarch64_emit_dec_asm;
    be->emit_not_asm = aarch64_emit_not_asm;
    be->emit_lea_asm = aarch64_emit_lea_asm;
    be->emit_deref_asm = aarch64_emit_deref_asm;
    be->emit_call_asm = aarch64_emit_call_asm;
    be->emit_ret_asm = aarch64_emit_ret_asm;
    be->emit_label_asm = aarch64_emit_label_asm;
    be->emit_jmp_asm = aarch64_emit_jmp_asm;
    be->emit_jeq_asm = aarch64_emit_jeq_asm;
    be->emit_jne_asm = aarch64_emit_jne_asm;
    be->emit_jlt_asm = aarch64_emit_jlt_asm;
    be->emit_jle_asm = aarch64_emit_jle_asm;
    be->emit_jgt_asm = aarch64_emit_jgt_asm;
    be->emit_jge_asm = aarch64_emit_jge_asm;
    be->emit_load_fp_asm = aarch64_emit_load_fp_asm;
    be->emit_store_fp_asm = aarch64_emit_store_fp_asm;
    be->emit_fp_add_asm = aarch64_emit_fp_add_asm;
    be->emit_fp_sub_asm = aarch64_emit_fp_sub_asm;
    be->emit_fp_mul_asm = aarch64_emit_fp_mul_asm;
    be->emit_fp_div_asm = aarch64_emit_fp_div_asm;
    be->emit_fp_cmp_asm = aarch64_emit_fp_cmp_asm;
    be->emit_fp_mov_asm = aarch64_emit_fp_mov_asm;
    be->emit_fp_cvtsf_asm = aarch64_emit_fp_cvtsf_asm;
    be->emit_fp_cvtsi_asm = aarch64_emit_fp_cvtsi_asm;
    be->emit_vla_alloc_asm = aarch64_emit_vla_alloc_asm;
    be->emit_param_save_int_asm = aarch64_emit_param_save_int_asm;
    be->emit_param_save_fp_asm = aarch64_emit_param_save_fp_asm;

    /* Vector instruction emission (NEON) */
    be->emit_vload_asm = aarch64_emit_vload_asm;
    be->emit_vstore_asm = aarch64_emit_vstore_asm;
    be->emit_vaddss_asm = aarch64_emit_vaddss_asm;
    be->emit_vsubss_asm = aarch64_emit_vsubss_asm;
    be->emit_vmulss_asm = aarch64_emit_vmulss_asm;
    be->emit_vdivss_asm = aarch64_emit_vdivss_asm;
    be->emit_vaddps_asm = aarch64_emit_vaddps_asm;
    be->emit_vsubps_asm = aarch64_emit_vsubps_asm;
    be->emit_vmulps_asm = aarch64_emit_vmulps_asm;
    be->emit_vdivps_asm = aarch64_emit_vdivps_asm;
    be->emit_vaddsd_asm = aarch64_emit_vaddsd_asm;
    be->emit_vsubsd_asm = aarch64_emit_vsubsd_asm;
    be->emit_vmulsd_asm = aarch64_emit_vmulsd_asm;
    be->emit_vdivsd_asm = aarch64_emit_vdivsd_asm;
    be->emit_vaddpd_asm = aarch64_emit_vaddpd_asm;
    be->emit_vsubpd_asm = aarch64_emit_vsubpd_asm;
    be->emit_vmulpd_asm = aarch64_emit_vmulpd_asm;
    be->emit_vdivpd_asm = aarch64_emit_vdivpd_asm;
    be->emit_vpaddd_asm = aarch64_emit_vpaddd_asm;
    be->emit_vpsubd_asm = aarch64_emit_vpsubd_asm;
    be->emit_vpmulld_asm = aarch64_emit_vpmulld_asm;
    be->emit_vmovss_asm = aarch64_emit_vmovss_asm;
    be->emit_vmovsd_asm = aarch64_emit_vmovsd_asm;
    be->emit_vmovaps_asm = aarch64_emit_vmovaps_asm;
    be->emit_vmovapd_asm = aarch64_emit_vmovapd_asm;
    be->emit_vandps_asm = aarch64_emit_vandps_asm;
    be->emit_vorps_asm = aarch64_emit_vorps_asm;
    be->emit_vxorps_asm = aarch64_emit_vxorps_asm;
    be->emit_vshufps_asm = aarch64_emit_vshufps_asm;
    be->emit_vcvt_si2ss_asm = aarch64_emit_vcvt_si2ss_asm;
    be->emit_vcvt_ss2si_asm = aarch64_emit_vcvt_ss2si_asm;
    be->emit_vcmpss_asm = aarch64_emit_vcmpss_asm;
    be->emit_vcmpsd_asm = aarch64_emit_vcmpsd_asm;
}
