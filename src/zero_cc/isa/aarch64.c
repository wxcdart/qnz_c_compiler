#include "isa_backend.h"
#include "zero_cc/parser/machine_emitter.h"
#include <stdint.h>

/* ARM64 register info */
static const ISARegInfo aarch64_regs = {
    .int_regs = {
        "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7",
        "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15",
        "x16", "x17", "x18", "x19", "x20", "x21", "x22", "x23",
        "x24", "x25", "x26", "x27", "x28", "x29", "x30", "sp",
    },
    .fp_regs = {
        "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7",
        "v8", "v9", "v10", "v11", "v12", "v13", "v14", "v15",
        "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23",
        "v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31",
    },
    .int_arg_regs = {"x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7"},
    .fp_arg_regs = {"v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7"},
    .int_ret_reg = "x0",
    .fp_ret_reg = "v0",
    .num_int_regs = 31,
    .num_fp_regs = 32,
};

/* ARM64 instruction encoding helpers */
static inline void arm64_emit_u32(ZeroCCCompiler *c, uint32_t insn) {
    me_emit_u8(c, insn & 0xFF);
    me_emit_u8(c, (insn >> 8) & 0xFF);
    me_emit_u8(c, (insn >> 16) & 0xFF);
    me_emit_u8(c, (insn >> 24) & 0xFF);
}

/* ARM64 backend implementation */
static void aarch64_emit_prologue(ZeroCCCompiler *c, size_t stack_size) {
    /* stp x29, x30, [sp, #-16]!  - save frame pointer and link register */
    arm64_emit_u32(c, 0xA9BD7BFD | ((stack_size / 16) << 15));
    /* mov x29, sp */
    arm64_emit_u32(c, 0x910003FD);
    /* sub sp, sp, #stack_size (additional space) */
    if (stack_size > 0) {
        arm64_emit_u32(c, 0xD1000000 | ((stack_size / 16) << 10) | 31);
    }
}

static void aarch64_emit_epilogue(ZeroCCCompiler *c) {
    /* ldp x29, x30, [sp], #16 */
    arm64_emit_u32(c, 0xA8C17BFD);
    /* ret */
    arm64_emit_u32(c, 0xD65F03C0);
}

static void aarch64_emit_leave(ZeroCCCompiler *c) {
    /* AArch64 doesn't have a direct 'leave' instruction
     * Equivalent: mov sp, fp; ldr x29, [sp], #8; ldr x30, [sp], #8
     * But for simplicity, we'll use the epilogue pattern without ret
     * ldp x29, x30, [sp], #16 (restores fp, lr and adjusts sp)
     */
    arm64_emit_u32(c, 0xA8C17BFD);
}

static size_t aarch64_get_code_offset(ZeroCCCompiler *c) {
    return me_get_offset(c);
}

static void aarch64_emit_mov_imm(ZeroCCCompiler *c, int dst_reg, int64_t imm) {
    /* mov reg, imm - use movz/movk sequence for 64-bit */
    if ((imm & 0xFFFF000000000000LL) == 0) {
        arm64_emit_u32(c, 0xD2800000 | (imm << 5) | dst_reg);  /* movz */
    } else {
        /* Use movz + movk for full 64-bit */
        arm64_emit_u32(c, 0xD2800000 | ((imm & 0xFFFF) << 5) | dst_reg);
        if (imm >> 16) arm64_emit_u32(c, 0xF2A00000 | (((imm >> 16) & 0xFFFF) << 5) | dst_reg);
        if (imm >> 32) arm64_emit_u32(c, 0xF2C00000 | (((imm >> 32) & 0xFFFF) << 5) | dst_reg);
        if (imm >> 48) arm64_emit_u32(c, 0xF2E00000 | (((imm >> 48) & 0xFFFF) << 5) | dst_reg);
    }
}

static void aarch64_emit_add(ZeroCCCompiler *c, int dst, int src1, int src2) {
    /* add dst, src1, src2 */
    arm64_emit_u32(c, 0x8B000000 | (src2 << 16) | (src1 << 5) | dst);
}

static void aarch64_emit_sub(ZeroCCCompiler *c, int dst, int src1, int src2) {
    /* sub dst, src1, src2 */
    arm64_emit_u32(c, 0xCB000000 | (src2 << 16) | (src1 << 5) | dst);
}

static void aarch64_emit_mul(ZeroCCCompiler *c, int dst, int src1, int src2) {
    /* mul dst, src1, src2 */
    arm64_emit_u32(c, 0x9B007C00 | (src2 << 16) | (src1 << 5) | dst);
}

