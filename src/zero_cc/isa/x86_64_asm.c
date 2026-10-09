/*
 * x86-64 Assembly Emitter
 * ISA-specific assembly emission for GNU Assembler (GAS) syntax
 */

#include "isa_backend.h"
#include "../parser/ir.h"
#include "../parser/regalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Hardware register names for x86-64 (indexed by hardware number) */
static const char *x86_hw_reg_names[] = {
    "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
    "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"
};

/* Get register name by hardware number */
static const char *x86_get_hw_reg_name(int phys_reg) {
    if (phys_reg >= 0 && phys_reg < 16) {
        return x86_hw_reg_names[phys_reg];
    }
    return "rax";  /* Default */
}

/* Prologue: push rbp, mov rbp, rsp, sub rsp, save callee-saved */
static void x86_emit_prologue_asm(ZeroCCCompiler *c, size_t stack_size) {
    FILE *out = c->asm_out;
    
    fprintf(out, "\tpushq\t%%rbp\n");
    fprintf(out, "\tmovq\t%%rsp, %%rbp\n");
    fprintf(out, "\tsubq\t$%zu, %%rsp\n", stack_size);
    
    /* Save callee-saved registers */
    const int *callee_saved = BE(c)->conv->callee_saved_gpr;
    int count = BE(c)->conv->callee_saved_gpr_count;
    
    for (int i = 0; i < count; i++) {
        int reg = callee_saved[i];
        const char *reg_name = BE(c)->get_hw_reg_name(reg);
        int offset = BE(c)->conv->callee_save_slot_offsets[i];
        fprintf(out, "\tmovq\t%%%s, %d(%%rbp)\n", reg_name, offset);
    }
}

/* Epilogue: restore callee-saved, leave, ret */
static void x86_emit_epilogue_asm(ZeroCCCompiler *c) {
    FILE *out = c->asm_out;
    
    /* Restore callee-saved registers */
    const int *callee_saved = BE(c)->conv->callee_saved_gpr;
    int count = BE(c)->conv->callee_saved_gpr_count;
    
    for (int i = 0; i < count; i++) {
        int reg = callee_saved[i];
        const char *reg_name = BE(c)->get_hw_reg_name(reg);
        int offset = BE(c)->conv->callee_save_slot_offsets[i];
        fprintf(out, "\tmovq\t%d(%%rbp), %%%s\n", offset, reg_name);
    }
    
    fprintf(out, "\tleave\n");
    fprintf(out, "\tret\n");
}

/* MOV_IMM: mov $imm, dst */
static void x86_emit_mov_imm_asm(ZeroCCCompiler *c, int dst, int64_t imm, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    fprintf(out, "\tmovq\t$%ld,%%%s\n", (long)imm, dst_name);
}

/* MOV_REG: mov src, dst */
static void x86_emit_mov_reg_asm(ZeroCCCompiler *c, int dst, int src, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *src_name = BE(c)->get_hw_reg_name(src);
    fprintf(out, "\tmovq\t%%%s,%%%s\n", src_name, dst_name);
}

/* LOAD: mov offset(base), dst */
static void x86_emit_load_asm(ZeroCCCompiler *c, int dst, int base, int32_t offset, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *base_name = BE(c)->get_hw_reg_name(base);
    fprintf(out, "\tmovq\t%d(%%%s),%%%s\n", offset, base_name, dst_name);
}

/* STORE: mov src, offset(base) */
static void x86_emit_store_asm(ZeroCCCompiler *c, int src, int base, int32_t offset, FILE *out) {
    const char *src_name = BE(c)->get_hw_reg_name(src);
    const char *base_name = BE(c)->get_hw_reg_name(base);
    fprintf(out, "\tmovq\t%%%s,%d(%%%s)\n", src_name, offset, base_name);
}

