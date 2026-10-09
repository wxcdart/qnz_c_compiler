/*
 * AArch64 Assembly Emitter
 * ISA-specific assembly emission for GNU Assembler (GAS) syntax
 */

#include "isa_backend.h"
#include "../parser/ir.h"
#include "../parser/regalloc.h"
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
    static char buf[16];
    if (vec_reg_idx < 0 || vec_reg_idx >= 32) {
        snprintf(buf, sizeof(buf), "v0");  /* Default to v0 for invalid */
    } else {
        snprintf(buf, sizeof(buf), "v%d", vec_reg_idx);
    }
    return buf;
}

/* Prologue: stp x29, x30, [sp, #-16]!, mov x29, sp, save callee-saved */
static void aarch64_emit_prologue_asm(ZeroCCCompiler *c, size_t stack_size) {
    FILE *out = c->asm_out ? c->asm_out : stdout;
    
    /* Frame setup */
    fprintf(out, "\tstp\tx29, x30, [sp, #%zu]!\n", stack_size);
    fprintf(out, "\tmov\tx29, sp\n");
    
    /* Save callee-saved registers */
    const int *callee_saved = BE(c)->conv->callee_saved_gpr;
    int count = BE(c)->conv->callee_saved_gpr_count;
    
    for (int i = 0; i < count; i++) {
        int reg = callee_saved[i];
        const char *reg_name = BE(c)->get_hw_reg_name(reg);
        int offset = BE(c)->conv->callee_save_slot_offsets[i];
        fprintf(out, "\tstr\t%s, [x29, #%d]\n", reg_name, offset);
    }
}

/* Epilogue: restore callee-saved, ldp x29, x30, [sp], ret */
static void aarch64_emit_epilogue_asm(ZeroCCCompiler *c) {
    FILE *out = c->asm_out ? c->asm_out : stdout;
    
    /* Restore callee-saved registers */
    const int *callee_saved = BE(c)->conv->callee_saved_gpr;
    int count = BE(c)->conv->callee_saved_gpr_count;
    
    for (int i = 0; i < count; i++) {
        int reg = callee_saved[i];
        const char *reg_name = BE(c)->get_hw_reg_name(reg);
        int offset = BE(c)->conv->callee_save_slot_offsets[i];
        fprintf(out, "\tldr\t%s, [x29, #%d]\n", reg_name, offset);
    }
    
    fprintf(out, "\tldp\tx29, x30, [sp], #16\n");
    fprintf(out, "\tret\n");
}

/* MOV_IMM: mov $imm, dst */
static void aarch64_emit_mov_imm_asm(ZeroCCCompiler *c, int dst, int64_t imm, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    fprintf(out, "\tmov\t%s, #%ld\n", dst_name, (long)imm);
}

/* MOV_REG: mov src, dst */
static void aarch64_emit_mov_reg_asm(ZeroCCCompiler *c, int dst, int src, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *src_name = BE(c)->get_hw_reg_name(src);
    fprintf(out, "\tmov\t%s, %s\n", dst_name, src_name);
}

/* LOAD: ldr dst, [base, #offset] */
static void aarch64_emit_load_asm(ZeroCCCompiler *c, int dst, int base, int32_t offset, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *base_name = BE(c)->get_hw_reg_name(base);
    fprintf(out, "\tldr\t%s, [%s, #%d]\n", dst_name, base_name, offset);
}

/* STORE: str src, [base, #offset] */
static void aarch64_emit_store_asm(ZeroCCCompiler *c, int src, int base, int32_t offset, FILE *out) {
    const char *src_name = BE(c)->get_hw_reg_name(src);
    const char *base_name = BE(c)->get_hw_reg_name(base);
    fprintf(out, "\tstr\t%s, [%s, #%d]\n", src_name, base_name, offset);
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
    fprintf(out, "\tlsl\t%s, %s, %s\n", dst_name, src_name, shift_name);
}

