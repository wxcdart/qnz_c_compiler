/* IR Emitter - Operation Emission Functions
 * 
 * Additional IR emission functions beyond those in ir.h
 * Provides more complete coverage for IR-based codegen.
 */

#ifndef ZERO_CC_PARSER_IR_EMIT_OPS_H
#define ZERO_CC_PARSER_IR_EMIT_OPS_H

#include "ir.h"

/* =========================================================================
 * Data Movement Operations
 * ========================================================================= */

/* mov [rbp - offset], rax (store) */
static inline void ir_emit_store(int temp_src, int fp_reg, int offset) {
    (void)fp_reg;  /* Frame pointer is fixed, not a temp */
    ir_emit((Instr){
        .op = OP_MOV_MEM,
        .temp_dest= TEMP_NONE,
        .temp_src1 = (uint16_t)temp_src,
        .temp_src2= TEMP_NONE,  /* Frame pointer marker */
        .cond = 0,
        .flags_out = 0xFF,  /* Doesn't set flags */
        .flags_in = 0xFF,   /* Doesn't read flags */
        .imm = offset,
        .imm64 = 0,
        .label_idx = 0
    });
}

/* mov rax, [rbp - offset] (load) */
static inline void ir_emit_load(int temp_dest, int fp_reg, int offset) {
    (void)fp_reg;  /* Frame pointer is fixed, not a temp */
    ir_emit((Instr){
        .op = OP_MOV,
        .temp_dest = (uint16_t)temp_dest,
        .temp_src1= TEMP_NONE,  /* Frame pointer marker */
        .temp_src2= TEMP_NONE,
        .cond = 0,
        .flags_out = 0xFF,  /* Doesn't set flags */
        .flags_in = 0xFF,   /* Doesn't read flags */
        .imm = offset,
        .imm64 = 0,
        .label_idx = 0
    });
}

/* mov rax, rbx (register move) */
static inline void ir_emit_mov_reg(int temp_dest, int temp_src) {
    ir_emit((Instr){
        .op = OP_MOV_REG,
        .temp_dest = (uint16_t)temp_dest,
        .temp_src1 = (uint16_t)temp_src,
        .temp_src2= TEMP_NONE,
        .cond = 0,
        .flags_out = 0xFF,  /* Doesn't set flags */
        .flags_in = 0xFF,   /* Doesn't read flags */
        .imm = 0,
        .imm64 = 0,
        .label_idx = 0
    });
}

/* In-place width extension: sign- or zero-extend the low 8/16/32 bits of a
 * temp to full 64-bit width. Used to give 32-bit `int` values true 32-bit
 * wrap semantics (the machine word is 64 bits, so without this a `int a=2000000000;
 * int b=a+a;` would compare b against a 64-bit residue instead of the
 * 32-bit wrap value). temp_dest must equal temp_src (in-place, no new temp). */
static inline void ir_emit_sext(int temp, int width) {
    Opcode op = (width == 4) ? OP_SEXT32 : (width == 2) ? OP_SEXT16 : OP_SEXT8;
    ir_emit((Instr){
        .op = op,
        .temp_dest = (uint16_t)temp,
        .temp_src1 = (uint16_t)temp,
        .temp_src2= TEMP_NONE,
        .cond = 0,
        .flags_out = 0xFF,
        .flags_in = 0xFF,
        .imm = 0,
        .imm64 = 0,
        .label_idx = 0
    });
}

static inline void ir_emit_zext(int temp, int width) {
    Opcode op = (width == 4) ? OP_ZEXT32 : (width == 2) ? OP_ZEXT16 : OP_ZEXT8;
    ir_emit((Instr){
        .op = op,
        .temp_dest = (uint16_t)temp,
        .temp_src1 = (uint16_t)temp,
        .temp_src2= TEMP_NONE,
        .cond = 0,
        .flags_out = 0xFF,
        .flags_in = 0xFF,
        .imm = 0,
        .imm64 = 0,
        .label_idx = 0
    });
}