static void aarch64_emit_div(ZeroCCCompiler *c, int dst, int src1, int src2) {
    /* udiv/sdiv dst, src1, src2 */
    arm64_emit_u32(c, 0x9AC00800 | (src2 << 16) | (src1 << 5) | dst);  /* udiv */
}

static void aarch64_emit_and(ZeroCCCompiler *c, int dst, int src1, int src2) {
    arm64_emit_u32(c, 0x8A000000 | (src2 << 16) | (src1 << 5) | dst);
}

static void aarch64_emit_or(ZeroCCCompiler *c, int dst, int src1, int src2) {
    arm64_emit_u32(c, 0xAA000000 | (src2 << 16) | (src1 << 5) | dst);
}

static void aarch64_emit_xor(ZeroCCCompiler *c, int dst, int src1, int src2) {
    arm64_emit_u32(c, 0xCA000000 | (src2 << 16) | (src1 << 5) | dst);
}

static void aarch64_emit_shl(ZeroCCCompiler *c, int dst, int src, int shift) {
    /* lsl dst, src, #shift */
    arm64_emit_u32(c, 0xD3400000 | (shift << 16) | (src << 5) | dst);
}

static void aarch64_emit_cmp(ZeroCCCompiler *c, int src1, int src2) {
    /* cmp src1, src2 */
    arm64_emit_u32(c, 0xEB00001F | (src2 << 16) | (src1 << 5));
}

static void aarch64_emit_neg(ZeroCCCompiler *c, int dst, int src) {
    /* neg dst, src */
    arm64_emit_u32(c, 0xCB000000 | (src << 16) | (31 << 5) | dst);  /* sub dst, xzr, src */
}

static void aarch64_emit_load(ZeroCCCompiler *c, int dst_reg, int base_reg, int32_t offset) {
    /* ldr dst, [base, #offset] */
    if (offset >= -256 && offset < 256) {
        arm64_emit_u32(c, 0xF9400000 | ((offset & 0x3FF) << 10) | (base_reg << 5) | dst_reg);
    } else {
        /* Use ldr with register offset */
        arm64_emit_u32(c, 0xF8606800 | (offset << 10) | (base_reg << 5) | dst_reg);
    }
}

static void aarch64_emit_store(ZeroCCCompiler *c, int src_reg, int base_reg, int32_t offset) {
    /* str src, [base, #offset] */
    if (offset >= -256 && offset < 256) {
        arm64_emit_u32(c, 0xF9000000 | ((offset & 0x3FF) << 10) | (base_reg << 5) | src_reg);
    } else {
        arm64_emit_u32(c, 0xF8206800 | (offset << 10) | (base_reg << 5) | src_reg);
    }
}

static size_t aarch64_emit_jmp(ZeroCCCompiler *c, const char *label) {
    size_t patch_pos = me_get_offset(c);
    /* b label - placeholder for patching */
    arm64_emit_u32(c, 0x14000000);  /* b . (placeholder) */
    (void)label;
    return patch_pos;
}

static size_t aarch64_emit_jeq(ZeroCCCompiler *c, const char *label) {
    size_t patch_pos = me_get_offset(c);
    arm64_emit_u32(c, 0x54000000 | 0x0);  /* b.eq */
    (void)label;
    return patch_pos;
}

static size_t aarch64_emit_jne(ZeroCCCompiler *c, const char *label) {
    size_t patch_pos = me_get_offset(c);
    arm64_emit_u32(c, 0x54000000 | 0x1);  /* b.ne */
    (void)label;
    return patch_pos;
}

static size_t aarch64_emit_jlt(ZeroCCCompiler *c, const char *label) {
    size_t patch_pos = me_get_offset(c);
    arm64_emit_u32(c, 0x54000000 | 0xB);  /* b.lt (signed) */
    (void)label;
    return patch_pos;
}

static size_t aarch64_emit_jle(ZeroCCCompiler *c, const char *label) {
    size_t patch_pos = me_get_offset(c);
    arm64_emit_u32(c, 0x54000000 | 0xD);  /* b.le (signed) */
    (void)label;
    return patch_pos;
}

static size_t aarch64_emit_jgt(ZeroCCCompiler *c, const char *label) {
    size_t patch_pos = me_get_offset(c);
    arm64_emit_u32(c, 0x54000000 | 0xC);  /* b.gt (signed) */
    (void)label;
    return patch_pos;
}