/* ADD: Handle src2 == dst case */
static void x86_emit_add_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *src1_name = BE(c)->get_hw_reg_name(src1);
    const char *src2_name = BE(c)->get_hw_reg_name(src2);
    
    if (dst == src2) {
        fprintf(out, "\taddq\t%%%s,%%%s\n", src1_name, dst_name);
    } else {
        fprintf(out, "\tmovq\t%%%s,%%%s\n", src1_name, dst_name);
        fprintf(out, "\taddq\t%%%s,%%%s\n", src2_name, dst_name);
    }
}

/* SUB: Handle src2 == dst case */
static void x86_emit_sub_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *src1_name = BE(c)->get_hw_reg_name(src1);
    const char *src2_name = BE(c)->get_hw_reg_name(src2);
    
    if (dst == src2) {
        fprintf(out, "\tsubq\t%%%s,%%%s\n", src1_name, dst_name);
    } else {
        fprintf(out, "\tmovq\t%%%s,%%%s\n", src1_name, dst_name);
        fprintf(out, "\tsubq\t%%%s,%%%s\n", src2_name, dst_name);
    }
}

/* MUL: x86-64 uses rax for multiplication */
static void x86_emit_mul_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *src1_name = BE(c)->get_hw_reg_name(src1);
    const char *src2_name = BE(c)->get_hw_reg_name(src2);
    
    fprintf(out, "\tmovq\t%%%s,%%rax\n", src1_name);
    fprintf(out, "\timulq\t%%%s\n", src2_name);
    fprintf(out, "\tmovq\t%%rax,%%%s\n", dst_name);
}

/* DIV: x86-64 uses rax:rdx for division */
static void x86_emit_div_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *src1_name = BE(c)->get_hw_reg_name(src1);
    const char *src2_name = BE(c)->get_hw_reg_name(src2);
    
    fprintf(out, "\tmovq\t%%%s,%%rax\n", src1_name);
    fprintf(out, "\tcqo\n");
    fprintf(out, "\tidivq\t%%%s\n", src2_name);
    fprintf(out, "\tmovq\t%%rax,%%%s\n", dst_name);
}

/* AND */
static void x86_emit_and_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *src1_name = BE(c)->get_hw_reg_name(src1);
    const char *src2_name = BE(c)->get_hw_reg_name(src2);
    
    fprintf(out, "\tmovq\t%%%s,%%%s\n", src1_name, dst_name);
    fprintf(out, "\tandq\t%%%s,%%%s\n", src2_name, dst_name);
}

/* OR */
static void x86_emit_or_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *src1_name = BE(c)->get_hw_reg_name(src1);
    const char *src2_name = BE(c)->get_hw_reg_name(src2);
    
    fprintf(out, "\tmovq\t%%%s,%%%s\n", src1_name, dst_name);
    fprintf(out, "\torq\t%%%s,%%%s\n", src2_name, dst_name);
}

/* XOR */
static void x86_emit_xor_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *src1_name = BE(c)->get_hw_reg_name(src1);
    const char *src2_name = BE(c)->get_hw_reg_name(src2);
    
    fprintf(out, "\tmovq\t%%%s,%%%s\n", src1_name, dst_name);
    fprintf(out, "\txorq\t%%%s,%%%s\n", src2_name, dst_name);
}

/* SHL */
static void x86_emit_shl_asm(ZeroCCCompiler *c, int dst, int src, int shift, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *src_name = BE(c)->get_hw_reg_name(src);
    const char *shift_name = BE(c)->get_hw_reg_name(shift);
    
    /* Handle RCX conflict (shift uses CL) */
    if (dst == 4) {  /* RCX */
        fprintf(out, "\tmovq\t%%%s,%%r10\n", src_name);
        fprintf(out, "\tmovq\t%%%s,%%rcx\n", shift_name);
        fprintf(out, "\tshlq\t%%cl,%%r10\n");
        fprintf(out, "\tmovq\t%%r10,%%%s\n", dst_name);
    } else {
        fprintf(out, "\tmovq\t%%%s,%%%s\n", src_name, dst_name);
        fprintf(out, "\tmovq\t%%%s,%%rcx\n", shift_name);
        fprintf(out, "\tshlq\t%%cl,%%%s\n", dst_name);
    }
}