/* lea rax, [rbp - offset] (address) */
static inline void ir_emit_lea(int temp_dest, int fp_reg, int offset) {
    (void)fp_reg;  /* Frame pointer is fixed, not a temp */
    ir_emit((Instr){
        .op = OP_LEA,
        .temp_dest = (uint16_t)temp_dest,
        .temp_src1= TEMP_NONE,
        .temp_src2= TEMP_NONE,
        .cond = 0,
        .flags_out = 0xFF,  /* Doesn't set flags */
        .flags_in = 0xFF,   /* Doesn't read flags */
        .imm = offset,
        .imm64 = 0,
        .label_idx = 0
    });
}

/* =========================================================================
 * Arithmetic Operations
 * ========================================================================= */

/* add rax, imm */
static inline void ir_emit_add_imm(int temp_dest, int16_t imm) {
    ir_emit((Instr){
        .op = OP_ADD_IMM,
        .temp_dest = (uint16_t)temp_dest,
        .temp_src1= TEMP_NONE,
        .temp_src2= TEMP_NONE,
        .cond = 0,
        .flags_out = 0xFF,  /* Doesn't set flags */
        .flags_in = 0xFF,   /* Doesn't read flags */
        .imm = imm,
        .imm64 = 0,
        .label_idx = 0
    });
}


/* sub rax, imm */
static inline void ir_emit_sub_imm(int temp_dest, int16_t imm) {
    ir_emit((Instr){
        .op = OP_SUB_IMM,
        .temp_dest = (uint16_t)temp_dest,
        .temp_src1= TEMP_NONE,
        .temp_src2= TEMP_NONE,
        .cond = 0,
        .flags_out = 0xFF,  /* Doesn't set flags */
        .flags_in = 0xFF,   /* Doesn't read flags */
        .imm = imm,
        .imm64 = 0,
        .label_idx = 0
    });
}

/* div rbx */
static inline void ir_emit_div(int temp_dest, int temp_src1, int temp_src2) {
    ir_emit((Instr){
        .op = OP_DIV,
        .temp_dest = (uint16_t)temp_dest,
        .temp_src1 = (uint16_t)temp_src1,
        .temp_src2 = (uint16_t)temp_src2,
        .cond = 0,
        .flags_out = 0xFF,  /* Doesn't set flags */
        .flags_in = 0xFF,   /* Doesn't read flags */
        .imm = 0,
        .imm64 = 0,
        .label_idx = 0
    });
}

/* mod rbx */
static inline void ir_emit_mod(int temp_dest, int temp_src1, int temp_src2) {
    ir_emit((Instr){
        .op = OP_MOD,
        .temp_dest = (uint16_t)temp_dest,
        .temp_src1 = (uint16_t)temp_src1,
        .temp_src2 = (uint16_t)temp_src2,
        .cond = 0,
        .flags_out = 0xFF,  /* Doesn't set flags */
        .flags_in = 0xFF,   /* Doesn't read flags */
        .imm = 0,
        .imm64 = 0,
        .label_idx = 0
    });
}

/* neg rax */
static inline void ir_emit_neg(int temp_dest, int temp_src) {
    ir_emit((Instr){
        .op = OP_NEG,
        .temp_dest = (uint16_t)temp_dest,
        .temp_src1 = (uint16_t)temp_src,
        .temp_src2= TEMP_NONE,
        .cond = 0,
        .flags_out = 0xFF,  /* Doesn't set flags */
        .flags_in = 0xFF,   /* Doesn't read flags */
        .imm = 0,
        .imm64 = 0,
        .label_idx = 0
    });
}

/* inc rax */
static inline void ir_emit_inc(int temp_dest) {
    ir_emit((Instr){
        .op = OP_INC,
        .temp_dest = (uint16_t)temp_dest,
        .temp_src1= TEMP_NONE,
        .temp_src2= TEMP_NONE,
        .cond = 0,
        .flags_out = 0xFF,  /* Doesn't set flags */
        .flags_in = 0xFF,   /* Doesn't read flags */
        .imm = 0,
        .imm64 = 0,
        .label_idx = 0
    });
}

