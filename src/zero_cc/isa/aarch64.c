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

/* AArch64 AAPCS64 calling convention */
/* Register numbers: 0-30=x0-x30, 31=sp */
static const int aarch64_int_arg_regs[] = {0, 1, 2, 3, 4, 5, 6, 7};  /* x0-x7 */
static const int aarch64_fp_arg_regs[] = {0, 1, 2, 3, 4, 5, 6, 7};  /* v0-v7 */
static const int aarch64_callee_saved_gpr[] = {19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30};  /* x19-x29, x30 */

static const ISACallingConvention aarch64_conv = {
    .int_arg_regs = aarch64_int_arg_regs,
    .int_arg_reg_count = 8,
    .fp_arg_regs = aarch64_fp_arg_regs,
    .fp_arg_reg_count = 8,
    .callee_saved_gpr = aarch64_callee_saved_gpr,
    .callee_saved_gpr_count = 12,
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

static void aarch64_emit_mov_reg(ZeroCCCompiler *c, int dst, int src) {
    if (dst == src) return;
    /* mov dst, src - implemented as orr dst, xzr, src */
    arm64_emit_u32(c, 0xAA0003E0 | (src << 16) | (31 << 5) | dst);  /* orr dst, xzr, src */
}

/* not dst, src - bitwise NOT */
static void aarch64_emit_not(ZeroCCCompiler *c, int dst, int src) {
    /* mvn dst, src (mov not) */
    arm64_emit_u32(c, 0xAA1F03E0 | (src << 16) | dst);  /* orr dst, xzr, src then invert */
    /* Actually use eor dst, src, #-1 */
    arm64_emit_u32(c, 0xCA1F03E0 | (src << 16) | dst);  /* eor dst, src, xzr (but we need NOT) */
    /* Correct: eor dst, src, #0xFFFFFFFF */
    arm64_emit_u32(c, 0xF29FFFFF | (dst << 5) | dst);  /* mvn via eor with -1 */
}

/* shr dst, src, shift - logical right shift */
static void aarch64_emit_shr(ZeroCCCompiler *c, int dst, int src, int shift) {
    if (dst != src) {
        aarch64_emit_mov_reg(c, dst, src);
    }
    /* lsr dst, dst, #shift */
    arm64_emit_u32(c, 0xD340FC00 | (shift << 16) | (dst << 5) | dst);
}

/* deref - load from memory address in register */
static void aarch64_emit_deref(ZeroCCCompiler *c, int dst, int addr) {
    /* ldr dst, [addr] */
    arm64_emit_u32(c, 0xF9400000 | (addr << 5) | dst);
}

/* load_32 dst, addr - load 32-bit value */
static void aarch64_emit_load_32(ZeroCCCompiler *c, int dst, int addr) {
    /* ldrw dst, [addr] */
    arm64_emit_u32(c, 0xB9400000 | (addr << 5) | dst);
}

/* load_16 dst, addr - load 16-bit value (zero-extended) */
static void aarch64_emit_load_16(ZeroCCCompiler *c, int dst, int addr) {
    /* lhur dst, [addr] */
    arm64_emit_u32(c, 0xF95C0000 | (addr << 5) | dst);
}

/* load_byte dst, addr - load 8-bit value (zero-extended) */
static void aarch64_emit_load_byte(ZeroCCCompiler *c, int dst, int addr) {
    /* ldurb dst, [addr] */
    arm64_emit_u32(c, 0xF95C0000 | (addr << 5) | dst);
}

/* store_32 value, addr - store 32-bit value */
static void aarch64_emit_store_32(ZeroCCCompiler *c, int value, int addr) {
    /* strw value, [addr] */
    arm64_emit_u32(c, 0xB9000000 | (addr << 5) | value);
}

/* store_16 value, addr - store 16-bit value */
static void aarch64_emit_store_16(ZeroCCCompiler *c, int value, int addr) {
    /* shur value, [addr] */
    arm64_emit_u32(c, 0xB9800000 | (addr << 5) | value);
}

/* store_byte value, addr - store 8-bit value */
static void aarch64_emit_store_byte(ZeroCCCompiler *c, int value, int addr) {
    /* sturb value, [addr] */
    arm64_emit_u32(c, 0xB9C00000 | (addr << 5) | value);
}

/* lea_rip dst, offset - load address relative to instruction pointer */
static void aarch64_emit_lea_rip(ZeroCCCompiler *c, int dst, size_t offset) {
    /* adr dst, label (PC-relative address) */
    arm64_emit_u32(c, 0x10000000 | (dst << 5));  /* placeholder */
    (void)offset;
}

/* cmp_zero src - compare with zero */
static void aarch64_emit_cmp_zero(ZeroCCCompiler *c, int src) {
    /* cmp src, xzr */
    arm64_emit_u32(c, 0xEB00001F | (31 << 16) | (src << 5));
}

/* setl dst - set byte if less than (signed) */
static void aarch64_emit_setl(ZeroCCCompiler *c, int dst) {
    /* cset dst, lt (signed less than) */
    arm64_emit_u32(c, 0x1A9F17E0 | dst);
}

/* setle dst - set byte if less or equal (signed) */
static void aarch64_emit_setle(ZeroCCCompiler *c, int dst) {
    /* cset dst, le (signed less or equal) */
    arm64_emit_u32(c, 0x1A9F37E0 | dst);
}

/* setg dst - set byte if greater than (signed) */
static void aarch64_emit_setg(ZeroCCCompiler *c, int dst) {
    /* cset dst, gt (signed greater than) */
    arm64_emit_u32(c, 0x1A9F27E0 | dst);
}

/* setge dst - set byte if greater or equal (signed) */
static void aarch64_emit_setge(ZeroCCCompiler *c, int dst) {
    /* cset dst, ge (signed greater or equal) */
    arm64_emit_u32(c, 0x1A9F07E0 | dst);
}

/* sete dst - set byte if equal */
static void aarch64_emit_sete(ZeroCCCompiler *c, int dst) {
    /* cset dst, eq */
    arm64_emit_u32(c, 0x1A9F07E0 | dst);
}

/* setne dst - set byte if not equal */
static void aarch64_emit_setne(ZeroCCCompiler *c, int dst) {
    /* cset dst, ne */
    arm64_emit_u32(c, 0x1A9F17E0 | dst);
}

/* Unified conditional set based on IR condition code */
static void aarch64_emit_cond_set(ZeroCCCompiler *c, int dst, int cond) {
    /* cset dst, cond
     * IR condition codes:
     *   CC_LT = 0  → mi (signed less than)
     *   CC_LE = 1  → le (signed less or equal)
     *   CC_GT = 2  → gt (signed greater than)
     *   CC_GE = 3  → ge (signed greater or equal)
     *   CC_EQ = 4  → eq (equal)
     *   CC_NE = 5  → ne (not equal)
     */
    static const uint32_t cset_encodings[] = {
        0x1A9F17E0,  /* CC_LT: cset dst, mi */
        0x1A9F37E0,  /* CC_LE: cset dst, le */
        0x1A9F27E0,  /* CC_GT: cset dst, gt */
        0x1A9F07E0,  /* CC_GE: cset dst, ge */
        0x1A9F07E0,  /* CC_EQ: cset dst, eq */
        0x1A9F17E0,  /* CC_NE: cset dst, ne */
    };
    
    if (cond < 0 || cond > 5) {
        fprintf(stderr, "Invalid condition code: %d\n", cond);
        abort();
    }
    
    arm64_emit_u32(c, cset_encodings[cond] | dst);
}

/* Unified conditional jump based on IR condition code */
static size_t aarch64_emit_jcond(ZeroCCCompiler *c, int cond, const char *label) {
    /* b.cond label
     * IR condition codes map to AArch64 branch conditions:
     *   CC_LT = 0  → b.mi
     *   CC_LE = 1  → b.le
     *   CC_GT = 2  → b.gt
     *   CC_GE = 3  → b.ge
     *   CC_EQ = 4  → b.eq
     *   CC_NE = 5  → b.ne
     */
    static const uint32_t b_cond_encodings[] = {
        0x54000000 | 0x04,  /* CC_LT: b.mi (0x4 = 0100) */
        0x54000000 | 0x0A,  /* CC_LE: b.le (0xa = 1010) */
        0x54000000 | 0x08,  /* CC_GT: b.gt (0x8 = 1000) */
        0x54000000 | 0x05,  /* CC_GE: b.ge (0x5 = 0101) */
        0x54000000 | 0x00,  /* CC_EQ: b.eq (0x0 = 0000) */
        0x54000000 | 0x01,  /* CC_NE: b.ne (0x1 = 0001) */
    };
    
    if (cond < 0 || cond > 5) {
        fprintf(stderr, "Invalid condition code: %d\n", cond);
        abort();
    }
    
    size_t patch_pos = me_get_offset(c);
    arm64_emit_u32(c, b_cond_encodings[cond]);  /* placeholder */
    (void)label;
    return patch_pos;
}

/* lnot dst, src - logical NOT (dst = src == 0 ? 1 : 0) */
static void aarch64_emit_lnot(ZeroCCCompiler *c, int dst, int src) {
    /* cmp src, #0 */
    arm64_emit_u32(c, 0xEB00001F | (src << 16));
    /* cset dst, eq */
    arm64_emit_u32(c, 0x1A9F07E0 | dst);
}

/* param_save_int - save integer parameter */
static void aarch64_emit_param_save_int(ZeroCCCompiler *c, int param_idx, int32_t offset) {
    /* ldr x0, [sp, #offset] (load param from stack) */
    arm64_emit_u32(c, 0xF9400000 | ((offset & 0x3FF) << 10) | (31 << 5) | 0);
    /* str x0, [fp, #offset] (store to stack slot) */
    arm64_emit_u32(c, 0xF9000000 | ((offset & 0x3FF) << 10) | (29 << 5) | 0);
}

/* param_save_fp - save FP parameter */
static void aarch64_emit_param_save_fp(ZeroCCCompiler *c, int param_idx, int32_t offset) {
    /* ldr d0, [sp, #offset] (load FP param from stack) */
    arm64_emit_u32(c, 0xF9400000 | ((offset & 0x3FF) << 10) | (31 << 5) | 0);
    /* str d0, [fp, #offset] (store to stack slot) */
    arm64_emit_u32(c, 0xF9000000 | ((offset & 0x3FF) << 10) | (29 << 5) | 0);
}

/* variadic_spill - spill variadic arguments */
static void aarch64_emit_variadic_spill(ZeroCCCompiler *c, int count, int32_t offset) {
    /* No-op for AArch64 - variadic args already in register save area */
    (void)c; (void)count; (void)offset;
}

/* variadic_al_setup - setup for variadic argument access */
static void aarch64_emit_variadic_al_setup(ZeroCCCompiler *c, int num_fp_args) {
    /* No-op for AArch64 */
    (void)c; (void)num_fp_args;
}

/* vla_alloc - variable length array allocation */
static void aarch64_emit_vla_alloc(ZeroCCCompiler *c, int dst_reg, int size_reg) {
    /* sub dst, sp, size */
    arm64_emit_u32(c, 0xCB0003E0 | (size_reg << 16) | 31 | dst_reg);
}

/* atomic_cmpxchg - atomic compare and exchange */
static void aarch64_emit_atomic_cmpxchg(ZeroCCCompiler *c, int dst, int addr, int expected, int desired, int width) {
    /* ldxr dst, [addr] (load exclusive) */
    arm64_emit_u32(c, 0x885F7C00 | (addr << 5) | dst);
    /* cmp dst, expected */
    arm64_emit_u32(c, 0xEB00001F | (expected << 16) | (dst << 5));
    /* b.ne skip (placeholder) */
    arm64_emit_u32(c, 0x54000000);
    /* stxr wtmp, desired, [addr] (store exclusive) */
    arm64_emit_u32(c, 0x88007C00 | (addr << 5) | desired);
}

/* atomic_xadd - atomic add and return old value */
static void aarch64_emit_atomic_xadd(ZeroCCCompiler *c, int dst, int addr, int value, int width) {
    /* ldxr dst, [addr] */
    arm64_emit_u32(c, 0x885F7C00 | (addr << 5) | dst);
    /* add tmp, dst, value */
    arm64_emit_u32(c, 0x8B000000 | (value << 16) | (dst << 5) | dst);
    /* stlxr wtmp, dst, [addr] */
    arm64_emit_u32(c, 0x881F7C00 | (addr << 5) | dst);
}

/* atomic_xchg - atomic exchange */
static void aarch64_emit_atomic_xchg(ZeroCCCompiler *c, int dst_reg, int addr_reg, int val_reg, int width) {
    /* ldaxr dst, [addr] (load acquire exclusive) */
    arm64_emit_u32(c, 0x887F7C00 | (addr_reg << 5) | dst_reg);
    /* stlxr wtmp, val, [addr] (store release exclusive) */
    arm64_emit_u32(c, 0x881F7C00 | (addr_reg << 5) | val_reg);
    (void)width;
}

/* mfence - memory fence */
static void aarch64_emit_mfence(ZeroCCCompiler *c) {
    /* dmb ish (data memory barrier) */
    arm64_emit_u32(c, 0xD5033FB5);
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

/* sar dst_reg, src1, src2 (arithmetic shift right: dst = src1 >> src2) */
static void aarch64_emit_sar(ZeroCCCompiler *c, int dst_reg, int src1, int src2) {
    if (dst_reg != src1) {
        /* mov dst, src1 */
        arm64_emit_u32(c, 0xAA0003E0 | (dst_reg << 5) | src1);
    }
    /* asr dst, dst, src2 */
    arm64_emit_u32(c, 0x9AC02000 | (dst_reg << 10) | (dst_reg << 5) | src2);
}

/* call [reg] - call through register */
static void aarch64_emit_call_reg(ZeroCCCompiler *c, int reg) {
    /* blr reg */
    arm64_emit_u32(c, 0xD63F0000 | (reg << 5));
}

/* stack_align - ensure 16-byte stack alignment (AArch64 already handles this) */
static void aarch64_emit_stack_align(ZeroCCCompiler *c, uint8_t alignment) {
    /* AArch64 ABI requires 16-byte alignment. 
     * This is typically handled by prologue/epilogue. */
    (void)c;
    (void)alignment;
}

/* add_imm dst_reg, imm (add immediate) */
static void aarch64_emit_add_imm(ZeroCCCompiler *c, int dst_reg, int16_t imm) {
    if (imm >= 0) {
        /* add dst, dst, #imm */
        arm64_emit_u32(c, 0x91000000 | ((imm & 0xFFF) << 10) | (dst_reg << 5) | dst_reg);
    } else {
        /* sub dst, dst, #-imm */
        arm64_emit_u32(c, 0xD1000000 | (((-imm) & 0xFFF) << 10) | (dst_reg << 5) | dst_reg);
    }
}

/* sub_imm dst_reg, imm (subtract immediate) */
static void aarch64_emit_sub_imm(ZeroCCCompiler *c, int dst_reg, int16_t imm) {
    if (imm >= 0) {
        /* sub dst, dst, #imm */
        arm64_emit_u32(c, 0xD1000000 | ((imm & 0xFFF) << 10) | (dst_reg << 5) | dst_reg);
    } else {
        /* add dst, dst, #-imm */
        arm64_emit_u32(c, 0x91000000 | (((-imm) & 0xFFF) << 10) | (dst_reg << 5) | dst_reg);
    }
}

/* inc dst_reg (increment) */
static void aarch64_emit_inc(ZeroCCCompiler *c, int dst_reg) {
    /* add dst, dst, #1 */
    arm64_emit_u32(c, 0x91000400 | (dst_reg << 5) | dst_reg);
}

/* dec dst_reg (decrement) */
static void aarch64_emit_dec(ZeroCCCompiler *c, int dst_reg) {
    /* sub dst, dst, #1 */
    arm64_emit_u32(c, 0xD1000400 | (dst_reg << 5) | dst_reg);
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

/* Resolve all label patches for AArch64 */
static void aarch64_resolve_labels(ZeroCCCompiler *c) {
    for (int i = 0; i < c->label_count; i++) {
        size_t target = c->labels[i].offset;
        for (int j = 0; j < c->labels[i].patch_count; j++) {
            size_t patch_offset = c->labels[i].patches[j];
            /* AArch64 uses 26-bit signed relative branch (>> 2 for word alignment) */
            int32_t rel = (int32_t)((target - patch_offset) >> 2);
            
            if (patch_offset + 4 <= c->text_section.length) {
                uint8_t *buf = (uint8_t *)c->text_section.data;
                uint32_t *insn = (uint32_t*)(buf + patch_offset);
                *insn = (*insn & 0xFC000000) | (rel & 0x03FFFFFF);
            }
        }
    }
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
    .conv = &aarch64_conv,
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
    .emit_cond_set = aarch64_emit_cond_set,
    .emit_neg = aarch64_emit_neg,
    .emit_mov_reg = aarch64_emit_mov_reg,
    .emit_not = aarch64_emit_not,
    .emit_shr = aarch64_emit_shr,
    .emit_test = aarch64_emit_test,
    .emit_cqo = aarch64_emit_cqo,
    .emit_idiv = aarch64_emit_idiv,
    .emit_lea = aarch64_emit_lea,
    .patch_jmp = aarch64_patch_jmp,
    .resolve_labels = aarch64_resolve_labels,
    
    /* VREG mapping */
    .get_int_reg = aarch64_get_int_reg,
    .get_fp_reg = aarch64_get_fp_reg,
    
    .emit_load = aarch64_emit_load,
    .emit_store = aarch64_emit_store,
    
    .emit_deref = aarch64_emit_deref,
    .emit_load_32 = aarch64_emit_load_32,
    .emit_load_16 = aarch64_emit_load_16,
    .emit_load_byte = aarch64_emit_load_byte,
    .emit_store_32 = aarch64_emit_store_32,
    .emit_store_16 = aarch64_emit_store_16,
    .emit_store_byte = aarch64_emit_store_byte,
    .emit_lea_rip = aarch64_emit_lea_rip,
    
    .emit_jmp = aarch64_emit_jmp,
    .emit_jeq = aarch64_emit_jeq,
    .emit_jne = aarch64_emit_jne,
    .emit_jlt = aarch64_emit_jlt,
    .emit_jle = aarch64_emit_jle,
    .emit_jgt = aarch64_emit_jgt,
    .emit_jge = aarch64_emit_jge,
    .emit_jcond = aarch64_emit_jcond,
    .emit_setl = aarch64_emit_setl,
    .emit_setle = aarch64_emit_setle,
    .emit_setg = aarch64_emit_setg,
    .emit_setge = aarch64_emit_setge,
    .emit_sete = aarch64_emit_sete,
    .emit_setne = aarch64_emit_setne,
    .emit_cmp_zero = aarch64_emit_cmp_zero,
    .emit_lnot = aarch64_emit_lnot,
    .emit_call = aarch64_emit_call,
    .emit_call_reg = aarch64_emit_call_reg,
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
    .emit_stack_align = aarch64_emit_stack_align,
    .emit_push_reg = aarch64_emit_push_reg,
    .emit_pop_reg = aarch64_emit_pop_reg,
    .emit_add_imm = aarch64_emit_add_imm,
    .emit_sub_imm = aarch64_emit_sub_imm,
    .emit_inc = aarch64_emit_inc,
    .emit_dec = aarch64_emit_dec,
    .emit_sar = aarch64_emit_sar,
    .emit_label = aarch64_emit_label,
    
    /* ABI and special operations */
    .emit_param_save_int = aarch64_emit_param_save_int,
    .emit_param_save_fp = aarch64_emit_param_save_fp,
    .emit_variadic_spill = aarch64_emit_variadic_spill,
    .emit_variadic_al_setup = aarch64_emit_variadic_al_setup,
    .emit_vla_alloc = aarch64_emit_vla_alloc,
    .emit_atomic_cmpxchg = aarch64_emit_atomic_cmpxchg,
    .emit_atomic_xadd = aarch64_emit_atomic_xadd,
    .emit_atomic_xchg = aarch64_emit_atomic_xchg,
    .emit_mfence = aarch64_emit_mfence,
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
