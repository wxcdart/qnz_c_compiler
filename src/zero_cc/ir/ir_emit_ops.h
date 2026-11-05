/* IR Emitter - Operation Emission Functions
 *
 * Additional IR emission functions beyond those in ir.h
 * Provides more complete coverage for IR-based codegen.
 */

#ifndef ZERO_CC_PARSER_IR_EMIT_OPS_H
#define ZERO_CC_PARSER_IR_EMIT_OPS_H

#include "zero_cc/ir/ir.h"

/* =========================================================================
 * Data Movement Operations
 * ========================================================================= */

/* mov [rbp - offset], rax (store) */
static inline void ir_emit_store(ZeroCCCompiler *c, int temp_src, int fp_reg, int offset) {
    (void)fp_reg;  /* Frame pointer is fixed, not a temp */
    ir_emit(c, (Instr){
        .op = OP_MOV_MEM,
        .temp_dest= TEMP_NONE,
        .temp_src1 = (uint16_t)temp_src,
        .temp_src2= TEMP_NONE,  /* Frame pointer marker */
        .cond = 0,
        .flags_out = 0xFF,  /* Doesn't set flags */
        .flags_in = 0xFF,   /* Doesn't read flags */
        .imm = (int16_t)offset,
        .imm64 = offset,
        .label_idx = 0
    });
}

/* mov rax, [rbp - offset] (load) */
static inline void ir_emit_load(ZeroCCCompiler *c, int temp_dest, int fp_reg, int offset) {
    (void)fp_reg;  /* Frame pointer is fixed, not a temp */
    ir_emit(c, (Instr){
        .op = OP_MOV,
        .temp_dest = (uint16_t)temp_dest,
        .temp_src1= TEMP_NONE,  /* Frame pointer marker */
        .temp_src2= TEMP_NONE,
        .cond = 0,
        .flags_out = 0xFF,  /* Doesn't set flags */
        .flags_in = 0xFF,   /* Doesn't read flags */
        .imm = (int16_t)offset,
        .imm64 = offset,
        .label_idx = 0
    });
}