/* dec rax */
static inline void ir_emit_dec(int temp_dest) {
    ir_emit((Instr){
        .op = OP_DEC,
        .temp_dest = (uint16_t)temp_dest,
        .temp_src1= TEMP_NONE,
        .temp_src2= TEMP_NONE,
        .cond = 0,
        .flags_out = 0xFF,  /* Doesn't set flags */
        .flags_in = 0xFF,   /* Doesn't read flags */
        .imm = 0,
        .imm64 = 0,
        .label_idx = 0
    });
}

/* =========================================================================
 * Logical Operations
 * ========================================================================= */

/* and rax, rbx */
static inline void ir_emit_and(int temp_dest, int temp_src1, int temp_src2) {
    ir_emit((Instr){
        .op = OP_AND,
        .temp_dest = (uint16_t)temp_dest,
        .temp_src1 = (uint16_t)temp_src1,
        .temp_src2 = (uint16_t)temp_src2,
        .cond = 0,
        .flags_out = 0xFF,  /* Doesn't set flags */
        .flags_in = 0xFF,   /* Doesn't read flags */
        .imm = 0,
        .imm64 = 0,
        .label_idx = 0
    });
}

/* or rax, rbx */
static inline void ir_emit_or(int temp_dest, int temp_src1, int temp_src2) {
    ir_emit((Instr){
        .op = OP_OR,
        .temp_dest = (uint16_t)temp_dest,
        .temp_src1 = (uint16_t)temp_src1,
        .temp_src2 = (uint16_t)temp_src2,
        .cond = 0,
        .flags_out = 0xFF,  /* Doesn't set flags */
        .flags_in = 0xFF,   /* Doesn't read flags */
        .imm = 0,
        .imm64 = 0,
        .label_idx = 0
    });
}

/* xor rax, rbx */
static inline void ir_emit_xor(int temp_dest, int temp_src1, int temp_src2) {
    ir_emit((Instr){
        .op = OP_XOR,
        .temp_dest = (uint16_t)temp_dest,
        .temp_src1 = (uint16_t)temp_src1,
        .temp_src2 = (uint16_t)temp_src2,
        .cond = 0,
        .flags_out = 0xFF,  /* Doesn't set flags */
        .flags_in = 0xFF,   /* Doesn't read flags */
        .imm = 0,
        .imm64 = 0,
        .label_idx = 0
    });
}

/* shl rax, rbx */
static inline void ir_emit_shl(int temp_dest, int temp_src1, int temp_src2) {
    ir_emit((Instr){
        .op = OP_SHL,
        .temp_dest = (uint16_t)temp_dest,
        .temp_src1 = (uint16_t)temp_src1,
        .temp_src2 = (uint16_t)temp_src2,
        .cond = 0,
        .flags_out = 0xFF,  /* Doesn't set flags */
        .flags_in = 0xFF,   /* Doesn't read flags */
        .imm = 0,
        .imm64 = 0,
        .label_idx = 0
    });
}

/* shr rax, rbx */
static inline void ir_emit_shr(int temp_dest, int temp_src1, int temp_src2) {
    ir_emit((Instr){
        .op = OP_SHR,
        .temp_dest = (uint16_t)temp_dest,
        .temp_src1 = (uint16_t)temp_src1,
        .temp_src2 = (uint16_t)temp_src2,
        .cond = 0,
        .flags_out = 0xFF,  /* Doesn't set flags */
        .flags_in = 0xFF,   /* Doesn't read flags */
        .imm = 0,
        .imm64 = 0,
        .label_idx = 0
    });
}

/* sar rax, rbx (arithmetic right shift, signed >>) */
static inline void ir_emit_sar(int temp_dest, int temp_src1, int temp_src2) {
    ir_emit((Instr){
        .op = OP_SAR,
        .temp_dest = (uint16_t)temp_dest,
        .temp_src1 = (uint16_t)temp_src1,
        .temp_src2 = (uint16_t)temp_src2,
        .cond = 0,
        .flags_out = 0xFF,  /* Doesn't set flags */
        .flags_in = 0xFF,   /* Doesn't read flags */
        .imm = 0,
        .imm64 = 0,
        .label_idx = 0
    });
}

/* =========================================================================
 * Comparison Operations
 * ========================================================================= */