/* SHR */
static void x86_emit_shr_asm(ZeroCCCompiler *c, int dst, int src, int shift, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *src_name = BE(c)->get_hw_reg_name(src);
    const char *shift_name = BE(c)->get_hw_reg_name(shift);
    
    if (dst == 4) {  /* RCX */
        fprintf(out, "\tmovq\t%%%s,%%r10\n", src_name);
        fprintf(out, "\tmovq\t%%%s,%%rcx\n", shift_name);
        fprintf(out, "\tshrq\t%%cl,%%r10\n");
        fprintf(out, "\tmovq\t%%r10,%%%s\n", dst_name);
    } else {
        fprintf(out, "\tmovq\t%%%s,%%%s\n", src_name, dst_name);
        fprintf(out, "\tmovq\t%%%s,%%rcx\n", shift_name);
        fprintf(out, "\tshrq\t%%cl,%%%s\n", dst_name);
    }
}

/* SAR */
static void x86_emit_sar_asm(ZeroCCCompiler *c, int dst, int src, int shift, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *src_name = BE(c)->get_hw_reg_name(src);
    const char *shift_name = BE(c)->get_hw_reg_name(shift);
    
    if (dst == 4) {  /* RCX */
        fprintf(out, "\tmovq\t%%%s,%%r10\n", src_name);
        fprintf(out, "\tmovq\t%%%s,%%rcx\n", shift_name);
        fprintf(out, "\tsarq\t%%cl,%%r10\n");
        fprintf(out, "\tmovq\t%%r10,%%%s\n", dst_name);
    } else {
        fprintf(out, "\tmovq\t%%%s,%%%s\n", src_name, dst_name);
        fprintf(out, "\tmovq\t%%%s,%%rcx\n", shift_name);
        fprintf(out, "\tsarq\t%%cl,%%%s\n", dst_name);
    }
}

/* CMP */
static void x86_emit_cmp_asm(ZeroCCCompiler *c, int src1, int src2, FILE *out) {
    const char *src1_name = BE(c)->get_hw_reg_name(src1);
    const char *src2_name = BE(c)->get_hw_reg_name(src2);
    fprintf(out, "\tcmpq\t%%%s,%%%s\n", src2_name, src1_name);
}

/* COND_SET */
static void x86_emit_cond_set_asm(ZeroCCCompiler *c, int dst, int cond, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *cc[] = {"l", "le", "g", "ge", "e", "ne"};
    fprintf(out, "\tset%s\t%%al\n", cc[cond]);
    fprintf(out, "\tmovzbl\t%%al,%%%s\n", dst_name);
}

/* NEG */
static void x86_emit_neg_asm(ZeroCCCompiler *c, int dst, int src, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *src_name = BE(c)->get_hw_reg_name(src);
    fprintf(out, "\tmovq\t%%%s,%%%s\n", src_name, dst_name);
    fprintf(out, "\tnegq\t%%%s\n", dst_name);
}

/* LEA */
static void x86_emit_lea_asm(ZeroCCCompiler *c, int dst, int base, int32_t offset, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *base_name = BE(c)->get_hw_reg_name(base);
    fprintf(out, "\tleaq\t%d(%%%s),%%%s\n", offset, base_name, dst_name);
}

/* DEREF */
static void x86_emit_deref_asm(ZeroCCCompiler *c, int dst, int src, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *src_name = BE(c)->get_hw_reg_name(src);
    fprintf(out, "\tmovq\t(%%%s),%%%s\n", src_name, dst_name);
}

