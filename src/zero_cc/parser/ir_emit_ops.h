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
        .op = OP_MOV_MEM_RAX,
        .temp_dest = 0xFF,
        .temp_src1 = (uint8_t)temp_src,
        .temp_src2 = 0xFF,  /* Frame pointer marker */
        .imm = offset,
        .imm64 = 0,
        .label_idx = 0
    });
}

/* mov rax, [rbp - offset] (load) */
static inline void ir_emit_load(int temp_dest, int fp_reg, int offset) {
    (void)fp_reg;  /* Frame pointer is fixed, not a temp */
    ir_emit((Instr){
        .op = OP_MOV_RAX_MEM,
        .temp_dest = (uint8_t)temp_dest,
        .temp_src1 = 0xFF,  /* Frame pointer marker */
        .temp_src2 = 0xFF,
        .imm = offset,
        .imm64 = 0,
        .label_idx = 0
    });
}

/* mov rax, rbx (register move) */
static inline void ir_emit_mov_reg(int temp_dest, int temp_src) {
    ir_emit((Instr){
        .op = OP_MOV_RAX_RBX,
        .temp_dest = (uint8_t)temp_dest,
        .temp_src1 = (uint8_t)temp_src,
        .temp_src2 = 0xFF,
        .imm = 0,
        .imm64 = 0,
        .label_idx = 0
    });
}

/* lea rax, [rbp - offset] (address) */
static inline void ir_emit_lea(int temp_dest, int fp_reg, int offset) {
    ir_emit((Instr){
        .op = OP_LEA_RAX_MEM,
        .temp_dest = (uint8_t)temp_dest,
        .temp_src1 = (uint8_t)fp_reg,
        .temp_src2 = 0xFF,
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
        .temp_dest = (uint8_t)temp_dest,
        .temp_src1 = 0xFF,
        .temp_src2 = 0xFF,
        .imm = imm,
        .imm64 = 0,
        .label_idx = 0
    });
}


/* sub rax, imm */
static inline void ir_emit_sub_imm(int temp_dest, int16_t imm) {
    ir_emit((Instr){
        .op = OP_SUB_IMM,
        .temp_dest = (uint8_t)temp_dest,
        .temp_src1 = 0xFF,
        .temp_src2 = 0xFF,
        .imm = imm,
        .imm64 = 0,
        .label_idx = 0
    });
}

/* div rbx */
static inline void ir_emit_div(int temp_dest, int temp_src1, int temp_src2) {
    ir_emit((Instr){
        .op = OP_DIV,
        .temp_dest = (uint8_t)temp_dest,
        .temp_src1 = (uint8_t)temp_src1,
        .temp_src2 = (uint8_t)temp_src2,
        .imm = 0,
        .imm64 = 0,
        .label_idx = 0
    });
}

/* mod rbx */
static inline void ir_emit_mod(int temp_dest, int temp_src1, int temp_src2) {
    ir_emit((Instr){
        .op = OP_MOD,
        .temp_dest = (uint8_t)temp_dest,
        .temp_src1 = (uint8_t)temp_src1,
        .temp_src2 = (uint8_t)temp_src2,
        .imm = 0,
        .imm64 = 0,
        .label_idx = 0
    });
}

/* neg rax */
static inline void ir_emit_neg(int temp_dest, int temp_src) {
    ir_emit((Instr){
        .op = OP_NEG,
        .temp_dest = (uint8_t)temp_dest,
        .temp_src1 = (uint8_t)temp_src,
        .temp_src2 = 0xFF,
        .imm = 0,
        .imm64 = 0,
        .label_idx = 0
    });
}

/* inc rax */
static inline void ir_emit_inc(int temp_dest) {
    ir_emit((Instr){
        .op = OP_INC,
        .temp_dest = (uint8_t)temp_dest,
        .temp_src1 = 0xFF,
        .temp_src2 = 0xFF,
        .imm = 0,
        .imm64 = 0,
        .label_idx = 0
    });
}

/* dec rax */
static inline void ir_emit_dec(int temp_dest) {
    ir_emit((Instr){
        .op = OP_DEC,
        .temp_dest = (uint8_t)temp_dest,
        .temp_src1 = 0xFF,
        .temp_src2 = 0xFF,
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
        .temp_dest = (uint8_t)temp_dest,
        .temp_src1 = (uint8_t)temp_src1,
        .temp_src2 = (uint8_t)temp_src2,
        .imm = 0,
        .imm64 = 0,
        .label_idx = 0
    });
}

/* or rax, rbx */
static inline void ir_emit_or(int temp_dest, int temp_src1, int temp_src2) {
    ir_emit((Instr){
        .op = OP_OR,
        .temp_dest = (uint8_t)temp_dest,
        .temp_src1 = (uint8_t)temp_src1,
        .temp_src2 = (uint8_t)temp_src2,
        .imm = 0,
        .imm64 = 0,
        .label_idx = 0
    });
}

/* xor rax, rbx */
static inline void ir_emit_xor(int temp_dest, int temp_src1, int temp_src2) {
    ir_emit((Instr){
        .op = OP_XOR,
        .temp_dest = (uint8_t)temp_dest,
        .temp_src1 = (uint8_t)temp_src1,
        .temp_src2 = (uint8_t)temp_src2,
        .imm = 0,
        .imm64 = 0,
        .label_idx = 0
    });
}

/* shl rax, rbx */
static inline void ir_emit_shl(int temp_dest, int temp_src1, int temp_src2) {
    ir_emit((Instr){
        .op = OP_SHL,
        .temp_dest = (uint8_t)temp_dest,
        .temp_src1 = (uint8_t)temp_src1,
        .temp_src2 = (uint8_t)temp_src2,
        .imm = 0,
        .imm64 = 0,
        .label_idx = 0
    });
}

