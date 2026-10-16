/*
 * RISC-V Assembly Emitter
 * ISA-specific assembly emission for GNU Assembler (GAS) syntax
 */

#include "isa_backend.h"
#include "../parser/ir.h"
#include "../parser/regalloc.h"
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
    static char buf[16];
    if (vec_reg_idx < 0 || vec_reg_idx >= 32) {
        snprintf(buf, sizeof(buf), "v0");  /* Default to v0 for invalid */
    } else {
        snprintf(buf, sizeof(buf), "v%d", vec_reg_idx);
    }
    return buf;
}

/* Prologue: allocate frame with s0 (fp) at top of local frame so negative offsets -off(s0) lie above sp */
static void riscv_emit_prologue_asm(ZeroCCCompiler *c, size_t stack_size) {
    FILE *out = c->asm_out ? c->asm_out : stdout;
    size_t aligned_sz = (stack_size + 15) & ~((size_t)15);
    size_t total_alloc = aligned_sz + 16; /* 16 bytes for ra and s0 above s0 */
    
    fprintf(out, "\taddi\tsp, sp, -%zu\n", total_alloc);
    fprintf(out, "\tsd\tra, %zu(sp)\n", aligned_sz + 8);
    fprintf(out, "\tsd\ts0, %zu(sp)\n", aligned_sz);
    fprintf(out, "\taddi\ts0, sp, %zu\n", aligned_sz);
    
    /* Save callee-saved registers s1..s11 at -offset(s0) */
    const int *callee_saved = BE(c)->conv->callee_saved_gpr;
    int count = BE(c)->conv->callee_saved_gpr_count;
    
    for (int i = 0; i < count; i++) {
        int reg = callee_saved[i];
        if (reg == 8) continue; /* s0 already saved above */
        const char *reg_name = BE(c)->get_hw_reg_name(reg);
        int offset = BE(c)->conv->callee_save_slot_offsets[i];
        fprintf(out, "\tsd\t%s, -%d(s0)\n", reg_name, offset);
    }
}