/* CALL - Full x86-64 ABI compliance */
static void x86_emit_call_asm(ZeroCCCompiler *c, const char *func_name, int arg_count, int *arg_temps, int *is_fp, FILE *out) {
    /* x86-64 System V ABI argument registers */
    static const char *int_arg_regs[] = {"rdi", "rsi", "rdx", "rcx", "r8", "r9"};
    static const char *fp_arg_regs[] = {"xmm0", "xmm1", "xmm2", "xmm3", 
                                         "xmm4", "xmm5", "xmm6", "xmm7"};
    
    int int_arg_idx = 0;
    int fp_arg_idx = 0;
    
    for (int i = 0; i < arg_count; i++) {
        int temp = arg_temps[i];
        int phys_reg = regalloc_get_reg_for_temp(&c->regalloc, temp);
        const char *reg_name = BE(c)->get_hw_reg_name(phys_reg);
        
        /* Use is_fp array to determine if argument is FP */
        int arg_is_fp = is_fp ? is_fp[i] : 0;
        
        if (arg_is_fp && fp_arg_idx < 8) {
            fprintf(out, "\tmovsd\t%%%s, %%%s\n", reg_name, fp_arg_regs[fp_arg_idx]);
            fp_arg_idx++;
        } else if (!arg_is_fp && int_arg_idx < 6) {
            fprintf(out, "\tmovq\t%%%s, %%%s\n", reg_name, int_arg_regs[int_arg_idx]);
            int_arg_idx++;
        }
        /* Stack arguments would go here for calls with >6 integer args or >8 FP args */
    }
    
    fprintf(out, "\tcall\t%s\n", func_name);
}

/* RET */
static void x86_emit_ret_asm(ZeroCCCompiler *c, FILE *out) {
    fprintf(out, "\tret\n");
}

/* LABEL */
static void x86_emit_label_asm(ZeroCCCompiler *c, const char *label, FILE *out) {
    fprintf(out, "%s:\n", label);
}

/* JMP */
static void x86_emit_jmp_asm(ZeroCCCompiler *c, const char *label, FILE *out) {
    fprintf(out, "\tjmp\t%s\n", label);
}

/* JEQ */
static void x86_emit_jeq_asm(ZeroCCCompiler *c, const char *label, FILE *out) {
    fprintf(out, "\tje\t%s\n", label);
}

/* JNE */
static void x86_emit_jne_asm(ZeroCCCompiler *c, const char *label, FILE *out) {
    fprintf(out, "\tjne\t%s\n", label);
}

/* JLT */
static void x86_emit_jlt_asm(ZeroCCCompiler *c, const char *label, FILE *out) {
    fprintf(out, "\tjl\t%s\n", label);
}

/* JLE */
static void x86_emit_jle_asm(ZeroCCCompiler *c, const char *label, FILE *out) {
    fprintf(out, "\tjle\t%s\n", label);
}

/* JGT */
static void x86_emit_jgt_asm(ZeroCCCompiler *c, const char *label, FILE *out) {
    fprintf(out, "\tjg\t%s\n", label);
}

/* JGE */
static void x86_emit_jge_asm(ZeroCCCompiler *c, const char *label, FILE *out) {
    fprintf(out, "\tjge\t%s\n", label);
}

/* FP ADD */
static void x86_emit_fp_add_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->regs->fp_regs[dst];
    const char *src1_name = BE(c)->regs->fp_regs[src1];
    const char *src2_name = BE(c)->regs->fp_regs[src2];
    fprintf(out, "\taddsd\t%%%s,%%%s\n", src2_name, dst_name);
}

/* FP SUB */
static void x86_emit_fp_sub_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->regs->fp_regs[dst];
    const char *src1_name = BE(c)->regs->fp_regs[src1];
    const char *src2_name = BE(c)->regs->fp_regs[src2];
    fprintf(out, "\tsubsd\t%%%s,%%%s\n", src2_name, dst_name);
}

/* FP MUL */
static void x86_emit_fp_mul_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->regs->fp_regs[dst];
    const char *src1_name = BE(c)->regs->fp_regs[src1];
    const char *src2_name = BE(c)->regs->fp_regs[src2];
    fprintf(out, "\tmulsd\t%%%s,%%%s\n", src2_name, dst_name);
}