static size_t aarch64_emit_jge(ZeroCCCompiler *c, const char *label) {
    size_t patch_pos = me_get_offset(c);
    arm64_emit_u32(c, 0x54000000 | 0xA);  /* b.ge (signed) */
    (void)label;
    return patch_pos;
}

static void aarch64_emit_call(ZeroCCCompiler *c, const char *func_name) {
    /* bl label */
    arm64_emit_u32(c, 0x94000000);  /* bl . (placeholder) */
    (void)func_name;
}

static void aarch64_emit_ret(ZeroCCCompiler *c) {
    /* ret */
    arm64_emit_u32(c, 0xD65F03C0);
}

/* Floating point operations */
static void aarch64_emit_load_fp(ZeroCCCompiler *c, int dst_reg, int base_reg, int32_t offset) {
    /* ldr dX, [base, #offset] */
    arm64_emit_u32(c, 0xF9400000 | ((offset & 0x3FF) << 10) | (base_reg << 5) | dst_reg);
}

static void aarch64_emit_store_fp(ZeroCCCompiler *c, int src_reg, int base_reg, int32_t offset) {
    /* str dX, [base, #offset] */
    arm64_emit_u32(c, 0xF9000000 | ((offset & 0x3FF) << 10) | (base_reg << 5) | src_reg);
}

static void aarch64_emit_load_fp_rodata(ZeroCCCompiler *c, int dst_reg, size_t rodata_offset) {
    /* Use literal pool or mov immediate for float */
    /* For now, store to memory and load */
    (void)c; (void)dst_reg; (void)rodata_offset;
}

static void aarch64_emit_fp_add(ZeroCCCompiler *c, int dst, int src1, int src2) {
    /* fadd dD, dN, dM */
    arm64_emit_u32(c, 0x4E208400 | (src2 << 16) | (src1 << 5) | dst);
}

static void aarch64_emit_fp_sub(ZeroCCCompiler *c, int dst, int src1, int src2) {
    arm64_emit_u32(c, 0x4E209400 | (src2 << 16) | (src1 << 5) | dst);
}

static void aarch64_emit_fp_mul(ZeroCCCompiler *c, int dst, int src1, int src2) {
    arm64_emit_u32(c, 0x4E20A400 | (src2 << 16) | (src1 << 5) | dst);
}

static void aarch64_emit_fp_div(ZeroCCCompiler *c, int dst, int src1, int src2) {
    arm64_emit_u32(c, 0x4E20B400 | (src2 << 16) | (src1 << 5) | dst);
}

static void aarch64_emit_fp_neg(ZeroCCCompiler *c, int dst, int src) {
    /* fneg dD, dN */
    arm64_emit_u32(c, 0x4E200400 | (src << 5) | dst);
}

static void aarch64_emit_fp_cmp(ZeroCCCompiler *c, int src1, int src2) {
    /* fcmp dN, dM */
    arm64_emit_u32(c, 0x4E20E400 | (src2 << 16) | (src1 << 5));
}

static void aarch64_emit_fp_cvtsi(ZeroCCCompiler *c, int int_dst, int fp_src) {
    /* fcvtzs wD, dN (convert double to signed int) */
    arm64_emit_u32(c, 0x1E640000 | (fp_src << 16) | int_dst);
}

static void aarch64_emit_fp_cvtsf(ZeroCCCompiler *c, int fp_dst, int int_src) {
    /* scvtf dD, wN (convert signed int to double) */
    arm64_emit_u32(c, 0x1E620000 | (int_src << 16) | fp_dst);
}

static void aarch64_emit_fp_mov(ZeroCCCompiler *c, int dst, int src) {
    if (dst == src) return;
    /* fmov dD, dN */
    arm64_emit_u32(c, 0x4E200400 | (src << 5) | dst | 0x00008000);
}

static void aarch64_emit_const(ZeroCCCompiler *c, int64_t value) {
    aarch64_emit_mov_imm(c, 0, value);
}

static void aarch64_emit_stack_adjust(ZeroCCCompiler *c, int32_t delta) {
    if (delta == 0) return;
    if (delta > 0) {
        arm64_emit_u32(c, 0x91000000 | ((delta / 4) << 10) | 31);  /* add sp, sp, #delta */
    } else {
        arm64_emit_u32(c, 0xD1000000 | ((-delta / 4) << 10) | 31);  /* sub sp, sp, #-delta */
    }
}