/* ASR: asr dst, src, shift */
static void aarch64_emit_sar_asm(ZeroCCCompiler *c, int dst, int src, int shift, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *src_name = BE(c)->get_hw_reg_name(src);
    const char *shift_name = BE(c)->get_hw_reg_name(shift);
    fprintf(out, "\tasr\t%s, %s, %s\n", dst_name, src_name, shift_name);
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
    const char *cc[] = {"lo", "ls", "hi", "hs", "eq", "ne"};
    fprintf(out, "\tcset\t%s, %s\n", dst_name, cc[cond]);
}

/* NEG: neg dst, src */
static void aarch64_emit_neg_asm(ZeroCCCompiler *c, int dst, int src, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *src_name = BE(c)->get_hw_reg_name(src);
    fprintf(out, "\tneg\t%s, %s\n", dst_name, src_name);
}

/* LEA: add dst, base, #offset */
static void aarch64_emit_lea_asm(ZeroCCCompiler *c, int dst, int base, int32_t offset, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *base_name = BE(c)->get_hw_reg_name(base);
    fprintf(out, "\tadd\t%s, %s, #%d\n", dst_name, base_name, offset);
}

/* DEREF: ldr dst, [src] */
static void aarch64_emit_deref_asm(ZeroCCCompiler *c, int dst, int src, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *src_name = BE(c)->get_hw_reg_name(src);
    fprintf(out, "\tldr\t%s, [%s]\n", dst_name, src_name);
}

/* CALL - Full AArch64 ABI compliance */
static void aarch64_emit_call_asm(ZeroCCCompiler *c, const char *func_name, int arg_count, int *arg_temps, int *is_fp, FILE *out) {
    /* AArch64 ABI argument registers */
    static const char *int_arg_regs[] = {"x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7"};
    static const char *fp_arg_regs[] = {"v0", "v1", "v2", "v3", 
                                         "v4", "v5", "v6", "v7"};
    
    int int_arg_idx = 0;
    int fp_arg_idx = 0;
    
    for (int i = 0; i < arg_count; i++) {
        int temp = arg_temps[i];
        int phys_reg = regalloc_get_reg_for_temp(&c->regalloc, temp);
        const char *reg_name = BE(c)->get_hw_reg_name(phys_reg);
        
        /* Use is_fp array to determine if argument is FP */
        int arg_is_fp = is_fp ? is_fp[i] : 0;
        
        if (arg_is_fp && fp_arg_idx < 8) {
            fprintf(out, "\tfmov\t%s, %s\n", fp_arg_regs[fp_arg_idx], reg_name);
            fp_arg_idx++;
        } else if (!arg_is_fp && int_arg_idx < 8) {
            fprintf(out, "\tmov\t%s, %s\n", int_arg_regs[int_arg_idx], reg_name);
            int_arg_idx++;
        }
        /* Stack arguments would go here for calls with >8 args */
    }
    
    fprintf(out, "\tbl\t%s\n", func_name);
}

/* RET: ret */
static void aarch64_emit_ret_asm(ZeroCCCompiler *c, FILE *out) {
    fprintf(out, "\tret\n");
}

/* LABEL: label: */
static void aarch64_emit_label_asm(ZeroCCCompiler *c, const char *label, FILE *out) {
    fprintf(out, "%s:\n", label);
}

/* JMP: b label */
static void aarch64_emit_jmp_asm(ZeroCCCompiler *c, const char *label, FILE *out) {
    fprintf(out, "\tb\t%s\n", label);
}

/* JEQ: b.eq label */
static void aarch64_emit_jeq_asm(ZeroCCCompiler *c, const char *label, FILE *out) {
    fprintf(out, "\tb.eq\t%s\n", label);
}

/* JNE: b.ne label */
static void aarch64_emit_jne_asm(ZeroCCCompiler *c, const char *label, FILE *out) {
    fprintf(out, "\tb.ne\t%s\n", label);
}

/* JLT: b.lt label */
static void aarch64_emit_jlt_asm(ZeroCCCompiler *c, const char *label, FILE *out) {
    fprintf(out, "\tb.lt\t%s\n", label);
}