/* FP DIV */
static void x86_emit_fp_div_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->regs->fp_regs[dst];
    const char *src1_name = BE(c)->regs->fp_regs[src1];
    const char *src2_name = BE(c)->regs->fp_regs[src2];
    fprintf(out, "\tdivsd\t%%%s,%%%s\n", src2_name, dst_name);
}

/* FP MOV */
static void x86_emit_fp_mov_asm(ZeroCCCompiler *c, int dst, int src, FILE *out) {
    const char *dst_name = BE(c)->regs->fp_regs[dst];
    const char *src_name = BE(c)->regs->fp_regs[src];
    fprintf(out, "\tmovsd\t%%%s,%%%s\n", src_name, dst_name);
}

/* VLA_ALLOC */
static void x86_emit_vla_alloc_asm(ZeroCCCompiler *c, int dst, int size, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *size_name = BE(c)->get_hw_reg_name(size);
    fprintf(out, "\tmovq\t%%%s,%%rax\n", size_name);
    fprintf(out, "\tandq\t$-16,%%rax\n");
    fprintf(out, "\tsubq\t%%rax,%%rsp\n");
    fprintf(out, "\tmovq\t%%rsp,%%%s\n", dst_name);
}

/* Initialize x86-64 assembly emission functions in backend */
void x86_init_asm_emission(ZeroCCISABackend *be) {
    be->emit_prologue_asm = x86_emit_prologue_asm;
    be->emit_epilogue_asm = x86_emit_epilogue_asm;
    be->get_hw_reg_name = x86_get_hw_reg_name;
    
    be->emit_mov_imm_asm = x86_emit_mov_imm_asm;
    be->emit_mov_reg_asm = x86_emit_mov_reg_asm;
    be->emit_load_asm = x86_emit_load_asm;
    be->emit_store_asm = x86_emit_store_asm;
    be->emit_add_asm = x86_emit_add_asm;
    be->emit_sub_asm = x86_emit_sub_asm;
    be->emit_mul_asm = x86_emit_mul_asm;
    be->emit_div_asm = x86_emit_div_asm;
    be->emit_and_asm = x86_emit_and_asm;
    be->emit_or_asm = x86_emit_or_asm;
    be->emit_xor_asm = x86_emit_xor_asm;
    be->emit_shl_asm = x86_emit_shl_asm;
    be->emit_shr_asm = x86_emit_shr_asm;
    be->emit_sar_asm = x86_emit_sar_asm;
    be->emit_cmp_asm = x86_emit_cmp_asm;
    be->emit_cond_set_asm = x86_emit_cond_set_asm;
    be->emit_neg_asm = x86_emit_neg_asm;
    be->emit_lea_asm = x86_emit_lea_asm;
    be->emit_deref_asm = x86_emit_deref_asm;
    be->emit_call_asm = x86_emit_call_asm;
    be->emit_ret_asm = x86_emit_ret_asm;
    be->emit_label_asm = x86_emit_label_asm;
    be->emit_jmp_asm = x86_emit_jmp_asm;
    be->emit_jeq_asm = x86_emit_jeq_asm;
    be->emit_jne_asm = x86_emit_jne_asm;
    be->emit_jlt_asm = x86_emit_jlt_asm;
    be->emit_jle_asm = x86_emit_jle_asm;
    be->emit_jgt_asm = x86_emit_jgt_asm;
    be->emit_jge_asm = x86_emit_jge_asm;
    be->emit_fp_add_asm = x86_emit_fp_add_asm;
    be->emit_fp_sub_asm = x86_emit_fp_sub_asm;
    be->emit_fp_mul_asm = x86_emit_fp_mul_asm;
    be->emit_fp_div_asm = x86_emit_fp_div_asm;
    be->emit_fp_mov_asm = x86_emit_fp_mov_asm;
    be->emit_vla_alloc_asm = x86_emit_vla_alloc_asm;
}