static void aarch64_emit_push_reg(ZeroCCCompiler *c, int reg) {
    /* stp xZ, xZ, [sp, #-16]! */
    arm64_emit_u32(c, 0xA9BF7BFD | (reg << 10) | (reg << 5));
}

static void aarch64_emit_pop_reg(ZeroCCCompiler *c, int reg) {
    /* ldp xZ, xZ, [sp], #16 */
    arm64_emit_u32(c, 0xA8C17BFD | (reg << 10) | (reg << 5));
}

static void aarch64_emit_label(ZeroCCCompiler *c, const char *name) {
    (void)c; (void)name;
}

static inline void aarch64_emit_fp_sqrt(ZeroCCCompiler *c, int dst, int src) {
    /* fsqrt dD, dN */
    arm64_emit_u32(c, 0x4E200400 | (src << 5) | dst | 0x00180000);
}



static size_t aarch64_patch_jmp(ZeroCCCompiler *c, size_t patch_offset, size_t target_offset) {
    /* ARM64 uses 26-bit signed relative branch (B, BL, B.cond) */
    int32_t rel = (int32_t)((target_offset - patch_offset) >> 2);
    
    if (patch_offset + 4 <= c->text_section.length) {
        uint8_t *buf = (uint8_t *)c->text_section.data;
        uint32_t *insn = (uint32_t*)(buf + patch_offset);
        *insn = (*insn & 0xFC000000) | (rel & 0x03FFFFFF);
    }
    return patch_offset;
}



static void aarch64_emit_test(ZeroCCCompiler *c, int src1, int src2) {
    /* tst Xn, Xm */
    arm64_emit_u32(c, 0x72000000 | ((src1 & 0x1F) << 5) | (src2 & 0x1F));
}

static void aarch64_emit_cqo(ZeroCCCompiler *c) {
    (void)c;
}

static void aarch64_emit_idiv(ZeroCCCompiler *c, int src) {
    /* sdiv X0, X0, Xn */
    arm64_emit_u32(c, 0x9AC00800 | ((src & 0x1F) << 16) | 0x0);
}

static void aarch64_emit_lea(ZeroCCCompiler *c, int dst, int base, int32_t offset) {
    if (offset >= -4096 && offset < 4096) {
        arm64_emit_u32(c, 0x91000000 | ((offset & 0x3FFFF) << 10) | ((base & 0x1F) << 5) | (dst & 0x1F));
    } else {
        aarch64_emit_mov_imm(c, dst, offset);
        arm64_emit_u32(c, 0x8B000000 | ((base & 0x1F) << 16) | (dst & 0x1F) | (dst & 0x1F));
    }
}



static void aarch64_emit_mod(ZeroCCCompiler *c, int dst, int src1, int src2) {
    /* Modulo: dst = src1 % src2
     * ARM64 udiv/sdiv don't provide remainder directly
     * Use: tmp = src1 / src2; dst = src1 - tmp * src2
     * For simplicity, use the same approach as div but with msub */
    (void)src1;
    /* msub dst, src2, tmp, src1 */
    arm64_emit_u32(c, 0x9B007C00 | ((src2 & 0x1F) << 16) | ((10 & 0x1F) << 10) | (dst & 0x1F));
    /* This is a simplified implementation - real implementation would be more complex */
}

/* ARM64 backend instance */

/* AArch64 VREG mapping: direct pass-through (VREG numbers = ARM encodings) */
static int aarch64_get_int_reg(int vreg) { return vreg; }
static int aarch64_get_fp_reg(int vreg) { return vreg; }