/* Epilogue: restore callee-saved, restore ra/s0, deallocate frame, ret */
static void riscv_emit_epilogue_asm(ZeroCCCompiler *c) {
    FILE *out = c->asm_out ? c->asm_out : stdout;
    
    /* Restore callee-saved registers s1..s11 */
    const int *callee_saved = BE(c)->conv->callee_saved_gpr;
    int count = BE(c)->conv->callee_saved_gpr_count;
    
    for (int i = 0; i < count; i++) {
        int reg = callee_saved[i];
        if (reg == 8) continue;
        const char *reg_name = BE(c)->get_hw_reg_name(reg);
        int offset = BE(c)->conv->callee_save_slot_offsets[i];
        fprintf(out, "\tld\t%s, -%d(s0)\n", reg_name, offset);
    }
    
    fprintf(out, "\taddi\tsp, s0, 0\n");
    fprintf(out, "\tld\tra, 8(sp)\n");
    fprintf(out, "\tld\ts0, 0(sp)\n");
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

/* LOAD: ld dst, offset(base) */
static void riscv_emit_load_asm(ZeroCCCompiler *c, int dst, int base, int32_t offset, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *base_name = BE(c)->get_hw_reg_name(base);
    fprintf(out, "\tld\t%s, %d(%s)\n", dst_name, offset, base_name);
}

/* STORE: sd src, offset(base) */
static void riscv_emit_store_asm(ZeroCCCompiler *c, int src, int base, int32_t offset, FILE *out) {
    const char *src_name = BE(c)->get_hw_reg_name(src);
    const char *base_name = BE(c)->get_hw_reg_name(base);
    fprintf(out, "\tsd\t%s, %d(%s)\n", src_name, offset, base_name);
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

/* DIV: div dst, src1, src2 */
static void riscv_emit_div_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *src1_name = BE(c)->get_hw_reg_name(src1);
    const char *src2_name = BE(c)->get_hw_reg_name(src2);
    fprintf(out, "\tdiv\t%s, %s, %s\n", dst_name, src1_name, src2_name);
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

/* CMP: sub t0, src1, src2 */
static void riscv_emit_cmp_asm(ZeroCCCompiler *c, int src1, int src2, FILE *out) {
    const char *src1_name = BE(c)->get_hw_reg_name(src1);
    const char *src2_name = BE(c)->get_hw_reg_name(src2);
    fprintf(out, "\tsub\tt0, %s, %s\n", src1_name, src2_name);
}

/* COND_SET: set dst to 1/0 based on t0 (src1 - src2) */
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

/* LEA: addi dst, base, offset */
static void riscv_emit_lea_asm(ZeroCCCompiler *c, int dst, int base, int32_t offset, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *base_name = BE(c)->get_hw_reg_name(base);
    fprintf(out, "\taddi\t%s, %s, %d\n", dst_name, base_name, offset);
}

/* DEREF: ld dst, 0(src) */
static void riscv_emit_deref_asm(ZeroCCCompiler *c, int dst, int src, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *src_name = BE(c)->get_hw_reg_name(src);
    fprintf(out, "\tld\t%s, 0(%s)\n", dst_name, src_name);
}

/* PARAM_SAVE_INT: save incoming integer parameter a0..a7 to -offset(s0) */
static void riscv_emit_param_save_int_asm(ZeroCCCompiler *c, int reg_idx, int32_t offset, FILE *out) {
    (void)c;
    if (reg_idx < 0 || reg_idx >= 8) return;
    fprintf(out, "\tsd\ta%d, -%d(s0)\n", reg_idx, offset);
}

/* PARAM_SAVE_FP: save incoming FP parameter fa0..fa7 to -offset(s0) */
static void riscv_emit_param_save_fp_asm(ZeroCCCompiler *c, int reg_idx, int32_t offset, FILE *out) {
    (void)c;
    if (reg_idx < 0 || reg_idx >= 8) return;
    fprintf(out, "\tfsd\tfa%d, -%d(s0)\n", reg_idx, offset);
}

/* CALL - Full RISC-V LP64D ABI compliance (including sret in a0 and indirect jalr calls) */
static void riscv_emit_call_asm(ZeroCCCompiler *c, const char *func_name, int arg_count, int *arg_temps, int *is_fp, FILE *out) {
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
        const char *reg_name = BE(c)->get_hw_reg_name(phys_reg);
        int arg_is_fp = is_fp ? is_fp[i] : 0;
        
        if (arg_is_fp == 2) {
            indirect_temp = temp;
        } else if (arg_is_fp == 1 && fp_arg_idx < 8) {
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
    if (!func_name && indirect_temp >= 0) {
        int r_callee = regalloc_get_reg_for_temp(&c->regalloc, indirect_temp);
        fprintf(out, "\tmv\tt1, %s\n", BE(c)->get_hw_reg_name(r_callee));
    }
    
    /* Arguments beyond a0-a7 (8 args) go on the stack: 16-byte aligned */
    int n_stack = (total_int_count > 8) ? total_int_count - 8 : 0;
    int stack_alloc = ((n_stack * 8) + 15) & ~15;
    if (n_stack > 0) {
        fprintf(out, "\taddi\tsp, sp, -%d\n", stack_alloc);
        for (int a = 8; a < total_int_count; a++) {
            int phys_reg = regalloc_get_reg_for_temp(&c->regalloc, int_arg_temps[a]);
            fprintf(out, "\tsd\t%s, %d(sp)\n", BE(c)->get_hw_reg_name(phys_reg), (a - 8) * 8);
        }
    }

    /* Parallel move of integer arguments via stack so a0..a7 don't clobber each other */
    if (int_arg_idx > 0) {
        int alloc_bytes = ((int_arg_idx * 8) + 15) & ~15;
        fprintf(out, "\taddi\tsp, sp, -%d\n", alloc_bytes);
        for (int a = 0; a < int_arg_idx; a++) {
            fprintf(out, "\tsd\t%s, %d(sp)\n", BE(c)->get_hw_reg_name(int_src_regs[a]), a * 8);
        }
        for (int a = 0; a < int_arg_idx; a++) {
            fprintf(out, "\tld\t%s, %d(sp)\n", int_arg_regs[a], a * 8);
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
static void riscv_emit_ret_asm(ZeroCCCompiler *c, FILE *out) {
    fprintf(out, "\tret\n");
}

/* LABEL: label: */
static void riscv_emit_label_asm(ZeroCCCompiler *c, const char *label, FILE *out) {
    fprintf(out, "%s:\n", label);
}

/* JMP: j label */
static void riscv_emit_jmp_asm(ZeroCCCompiler *c, const char *label, FILE *out) {
    fprintf(out, "\tj\t%s\n", label);
}

/* JEQ: beq t0, zero, label */
static void riscv_emit_jeq_asm(ZeroCCCompiler *c, const char *label, FILE *out) {
    fprintf(out, "\tbeq\tt0, zero, %s\n", label);
}

/* JNE: bne t0, zero, label */
static void riscv_emit_jne_asm(ZeroCCCompiler *c, const char *label, FILE *out) {
    fprintf(out, "\tbne\tt0, zero, %s\n", label);
}

/* JLT: blt t0, zero, label */
static void riscv_emit_jlt_asm(ZeroCCCompiler *c, const char *label, FILE *out) {
    fprintf(out, "\tblt\tt0, zero, %s\n", label);
}

/* JLE: ble t0, zero, label */
static void riscv_emit_jle_asm(ZeroCCCompiler *c, const char *label, FILE *out) {
    fprintf(out, "\tble\tt0, zero, %s\n", label);
}

/* JGT: bgt t0, zero, label */
static void riscv_emit_jgt_asm(ZeroCCCompiler *c, const char *label, FILE *out) {
    fprintf(out, "\tbgt\tt0, zero, %s\n", label);
}

/* JGE: bge t0, zero, label */
static void riscv_emit_jge_asm(ZeroCCCompiler *c, const char *label, FILE *out) {
    fprintf(out, "\tbge\tt0, zero, %s\n", label);
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
static void riscv_emit_vshufps_asm(ZeroCCCompiler *c, int dst, int src1, int src2, int imm, FILE *out) {
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

/* Vector compare - feq.s */
static void riscv_emit_vcmpss_asm(ZeroCCCompiler *c, int dst, int src1, int src2, int pred, FILE *out) {
    const char *src1_name = (src1 >= 0 && src1 < 32) ? BE(c)->regs->fp_regs[src1] : "fa0";
    const char *src2_name = (src2 >= 0 && src2 < 32) ? BE(c)->regs->fp_regs[src2] : "fa1";
    fprintf(out, "\tfeq.s\tt0, %s, %s\n", src1_name, src2_name);
}

/* Vector scalar double comparison - feq.d */
static void riscv_emit_vcmpsd_asm(ZeroCCCompiler *c, int dst, int src1, int src2, int pred, FILE *out) {
    const char *src1_name = (src1 >= 0 && src1 < 32) ? BE(c)->regs->fp_regs[src1] : "fa0";
    const char *src2_name = (src2 >= 0 && src2 < 32) ? BE(c)->regs->fp_regs[src2] : "fa1";
    fprintf(out, "\tfeq.d\tt0, %s, %s\n", src1_name, src2_name);
}

/* Initialize RISC-V assembly emission functions in backend */
void riscv64_init_asm_emission(ZeroCCISABackend *be) {
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
    be->emit_and_asm = riscv_emit_and_asm;
    be->emit_or_asm = riscv_emit_or_asm;
    be->emit_xor_asm = riscv_emit_xor_asm;
    be->emit_shl_asm = riscv_emit_shl_asm;
    be->emit_shr_asm = riscv_emit_shr_asm;
    be->emit_sar_asm = riscv_emit_sar_asm;
    be->emit_cmp_asm = riscv_emit_cmp_asm;
    be->emit_cond_set_asm = riscv_emit_cond_set_asm;
    be->emit_neg_asm = riscv_emit_neg_asm;
    be->emit_lea_asm = riscv_emit_lea_asm;
    be->emit_deref_asm = riscv_emit_deref_asm;
    be->emit_call_asm = riscv_emit_call_asm;
    be->emit_ret_asm = riscv_emit_ret_asm;
    be->emit_label_asm = riscv_emit_label_asm;
    be->emit_jmp_asm = riscv_emit_jmp_asm;
    be->emit_jeq_asm = riscv_emit_jeq_asm;
    be->emit_jne_asm = riscv_emit_jne_asm;
    be->emit_jlt_asm = riscv_emit_jlt_asm;
    be->emit_jle_asm = riscv_emit_jle_asm;
    be->emit_jgt_asm = riscv_emit_jgt_asm;
    be->emit_jge_asm = riscv_emit_jge_asm;
    be->emit_fp_add_asm = riscv_emit_fp_add_asm;
    be->emit_fp_sub_asm = riscv_emit_fp_sub_asm;
    be->emit_fp_mul_asm = riscv_emit_fp_mul_asm;
    be->emit_fp_div_asm = riscv_emit_fp_div_asm;
    be->emit_fp_mov_asm = riscv_emit_fp_mov_asm;
    be->emit_vla_alloc_asm = riscv_emit_vla_alloc_asm;
    be->emit_param_save_int_asm = riscv_emit_param_save_int_asm;
    be->emit_param_save_fp_asm = riscv_emit_param_save_fp_asm;
    
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
    be->emit_vcmpss_asm = riscv_emit_vcmpss_asm;
    be->emit_vcmpsd_asm = riscv_emit_vcmpsd_asm;
}
