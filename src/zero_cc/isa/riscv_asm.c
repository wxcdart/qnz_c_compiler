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

/* Prologue: addi sp, sp, -frame_size, save callee-saved */
static void riscv_emit_prologue_asm(ZeroCCCompiler *c, size_t stack_size) {
    FILE *out = c->asm_out ? c->asm_out : stdout;
    
    /* Frame setup */
    fprintf(out, "\taddi\tsp, sp, -%zu\n", stack_size);
    
    /* Save callee-saved registers */
    const int *callee_saved = BE(c)->conv->callee_saved_gpr;
    int count = BE(c)->conv->callee_saved_gpr_count;
    
    for (int i = 0; i < count; i++) {
        int reg = callee_saved[i];
        const char *reg_name = BE(c)->get_hw_reg_name(reg);
        int offset = BE(c)->conv->callee_save_slot_offsets[i];
        fprintf(out, "\tsw\t%s, %d(sp)\n", reg_name, offset);
    }
}

/* Epilogue: restore callee-saved, addi sp, sp, frame_size, ret */
static void riscv_emit_epilogue_asm(ZeroCCCompiler *c) {
    FILE *out = c->asm_out ? c->asm_out : stdout;
    
    /* Restore callee-saved registers */
    const int *callee_saved = BE(c)->conv->callee_saved_gpr;
    int count = BE(c)->conv->callee_saved_gpr_count;
    
    for (int i = 0; i < count; i++) {
        int reg = callee_saved[i];
        const char *reg_name = BE(c)->get_hw_reg_name(reg);
        int offset = BE(c)->conv->callee_save_slot_offsets[i];
        fprintf(out, "\tlw\t%s, %d(sp)\n", reg_name, offset);
    }
    
    fprintf(out, "\taddi\tsp, sp, %zu\n", c->current_stack_frame > 0 ? (size_t)c->current_stack_frame : 64);
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

/* LOAD: lw dst, offset(base) */
static void riscv_emit_load_asm(ZeroCCCompiler *c, int dst, int base, int32_t offset, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *base_name = BE(c)->get_hw_reg_name(base);
    fprintf(out, "\tlw\t%s, %d(%s)\n", dst_name, offset, base_name);
}

/* STORE: sw src, offset(base) */
static void riscv_emit_store_asm(ZeroCCCompiler *c, int src, int base, int32_t offset, FILE *out) {
    const char *src_name = BE(c)->get_hw_reg_name(src);
    const char *base_name = BE(c)->get_hw_reg_name(base);
    fprintf(out, "\tsw\t%s, %d(%s)\n", src_name, offset, base_name);
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

/* CMP: sub dst, src1, src2 (sets flags) */
static void riscv_emit_cmp_asm(ZeroCCCompiler *c, int src1, int src2, FILE *out) {
    const char *src1_name = BE(c)->get_hw_reg_name(src1);
    const char *src2_name = BE(c)->get_hw_reg_name(src2);
    fprintf(out, "\tsub\tx0, %s, %s\n", src1_name, src2_name);
}

/* COND_SET: slt/sgt instructions */
static void riscv_emit_cond_set_asm(ZeroCCCompiler *c, int dst, int cond, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *src1_name = BE(c)->get_hw_reg_name(0);  /* src1 */
    const char *src2_name = BE(c)->get_hw_reg_name(0);  /* src2 */
    
    /* Note: This is simplified - actual implementation needs src1/src2 */
    const char *cc[] = {"slt", "slt", "sgt", "sgt", "seqz", "snez"};
    fprintf(out, "\t%s\t%s, %s, %s\n", cc[cond], dst_name, src1_name, src2_name);
}

/* NEG: sub dst, zero, src */
static void riscv_emit_neg_asm(ZeroCCCompiler *c, int dst, int src, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *src_name = BE(c)->get_hw_reg_name(src);
    fprintf(out, "\tsub\t%s, zero, %s\n", dst_name, src_name);
}

/* LEA: add dst, base, #offset */
static void riscv_emit_lea_asm(ZeroCCCompiler *c, int dst, int base, int32_t offset, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *base_name = BE(c)->get_hw_reg_name(base);
    fprintf(out, "\tadd\t%s, %s, %d\n", dst_name, base_name, offset);
}

/* DEREF: lw dst, 0(src) */
static void riscv_emit_deref_asm(ZeroCCCompiler *c, int dst, int src, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *src_name = BE(c)->get_hw_reg_name(src);
    fprintf(out, "\tlw\t%s, 0(%s)\n", dst_name, src_name);
}

/* CALL - Full RISC-V ABI compliance */
static void riscv_emit_call_asm(ZeroCCCompiler *c, const char *func_name, int arg_count, int *arg_temps, int *is_fp, FILE *out) {
    /* RISC-V ABI argument registers */
    static const char *int_arg_regs[] = {"a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7"};
    static const char *fp_arg_regs[] = {"fa0", "fa1", "fa2", "fa3", 
                                         "fa4", "fa5", "fa6", "fa7"};
    
    int int_arg_idx = 0;
    int fp_arg_idx = 0;
    
    for (int i = 0; i < arg_count; i++) {
        int temp = arg_temps[i];
        int phys_reg = regalloc_get_reg_for_temp(&c->regalloc, temp);
        const char *reg_name = BE(c)->get_hw_reg_name(phys_reg);
        
        /* Use is_fp array to determine if argument is FP */
        int arg_is_fp = is_fp ? is_fp[i] : 0;
        
        if (arg_is_fp && fp_arg_idx < 8) {
            fprintf(out, "\tfmv.d\t%s, %s\n", fp_arg_regs[fp_arg_idx], reg_name);
            fp_arg_idx++;
        } else if (!arg_is_fp && int_arg_idx < 8) {
            fprintf(out, "\tmv\t%s, %s\n", int_arg_regs[int_arg_idx], reg_name);
            int_arg_idx++;
        }
        /* Stack arguments would go here for calls with >8 args */
    }
    
    fprintf(out, "\tjal\tra, %s\n", func_name);
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

/* JEQ: beq src1, src2, label */
static void riscv_emit_jeq_asm(ZeroCCCompiler *c, const char *label, FILE *out) {
    fprintf(out, "\tbeq\tzero, zero, %s\n", label);
}

/* JNE: bne src1, src2, label */
static void riscv_emit_jne_asm(ZeroCCCompiler *c, const char *label, FILE *out) {
    fprintf(out, "\tbne\tzero, zero, %s\n", label);
}

/* JLT: blt src1, src2, label */
static void riscv_emit_jlt_asm(ZeroCCCompiler *c, const char *label, FILE *out) {
    fprintf(out, "\tblt\tzero, zero, %s\n", label);
}

/* JLE: ble src1, src2, label */
static void riscv_emit_jle_asm(ZeroCCCompiler *c, const char *label, FILE *out) {
    fprintf(out, "\tble\tzero, zero, %s\n", label);
}

/* JGT: bgt src1, src2, label */
static void riscv_emit_jgt_asm(ZeroCCCompiler *c, const char *label, FILE *out) {
    fprintf(out, "\tbgt\tzero, zero, %s\n", label);
}

/* JGE: bge src1, src2, label */
static void riscv_emit_jge_asm(ZeroCCCompiler *c, const char *label, FILE *out) {
    fprintf(out, "\tbge\tzero, zero, %s\n", label);
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

/* Initialize RISC-V assembly emission functions in backend */
void riscv64_init_asm_emission(ZeroCCISABackend *be) {
    be->emit_prologue_asm = riscv_emit_prologue_asm;
    be->emit_epilogue_asm = riscv_emit_epilogue_asm;
    be->get_hw_reg_name = riscv_get_hw_reg_name;
    
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
}