/* shr rax, rbx */
static inline void ir_emit_shr(int temp_dest, int temp_src1, int temp_src2) {
    ir_emit((Instr){
        .op = OP_SHR,
        .temp_dest = (uint8_t)temp_dest,
        .temp_src1 = (uint8_t)temp_src1,
        .temp_src2 = (uint8_t)temp_src2,
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
        .temp_dest = 0xFF,
        .temp_src1 = (uint8_t)temp_src1,
        .temp_src2 = (uint8_t)temp_src2,
        .imm = 0,
        .imm64 = 0,
        .label_idx = 0
    });
}

/* test rax, rax */
static inline void ir_emit_test(int temp_src) {
    ir_emit((Instr){
        .op = OP_TEST,
        .temp_dest = 0xFF,
        .temp_src1 = (uint8_t)temp_src,
        .temp_src2 = (uint8_t)temp_src,
        .imm = 0,
        .imm64 = 0,
        .label_idx = 0
    });
}

/* setl al */
static inline void ir_emit_setl(int temp_dest) {
    ir_emit((Instr){
        .op = OP_SETL,
        .temp_dest = (uint8_t)temp_dest,
        .temp_src1 = 0xFF,
        .temp_src2 = 0xFF,
        .imm = 0,
        .imm64 = 0,
        .label_idx = 0
    });
}

/* setle al */
static inline void ir_emit_setle(int temp_dest) {
    ir_emit((Instr){
        .op = OP_SETLE,
        .temp_dest = (uint8_t)temp_dest,
        .temp_src1 = 0xFF,
        .temp_src2 = 0xFF,
        .imm = 0,
        .imm64 = 0,
        .label_idx = 0
    });
}

/* setg al */
static inline void ir_emit_setg(int temp_dest) {
    ir_emit((Instr){
        .op = OP_SETG,
        .temp_dest = (uint8_t)temp_dest,
        .temp_src1 = 0xFF,
        .temp_src2 = 0xFF,
        .imm = 0,
        .imm64 = 0,
        .label_idx = 0
    });
}

/* setge al */
static inline void ir_emit_setge(int temp_dest) {
    ir_emit((Instr){
        .op = OP_SETGE,
        .temp_dest = (uint8_t)temp_dest,
        .temp_src1 = 0xFF,
        .temp_src2 = 0xFF,
        .imm = 0,
        .imm64 = 0,
        .label_idx = 0
    });
}

/* sete al */
static inline void ir_emit_sete(int temp_dest) {
    ir_emit((Instr){
        .op = OP_SET_E,
        .temp_dest = (uint8_t)temp_dest,
        .temp_src1 = 0xFF,
        .temp_src2 = 0xFF,
        .imm = 0,
        .imm64 = 0,
        .label_idx = 0
    });
}

/* setne al */
static inline void ir_emit_setne(int temp_dest) {
    ir_emit((Instr){
        .op = OP_SET_NE,
        .temp_dest = (uint8_t)temp_dest,
        .temp_src1 = 0xFF,
        .temp_src2 = 0xFF,
        .imm = 0,
        .imm64 = 0,
        .label_idx = 0
    });
}

/* deref: mov rax, [rax] */
static inline void ir_emit_deref(int temp_dest, int temp_src) {
    ir_emit((Instr){
        .op = OP_DEREF,
        .temp_dest = (uint8_t)temp_dest,
        .temp_src1 = (uint8_t)temp_src,
        .temp_src2 = 0xFF,
        .imm = 0,
        .imm64 = 0,
        .label_idx = 0
    });
}

/* load_byte: movzx dst, byte ptr [src] */
static inline void ir_emit_load_byte(int temp_dest, int temp_src) {
    ir_emit((Instr){
        .op = OP_LOAD_BYTE,
        .temp_dest = (uint8_t)temp_dest,
        .temp_src1 = (uint8_t)temp_src,
        .temp_src2 = 0xFF,
        .imm = 0,
        .imm64 = 0,
        .label_idx = 0
    });
}

/* cmp rax, 0 */
static inline void ir_emit_cmp_zero(int temp_src) {
    ir_emit((Instr){
        .op = OP_CMP_ZERO,
        .temp_dest = 0xFF,
        .temp_src1 = (uint8_t)temp_src,
        .temp_src2 = 0xFF,
        .imm = 0,
        .imm64 = 0,
        .label_idx = 0
    });
}

/* not rax */
static inline void ir_emit_not(int temp_dest, int temp_src) {
    ir_emit((Instr){
        .op = OP_NOT,
        .temp_dest = (uint8_t)temp_dest,
        .temp_src1 = (uint8_t)temp_src,
        .temp_src2 = 0xFF,
        .imm = 0,
        .imm64 = 0,
        .label_idx = 0
    });
}

/* lnot: !rax (test + sete + movzx) */
static inline void ir_emit_lnot(int temp_dest, int temp_src) {
    ir_emit((Instr){
        .op = OP_LNOT,
        .temp_dest = (uint8_t)temp_dest,
        .temp_src1 = (uint8_t)temp_src,
        .temp_src2 = 0xFF,
        .imm = 0,
        .imm64 = 0,
        .label_idx = 0
    });
}

/* cvttsd2si: convert float/double (in xmm0 or temp_src) to int (in temp_dest) */
static inline void ir_emit_cvttsd2si(int temp_dest, int temp_src) {
    ir_emit((Instr){
        .op = OP_CVTTSD2SI,
        .temp_dest = (uint8_t)temp_dest,
        .temp_src1 = (uint8_t)temp_src,
        .temp_src2 = 0xFF,
        .imm = 0,
        .imm64 = 0,
        .label_idx = 0
    });
}

#endif /* ZERO_CC_PARSER_IR_EMIT_OPS_H */