/* cmp rax, rbx */
static inline void ir_emit_cmp(int temp_src1, int temp_src2) {
    ir_emit((Instr){
        .op = OP_CMP,
        .temp_dest= TEMP_NONE,
        .temp_src1 = (uint16_t)temp_src1,
        .temp_src2 = (uint16_t)temp_src2,
        .cond = 0,
        .flags_out = REG_FLAGS,  /* Explicitly sets flags */
        .flags_in = 0xFF,         /* Doesn't read flags */
        .imm = 0,
        .imm64 = 0,
        .label_idx = 0
    });
}

/* cmp and set dst based on condition (ISA-neutral) */
static inline void ir_emit_cond_set(int temp_dest, int cond) {
    ir_emit((Instr){
        .op = OP_COND_SET,
        .temp_dest = (uint16_t)temp_dest,
        .temp_src1= TEMP_NONE,
        .temp_src2= TEMP_NONE,
        .cond = (uint8_t)cond,
        .flags_out = 0xFF,        /* Doesn't set flags */
        .flags_in = REG_FLAGS,     /* Explicitly reads flags */
        .imm = 0,
        .imm64 = 0,
        .label_idx = 0
    });
}

/* deref: mov rax, [rax] */
static inline void ir_emit_deref(int temp_dest, int temp_src) {
    ir_emit((Instr){
        .op = OP_DEREF,
        .temp_dest = (uint16_t)temp_dest,
        .temp_src1 = (uint16_t)temp_src,
        .temp_src2= TEMP_NONE,
        .cond = 0,
        .flags_out = 0xFF,  /* Doesn't set flags */
        .flags_in = 0xFF,   /* Doesn't read flags */
        .imm = 0,
        .imm64 = 0,
        .label_idx = 0
    });
}

/* load_byte: movzx dst, byte ptr [src] */
static inline void ir_emit_load_byte(int temp_dest, int temp_src) {
    ir_emit((Instr){
        .op = OP_LOAD_BYTE,
        .temp_dest = (uint16_t)temp_dest,
        .temp_src1 = (uint16_t)temp_src,
        .temp_src2= TEMP_NONE,
        .cond = 0,
        .flags_out = 0xFF,  /* Doesn't set flags */
        .flags_in = 0xFF,   /* Doesn't read flags */
        .imm = 0,
        .imm64 = 0,
        .label_idx = 0
    });
}

/* cmp rax, 0 */
static inline void ir_emit_cmp_zero(int temp_src) {
    ir_emit((Instr){
        .op = OP_CMP_ZERO,
        .temp_dest= TEMP_NONE,
        .temp_src1 = (uint16_t)temp_src,
        .temp_src2= TEMP_NONE,
        .cond = 0,
        .flags_out = REG_FLAGS,  /* Explicitly sets flags */
        .flags_in = 0xFF,         /* Doesn't read flags */
        .imm = 0,
        .imm64 = 0,
        .label_idx = 0
    });
}

/* not rax */
static inline void ir_emit_not(int temp_dest, int temp_src) {
    ir_emit((Instr){
        .op = OP_NOT,
        .temp_dest = (uint16_t)temp_dest,
        .temp_src1 = (uint16_t)temp_src,
        .temp_src2= TEMP_NONE,
        .cond = 0,
        .flags_out = 0xFF,  /* Doesn't set flags */
        .flags_in = 0xFF,   /* Doesn't read flags */
        .imm = 0,
        .imm64 = 0,
        .label_idx = 0
    });
}

/* lnot: !rax (test + sete + movzx) */
static inline void ir_emit_lnot(int temp_dest, int temp_src) {
    ir_emit((Instr){
        .op = OP_LNOT,
        .temp_dest = (uint16_t)temp_dest,
        .temp_src1 = (uint16_t)temp_src,
        .temp_src2= TEMP_NONE,
        .cond = 0,
        .flags_out = 0xFF,  /* Doesn't set flags */
        .flags_in = 0xFF,   /* Doesn't read flags */
        .imm = 0,
        .imm64 = 0,
        .label_idx = 0
    });
}


#endif /* ZERO_CC_PARSER_IR_EMIT_OPS_H */