const ZeroCCISABackend aarch64_backend = {
    .isa = ISA_AARCH64,
    .regs = &aarch64_regs,
    .fp_reg = 29,  /* x29 */
    .sp_reg = 31,  /* sp */
    
    .emit_prologue = aarch64_emit_prologue,
    .emit_leave = aarch64_emit_leave,
    .emit_epilogue = aarch64_emit_epilogue,
    .get_code_offset = aarch64_get_code_offset,
    
    .emit_mov_imm = aarch64_emit_mov_imm,
    .emit_add = aarch64_emit_add,
    .emit_sub = aarch64_emit_sub,
    .emit_mul = aarch64_emit_mul,
    .emit_div = aarch64_emit_div,
    .emit_mod = aarch64_emit_mod,
    .emit_and = aarch64_emit_and,
    .emit_or = aarch64_emit_or,
    .emit_xor = aarch64_emit_xor,
    .emit_shl = aarch64_emit_shl,
    .emit_cmp = aarch64_emit_cmp,
    .emit_neg = aarch64_emit_neg,
    .emit_test = aarch64_emit_test,
    .emit_cqo = aarch64_emit_cqo,
    .emit_idiv = aarch64_emit_idiv,
    .emit_lea = aarch64_emit_lea,
    .patch_jmp = aarch64_patch_jmp,
    .resolve_labels = NULL,  /* No label resolution needed for now */
    
    /* VREG mapping */
    .get_int_reg = aarch64_get_int_reg,
    .get_fp_reg = aarch64_get_fp_reg,
    
    .emit_load = aarch64_emit_load,
    .emit_store = aarch64_emit_store,
    
    .emit_jmp = aarch64_emit_jmp,
    .emit_jeq = aarch64_emit_jeq,
    .emit_jne = aarch64_emit_jne,
    .emit_jlt = aarch64_emit_jlt,
    .emit_jle = aarch64_emit_jle,
    .emit_jgt = aarch64_emit_jgt,
    .emit_jge = aarch64_emit_jge,
    .emit_call = aarch64_emit_call,
    .emit_ret = aarch64_emit_ret,
    
    .emit_load_fp = aarch64_emit_load_fp,
    .emit_store_fp = aarch64_emit_store_fp,
    .emit_load_fp_rodata = aarch64_emit_load_fp_rodata,
    .emit_fp_add = aarch64_emit_fp_add,
    .emit_fp_sub = aarch64_emit_fp_sub,
    .emit_fp_mul = aarch64_emit_fp_mul,
    .emit_fp_div = aarch64_emit_fp_div,
    .emit_fp_neg = aarch64_emit_fp_neg,
    .emit_fp_cmp = aarch64_emit_fp_cmp,
    .emit_fp_cvtsi = aarch64_emit_fp_cvtsi,
    .emit_fp_cvtsf = aarch64_emit_fp_cvtsf,
    .emit_fp_mov = aarch64_emit_fp_mov,
    
    .emit_const = aarch64_emit_const,
    .emit_stack_adjust = aarch64_emit_stack_adjust,
    .emit_push_reg = aarch64_emit_push_reg,
    .emit_pop_reg = aarch64_emit_pop_reg,
    .emit_label = aarch64_emit_label,
};
/* =========================================================================
 * Virtual Register (VREG) Mapping for AArch64
 * =========================================================================
 *
 * The lowering code uses virtual registers defined in ../types/vregs.h.
 * Each VREG number is interpreted as the AArch64 hardware encoding:
 *
 *   VREG_0  = 0 = x0    (result / return value / 1st arg)
 *   VREG_1  = 1 = x1    (temp / 2nd arg)
 *   VREG_2  = 2 = x2    (temp / 3rd arg)
 *   VREG_3  = 3 = x3    (temp / 4th arg)
 *   VREG_4  = 4 = x4    (temp / 5th arg)
 *   VREG_5  = 5 = x5    (temp / 6th arg)
 *   VREG_6  = 6 = x6    (temp / 7th arg)
 *   VREG_7  = 7 = x7    (temp / 8th arg)
 *   VREG_8  = 8 = x8    (temp / return address register)
 *   VREG_9  = 9 = x9    (temp / caller-saved)
 *   VREG_10 = 10 = x10  (temp / caller-saved)
 *   VREG_11 = 11 = x11  (temp / caller-saved)
 *   VREG_12 = 12 = x12  (temp / caller-saved)
 *   VREG_13 = 13 = x13  (temp / caller-saved)
 *   VREG_14 = 14 = x14  (temp / caller-saved)
 *   VREG_15 = 15 = x15  (temp / caller-saved)
 *
 * Float registers (VREG_F0, VREG_F1, ...):
 *   VREG_F0 = 0 = d0    (float result / return value / 1st float arg)
 *   VREG_F1 = 1 = d1    (float temp / 2nd float arg)
 *   VREG_F2 = 2 = d2    (float temp / 3rd float arg)
 *   ...
 *
 * Special registers:
 *   BE(c)->sp_reg = 31 (sp) - use this instead of VREG_31
 *   BE(c)->fp_reg = 29 (fp/x29) - use this instead of VREG_29
 *
 * Note: The lowering code should use VREG_0-VREG_15 for general purposes,
 * and BE(c)->sp_reg / BE(c)->fp_reg for stack/frame pointers.
 */