/* mov rax, rbx (register move) */
static inline void ir_emit_mov_reg(ZeroCCCompiler *c, int temp_dest, int temp_src) {
    ir_emit(c, (Instr){
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
static inline void ir_emit_sext(ZeroCCCompiler *c, int temp, int width) {
    Opcode op = (width == 4) ? OP_SEXT32 : (width == 2) ? OP_SEXT16 : OP_SEXT8;
    ir_emit(c, (Instr){
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

static inline void ir_emit_zext(ZeroCCCompiler *c, int temp, int width) {
    Opcode op = (width == 4) ? OP_ZEXT32 : (width == 2) ? OP_ZEXT16 : OP_ZEXT8;
    ir_emit(c, (Instr){
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
static inline void ir_emit_lea(ZeroCCCompiler *c, int temp_dest, int fp_reg, int offset) {
    (void)fp_reg;  /* Frame pointer is fixed, not a temp */
    ir_emit(c, (Instr){
        .op = OP_LEA,
        .temp_dest = (uint16_t)temp_dest,
        .temp_src1= TEMP_NONE,
        .temp_src2= TEMP_NONE,
        .cond = 0,
        .flags_out = 0xFF,  /* Doesn't set flags */
        .flags_in = 0xFF,   /* Doesn't read flags */
        .imm = (int16_t)offset,
        .imm64 = offset,
        .label_idx = 0
    });
}

/* =========================================================================
 * Arithmetic Operations
 * ========================================================================= */

/* add rax, imm */
static inline void ir_emit_add_imm(ZeroCCCompiler *c, int temp_dest, int16_t imm) {
    ir_emit(c, (Instr){
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
static inline void ir_emit_sub_imm(ZeroCCCompiler *c, int temp_dest, int16_t imm) {
    ir_emit(c, (Instr){
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
static inline void ir_emit_div(ZeroCCCompiler *c, int temp_dest, int temp_src1, int temp_src2) {
    ir_emit(c, (Instr){
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
static inline void ir_emit_mod(ZeroCCCompiler *c, int temp_dest, int temp_src1, int temp_src2) {
    ir_emit(c, (Instr){
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
static inline void ir_emit_neg(ZeroCCCompiler *c, int temp_dest, int temp_src) {
    ir_emit(c, (Instr){
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
static inline void ir_emit_inc(ZeroCCCompiler *c, int temp_dest) {
    ir_emit(c, (Instr){
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
static inline void ir_emit_dec(ZeroCCCompiler *c, int temp_dest) {
    ir_emit(c, (Instr){
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
static inline void ir_emit_and(ZeroCCCompiler *c, int temp_dest, int temp_src1, int temp_src2) {
    ir_emit(c, (Instr){
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
static inline void ir_emit_or(ZeroCCCompiler *c, int temp_dest, int temp_src1, int temp_src2) {
    ir_emit(c, (Instr){
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
static inline void ir_emit_xor(ZeroCCCompiler *c, int temp_dest, int temp_src1, int temp_src2) {
    ir_emit(c, (Instr){
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
static inline void ir_emit_shl(ZeroCCCompiler *c, int temp_dest, int temp_src1, int temp_src2) {
    ir_emit(c, (Instr){
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
static inline void ir_emit_shr(ZeroCCCompiler *c, int temp_dest, int temp_src1, int temp_src2) {
    ir_emit(c, (Instr){
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
static inline void ir_emit_sar(ZeroCCCompiler *c, int temp_dest, int temp_src1, int temp_src2) {
    ir_emit(c, (Instr){
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
static inline void ir_emit_cmp(ZeroCCCompiler *c, int temp_src1, int temp_src2) {
    ir_emit(c, (Instr){
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
static inline void ir_emit_cond_set(ZeroCCCompiler *c, int temp_dest, int cond) {
    ir_emit(c, (Instr){
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
static inline void ir_emit_deref(ZeroCCCompiler *c, int temp_dest, int temp_src) {
    ir_emit(c, (Instr){
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
static inline void ir_emit_load_byte(ZeroCCCompiler *c, int temp_dest, int temp_src) {
    ir_emit(c, (Instr){
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
static inline void ir_emit_cmp_zero(ZeroCCCompiler *c, int temp_src) {
    ir_emit(c, (Instr){
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
static inline void ir_emit_not(ZeroCCCompiler *c, int temp_dest, int temp_src) {
    ir_emit(c, (Instr){
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
static inline void ir_emit_lnot(ZeroCCCompiler *c, int temp_dest, int temp_src) {
    ir_emit(c, (Instr){
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


/* =========================================================================
 * GNU __int128 (Sprint 109): 128-bit integer ops on a (lo,hi) GPR128 temp.
 * A 128-bit value lives in ONE temp classified REG_CLASS_GPR128; its low
 * half is the temp's normal register and its high half is the paired
 * register (regalloc_get_reg_hi_for_temp). Load/store are frame-relative
 * (like OP_MOV/OP_MOV_MEM) or register-base (like OP_DEREF).
 * ========================================================================= */
static inline void ir_emit_add128(ZeroCCCompiler *c, int d, int a, int b) {
    ir_emit(c, (Instr){.op = OP_ADD128, .temp_dest = (uint16_t)d, .temp_src1 = (uint16_t)a, .temp_src2 = (uint16_t)b, .cond = 0, .flags_out = 0xFF, .flags_in = 0xFF, .imm = 0, .imm64 = 0, .label_idx = 0});
}
static inline void ir_emit_sub128(ZeroCCCompiler *c, int d, int a, int b) {
    ir_emit(c, (Instr){.op = OP_SUB128, .temp_dest = (uint16_t)d, .temp_src1 = (uint16_t)a, .temp_src2 = (uint16_t)b, .cond = 0, .flags_out = 0xFF, .flags_in = 0xFF, .imm = 0, .imm64 = 0, .label_idx = 0});
}
static inline void ir_emit_mul128(ZeroCCCompiler *c, int d, int a, int b) {
    ir_emit(c, (Instr){.op = OP_MUL128, .temp_dest = (uint16_t)d, .temp_src1 = (uint16_t)a, .temp_src2 = (uint16_t)b, .cond = 0, .flags_out = 0xFF, .flags_in = 0xFF, .imm = 0, .imm64 = 0, .label_idx = 0});
}
static inline void ir_emit_and128(ZeroCCCompiler *c, int d, int a, int b) {
    ir_emit(c, (Instr){.op = OP_AND128, .temp_dest = (uint16_t)d, .temp_src1 = (uint16_t)a, .temp_src2 = (uint16_t)b, .cond = 0, .flags_out = 0xFF, .flags_in = 0xFF, .imm = 0, .imm64 = 0, .label_idx = 0});
}
static inline void ir_emit_or128(ZeroCCCompiler *c, int d, int a, int b) {
    ir_emit(c, (Instr){.op = OP_OR128, .temp_dest = (uint16_t)d, .temp_src1 = (uint16_t)a, .temp_src2 = (uint16_t)b, .cond = 0, .flags_out = 0xFF, .flags_in = 0xFF, .imm = 0, .imm64 = 0, .label_idx = 0});
}
static inline void ir_emit_xor128(ZeroCCCompiler *c, int d, int a, int b) {
    ir_emit(c, (Instr){.op = OP_XOR128, .temp_dest = (uint16_t)d, .temp_src1 = (uint16_t)a, .temp_src2 = (uint16_t)b, .cond = 0, .flags_out = 0xFF, .flags_in = 0xFF, .imm = 0, .imm64 = 0, .label_idx = 0});
}
static inline void ir_emit_not128(ZeroCCCompiler *c, int d, int a) {
    ir_emit(c, (Instr){.op = OP_NOT128, .temp_dest = (uint16_t)d, .temp_src1 = (uint16_t)a, .temp_src2 = TEMP_NONE, .cond = 0, .flags_out = 0xFF, .flags_in = 0xFF, .imm = 0, .imm64 = 0, .label_idx = 0});
}
static inline void ir_emit_neg128(ZeroCCCompiler *c, int d, int a) {
    ir_emit(c, (Instr){.op = OP_NEG128, .temp_dest = (uint16_t)d, .temp_src1 = (uint16_t)a, .temp_src2 = TEMP_NONE, .cond = 0, .flags_out = 0xFF, .flags_in = 0xFF, .imm = 0, .imm64 = 0, .label_idx = 0});
}
static inline void ir_emit_mov128(ZeroCCCompiler *c, int d, int a) {
    ir_emit(c, (Instr){.op = OP_MOV128, .temp_dest = (uint16_t)d, .temp_src1 = (uint16_t)a, .temp_src2 = TEMP_NONE, .cond = 0, .flags_out = 0xFF, .flags_in = 0xFF, .imm = 0, .imm64 = 0, .label_idx = 0});
}
static inline void ir_emit_mov128_imm(ZeroCCCompiler *c, int d, int64_t imm) {
    ir_emit(c, (Instr){.op = OP_MOV128_IMM, .temp_dest = (uint16_t)d, .temp_src1 = TEMP_NONE, .temp_src2 = TEMP_NONE, .cond = 0, .flags_out = 0xFF, .flags_in = 0xFF, .imm = 0, .imm64 = imm, .label_idx = 0});
}
/* Register-base 128-bit load / store: base is an ADDRESS temp (a register).
 * Locals: ir_emit_lea the frame offset first; globals: ir_emit_global_addr. */
static inline void ir_emit_load128(ZeroCCCompiler *c, int d, int base_addr) {
    ir_emit(c, (Instr){.op = OP_LOAD128, .temp_dest = (uint16_t)d, .temp_src1 = (uint16_t)base_addr, .temp_src2 = TEMP_NONE, .cond = 0, .flags_out = 0xFF, .flags_in = 0xFF, .imm = 0, .imm64 = 0, .label_idx = 0});
}
static inline void ir_emit_store128(ZeroCCCompiler *c, int s, int base_addr) {
    ir_emit(c, (Instr){.op = OP_STORE128, .temp_dest = TEMP_NONE, .temp_src1 = (uint16_t)s, .temp_src2 = (uint16_t)base_addr, .cond = 0, .flags_out = 0xFF, .flags_in = 0xFF, .imm = 0, .imm64 = 0, .label_idx = 0});
}
/* 64-bit <-> 128-bit conversions. zext/sext: 64-bit src -> 128-bit dest.
 * trunc: 128-bit src -> 64-bit dest (takes the low half). */
static inline void ir_emit_zext64_128(ZeroCCCompiler *c, int d, int a) {
    ir_emit(c, (Instr){.op = OP_ZEXT64_128, .temp_dest = (uint16_t)d, .temp_src1 = (uint16_t)a, .temp_src2 = TEMP_NONE, .cond = 0, .flags_out = 0xFF, .flags_in = 0xFF, .imm = 0, .imm64 = 0, .label_idx = 0});
}
static inline void ir_emit_sext64_128(ZeroCCCompiler *c, int d, int a) {
    ir_emit(c, (Instr){.op = OP_SEXT64_128, .temp_dest = (uint16_t)d, .temp_src1 = (uint16_t)a, .temp_src2 = TEMP_NONE, .cond = 0, .flags_out = 0xFF, .flags_in = 0xFF, .imm = 0, .imm64 = 0, .label_idx = 0});
}
static inline void ir_emit_trunc128_64(ZeroCCCompiler *c, int d, int a) {
    ir_emit(c, (Instr){.op = OP_TRUNC128_64, .temp_dest = (uint16_t)d, .temp_src1 = (uint16_t)a, .temp_src2 = TEMP_NONE, .cond = 0, .flags_out = 0xFF, .flags_in = 0xFF, .imm = 0, .imm64 = 0, .label_idx = 0});
}
/* Extract a half of a 128-bit value into a 64-bit temp (for compare/shift
 * decomposition). dest is a plain 64-bit temp; src1 is a GPR128 temp. */
static inline void ir_emit_lo128(ZeroCCCompiler *c, int d, int a) {
    ir_emit(c, (Instr){.op = OP_LO128, .temp_dest = (uint16_t)d, .temp_src1 = (uint16_t)a, .temp_src2 = TEMP_NONE, .cond = 0, .flags_out = 0xFF, .flags_in = 0xFF, .imm = 0, .imm64 = 0, .label_idx = 0});
}
static inline void ir_emit_hi128(ZeroCCCompiler *c, int d, int a) {
    ir_emit(c, (Instr){.op = OP_HI128, .temp_dest = (uint16_t)d, .temp_src1 = (uint16_t)a, .temp_src2 = TEMP_NONE, .cond = 0, .flags_out = 0xFF, .flags_in = 0xFF, .imm = 0, .imm64 = 0, .label_idx = 0});
}
/* Build a 128-bit value from two 64-bit halves: dest.lo = src1, dest.hi = src2. */
static inline void ir_emit_mk128(ZeroCCCompiler *c, int d, int lo, int hi) {
    ir_emit(c, (Instr){.op = OP_MK128, .temp_dest = (uint16_t)d, .temp_src1 = (uint16_t)lo, .temp_src2 = (uint16_t)hi, .cond = 0, .flags_out = 0xFF, .flags_in = 0xFF, .imm = 0, .imm64 = 0, .label_idx = 0});
}


#endif /* ZERO_CC_PARSER_IR_EMIT_OPS_H */