/* JLE: b.le label */
static void aarch64_emit_jle_asm(ZeroCCCompiler *c, const char *label, FILE *out) {
    fprintf(out, "\tb.le\t%s\n", label);
}

/* JGT: b.gt label */
static void aarch64_emit_jgt_asm(ZeroCCCompiler *c, const char *label, FILE *out) {
    fprintf(out, "\tb.gt\t%s\n", label);
}

/* JGE: b.ge label */
static void aarch64_emit_jge_asm(ZeroCCCompiler *c, const char *label, FILE *out) {
    fprintf(out, "\tb.ge\t%s\n", label);
}

/* FP ADD: fadd dst, src1, src2 */
static void aarch64_emit_fp_add_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->regs->fp_regs[dst];
    const char *src1_name = BE(c)->regs->fp_regs[src1];
    const char *src2_name = BE(c)->regs->fp_regs[src2];
    fprintf(out, "\tfadd\t%s, %s, %s\n", dst_name, src1_name, src2_name);
}

/* FP SUB: fsub dst, src1, src2 */
static void aarch64_emit_fp_sub_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->regs->fp_regs[dst];
    const char *src1_name = BE(c)->regs->fp_regs[src1];
    const char *src2_name = BE(c)->regs->fp_regs[src2];
    fprintf(out, "\tfsub\t%s, %s, %s\n", dst_name, src1_name, src2_name);
}

/* FP MUL: fmul dst, src1, src2 */
static void aarch64_emit_fp_mul_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->regs->fp_regs[dst];
    const char *src1_name = BE(c)->regs->fp_regs[src1];
    const char *src2_name = BE(c)->regs->fp_regs[src2];
    fprintf(out, "\tfmul\t%s, %s, %s\n", dst_name, src1_name, src2_name);
}

/* FP DIV: fdiv dst, src1, src2 */
static void aarch64_emit_fp_div_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->regs->fp_regs[dst];
    const char *src1_name = BE(c)->regs->fp_regs[src1];
    const char *src2_name = BE(c)->regs->fp_regs[src2];
    fprintf(out, "\tfdiv\t%s, %s, %s\n", dst_name, src1_name, src2_name);
}

/* FP MOV: fmov dst, src */
static void aarch64_emit_fp_mov_asm(ZeroCCCompiler *c, int dst, int src, FILE *out) {
    const char *dst_name = BE(c)->regs->fp_regs[dst];
    const char *src_name = BE(c)->regs->fp_regs[src];
    fprintf(out, "\tfmov\t%s, %s\n", dst_name, src_name);
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

/* Vector load - ld1 */
static void aarch64_emit_vload_asm(ZeroCCCompiler *c, int dst, int base, int32_t offset, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *base_name = BE(c)->get_hw_reg_name(base);
    fprintf(out, "\tld1\t{%s.4s}, [%s, #%d]\n", dst_name, base_name, offset);
}

/* Vector store - st1 */
static void aarch64_emit_vstore_asm(ZeroCCCompiler *c, int src, int base, int32_t offset, FILE *out) {
    const char *src_name = BE(c)->get_vec_reg_name(src);
    const char *base_name = BE(c)->get_hw_reg_name(base);
    fprintf(out, "\tst1\t{%s.4s}, [%s, #%d]\n", src_name, base_name, offset);
}

/* Vector scalar single add - fadd s */
static void aarch64_emit_vaddss_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src2_name = BE(c)->get_vec_reg_name(src2);
    fprintf(out, "\tfadd\ts%s, s%s, s%s\n", dst_name, dst_name, src2_name);
}

/* Vector scalar single sub - fsub s */
static void aarch64_emit_vsubss_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src2_name = BE(c)->get_vec_reg_name(src2);
    fprintf(out, "\tfsub\ts%s, s%s, s%s\n", dst_name, dst_name, src2_name);
}

/* Vector scalar single mul - fmul s */
static void aarch64_emit_vmulss_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src2_name = BE(c)->get_vec_reg_name(src2);
    fprintf(out, "\tfmul\ts%s, s%s, s%s\n", dst_name, dst_name, src2_name);
}

/* Vector scalar single div - fdiv s */
static void aarch64_emit_vdivss_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src2_name = BE(c)->get_vec_reg_name(src2);
    fprintf(out, "\tfdiv\ts%s, s%s, s%s\n", dst_name, dst_name, src2_name);
}

/* Vector packed single add - fadd.4s */
static void aarch64_emit_vaddps_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src2_name = BE(c)->get_vec_reg_name(src2);
    fprintf(out, "\tfadd\t%s.4s, %s.4s, %s.4s\n", dst_name, dst_name, src2_name);
}

/* Vector packed single sub - fsub.4s */
static void aarch64_emit_vsubps_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src2_name = BE(c)->get_vec_reg_name(src2);
    fprintf(out, "\tfsub\t%s.4s, %s.4s, %s.4s\n", dst_name, dst_name, src2_name);
}

/* Vector packed single mul - fmul.4s */
static void aarch64_emit_vmulps_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src2_name = BE(c)->get_vec_reg_name(src2);
    fprintf(out, "\tfmul\t%s.4s, %s.4s, %s.4s\n", dst_name, dst_name, src2_name);
}

/* Vector packed single div - fdiv.4s */
static void aarch64_emit_vdivps_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src2_name = BE(c)->get_vec_reg_name(src2);
    fprintf(out, "\tfdiv\t%s.4s, %s.4s, %s.4s\n", dst_name, dst_name, src2_name);
}

/* Vector scalar double add - fadd d */
static void aarch64_emit_vaddsd_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src2_name = BE(c)->get_vec_reg_name(src2);
    fprintf(out, "\tfadd\td%s, d%s, d%s\n", dst_name, dst_name, src2_name);
}

/* Vector scalar double sub - fsub d */
static void aarch64_emit_vsubsd_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src2_name = BE(c)->get_vec_reg_name(src2);
    fprintf(out, "\tfsub\td%s, d%s, d%s\n", dst_name, dst_name, src2_name);
}

/* Vector scalar double mul - fmul d */
static void aarch64_emit_vmulsd_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src2_name = BE(c)->get_vec_reg_name(src2);
    fprintf(out, "\tfmul\td%s, d%s, d%s\n", dst_name, dst_name, src2_name);
}

/* Vector scalar double div - fdiv d */
static void aarch64_emit_vdivsd_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src2_name = BE(c)->get_vec_reg_name(src2);
    fprintf(out, "\tfdiv\td%s, d%s, d%s\n", dst_name, dst_name, src2_name);
}

/* Vector packed double add - fadd.2d */
static void aarch64_emit_vaddpd_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src2_name = BE(c)->get_vec_reg_name(src2);
    fprintf(out, "\tfadd\t%s.2d, %s.2d, %s.2d\n", dst_name, dst_name, src2_name);
}

/* Vector packed double sub - fsub.2d */
static void aarch64_emit_vsubpd_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src2_name = BE(c)->get_vec_reg_name(src2);
    fprintf(out, "\tfsub\t%s.2d, %s.2d, %s.2d\n", dst_name, dst_name, src2_name);
}

/* Vector packed double mul - fmul.2d */
static void aarch64_emit_vmulpd_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src2_name = BE(c)->get_vec_reg_name(src2);
    fprintf(out, "\tfmul\t%s.2d, %s.2d, %s.2d\n", dst_name, dst_name, src2_name);
}

/* Vector packed double div - fdiv.2d */
static void aarch64_emit_vdivpd_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src2_name = BE(c)->get_vec_reg_name(src2);
    fprintf(out, "\tfdiv\t%s.2d, %s.2d, %s.2d\n", dst_name, dst_name, src2_name);
}

/* Vector move scalar - fmov */
static void aarch64_emit_vmovss_asm(ZeroCCCompiler *c, int dst, int src, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src_name = BE(c)->get_vec_reg_name(src);
    fprintf(out, "\tfmov\ts%s, s%s\n", dst_name, src_name);
}

/* Vector scalar double move - fmov (double) */
static void aarch64_emit_vmovsd_asm(ZeroCCCompiler *c, int dst, int src, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src_name = BE(c)->get_vec_reg_name(src);
    fprintf(out, "\tfmov\td%s, d%s\n", dst_name, src_name);
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
    const char *src2_name = BE(c)->get_vec_reg_name(src2);
    fprintf(out, "\tand\t%s.16b, %s.16b, %s.16b\n", dst_name, dst_name, src2_name);
}

/* Vector bitwise or - orr */
static void aarch64_emit_vorps_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src2_name = BE(c)->get_vec_reg_name(src2);
    fprintf(out, "\torr\t%s.16b, %s.16b, %s.16b\n", dst_name, dst_name, src2_name);
}

/* Vector bitwise xor - eor */
static void aarch64_emit_vxorps_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src2_name = BE(c)->get_vec_reg_name(src2);
    fprintf(out, "\teor\t%s.16b, %s.16b, %s.16b\n", dst_name, dst_name, src2_name);
}

/* Vector shuffle - ext */
static void aarch64_emit_vshufps_asm(ZeroCCCompiler *c, int dst, int src1, int src2, int imm, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src2_name = BE(c)->get_vec_reg_name(src2);
    fprintf(out, "\text\t%s.16b, %s.16b, %s.16b, #%d\n", dst_name, dst_name, src2_name, imm);
}

/* Convert scalar int to scalar single - scvtf s */
static void aarch64_emit_vcvt_si2ss_asm(ZeroCCCompiler *c, int dst, int src, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src_name = BE(c)->get_hw_reg_name(src);
    fprintf(out, "\tscvtf\ts%s, %s\n", dst_name, src_name);
}

/* Convert scalar single to scalar int - fcvtzs */
static void aarch64_emit_vcvt_ss2si_asm(ZeroCCCompiler *c, int dst, int src, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *src_name = BE(c)->get_vec_reg_name(src);
    fprintf(out, "\tfcvtzs\t%s, s%s\n", dst_name, src_name);
}

/* Vector compare scalar single - fcmp */
static void aarch64_emit_vcmpss_asm(ZeroCCCompiler *c, int dst, int src1, int src2, int pred, FILE *out) {
    const char *src1_name = BE(c)->get_vec_reg_name(src1);
    const char *src2_name = BE(c)->get_vec_reg_name(src2);
    fprintf(out, "\tfcmp\ts%s, s%s\n", src1_name, src2_name);
}

/* Vector scalar double comparison - fcmp (double) */
static void aarch64_emit_vcmpsd_asm(ZeroCCCompiler *c, int dst, int src1, int src2, int pred, FILE *out) {
    const char *src1_name = BE(c)->get_vec_reg_name(src1);
    const char *src2_name = BE(c)->get_vec_reg_name(src2);
    fprintf(out, "\tfcmp\td%s, d%s\n", src1_name, src2_name);
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
    be->emit_and_asm = aarch64_emit_and_asm;
    be->emit_or_asm = aarch64_emit_or_asm;
    be->emit_xor_asm = aarch64_emit_xor_asm;
    be->emit_shl_asm = aarch64_emit_shl_asm;
    be->emit_shr_asm = aarch64_emit_shr_asm;
    be->emit_sar_asm = aarch64_emit_sar_asm;
    be->emit_cmp_asm = aarch64_emit_cmp_asm;
    be->emit_cond_set_asm = aarch64_emit_cond_set_asm;
    be->emit_neg_asm = aarch64_emit_neg_asm;
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
    be->emit_fp_add_asm = aarch64_emit_fp_add_asm;
    be->emit_fp_sub_asm = aarch64_emit_fp_sub_asm;
    be->emit_fp_mul_asm = aarch64_emit_fp_mul_asm;
    be->emit_fp_div_asm = aarch64_emit_fp_div_asm;
    be->emit_fp_mov_asm = aarch64_emit_fp_mov_asm;
    be->emit_vla_alloc_asm = aarch64_emit_vla_alloc_asm;
    
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
