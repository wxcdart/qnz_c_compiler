#include "isa_backend.h"
#include "zero_cc/ir/machine_emitter.h"
#include "zero_cc/ir/asm_emitter.h"
#include "riscv_asm.c"  /* Include assembly emission functions */
#include <stdint.h>

/* Helper macro to convert VREG to physical register */
static int riscv_vreg_to_phys(int vreg) {
    switch (vreg) {
        case 0:  return 10;  /* a0 - result */
        case 1:  return 11;  /* a1 */
        case 2:  return 12;  /* a2 */
        case 3:  return 13;  /* a3 */
        case 4:  return 14;  /* a4 */
        case 5:  return 15;  /* a5 */
        case 6:  return 16;  /* a6 */
        case 7:  return 17;  /* a7 */
        case 8:  return 5;   /* t0 */
        case 9:  return 6;   /* t1 */
        case 10: return 7;   /* t2 */
        case 11: return 28;  /* t3 */
        case 12: return 29;  /* t4 */
        case 13: return 30;  /* t5 */
        case 14: return 31;  /* t6 */
        case 15: return 3;   /* gp (avoid for now) */
        default: return vreg;
    }
}

/* Wrapper functions for VREG mapping */
static int riscv_get_int_reg(int vreg) { return riscv_vreg_to_phys(vreg); }
static int riscv_get_fp_reg(int vreg) { return vreg; }  /* Float regs are sequential */

#define RV_REG(vreg) riscv_vreg_to_phys(vreg)


/* RISC-V register info */
static const ISARegInfo riscv_regs = {
    .int_regs = {
        "zero", "ra", "sp", "gp", "tp", "t0", "t1", "t2",
        "s0", "s1", "a0", "a1", "a2", "a3", "a4", "a5",
        "a6", "a7", "s2", "s3", "s4", "s5", "s6", "s7",
        "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6",
    },
    .fp_regs = {
        "ft0", "ft1", "ft2", "ft3", "ft4", "ft5", "ft6", "ft7",
        "fs0", "fs1", "fa0", "fa1", "fa2", "fa3", "fa4", "fa5",
        "fa6", "fa7", "fs2", "fs3", "fs4", "fs5", "fs6", "fs7",
        "fs8", "fs9", "fs10", "fs11", "ft8", "ft9", "ft10", "ft11",
    },
    .int_arg_regs = {"a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7"},
    .fp_arg_regs = {"fa0", "fa1", "fa2", "fa3", "fa4", "fa5", "fa6", "fa7"},
    .int_ret_reg = "a0",
    .fp_ret_reg = "fa0",
    .num_int_regs = 32,
    .num_fp_regs = 32,
    .vec_regs = {"v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v8", "v9", "v10", "v11", "v12", "v13", "v14", "v15", "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23", "v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31"},
    .num_vec_regs = 32,
};

/* RISC-V calling convention (LP64D for 64-bit, ILP32D for 32-bit) */
/* Register numbers: 0=zero 1=ra 2=sp 3=gp 4=tp 5=t0 6=t1 7=t2 8=s0 9=s1 10=a0... */
static const int riscv64_int_arg_regs[] = {10, 11, 12, 13, 14, 15, 16, 17};  /* a0-a7 */
static const int riscv64_fp_arg_regs[] = {10, 11, 12, 13, 14, 15, 16, 17};  /* fa0-fa7 */
static const int riscv64_callee_saved_gpr[] = {8, 9, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27};  /* s0-s11 */

static const ISACallingConvention riscv64_conv = {
    .int_arg_regs = riscv64_int_arg_regs,
    .int_arg_reg_count = 8,
    .fp_arg_regs = riscv64_fp_arg_regs,
    .fp_arg_reg_count = 8,
    .callee_saved_gpr = riscv64_callee_saved_gpr,
    .callee_saved_gpr_count = 12,
    .callee_save_slot_offsets = (const int32_t[]){8, 16, 24, 32, 40, 48, 56, 64, 72, 80, 88, 96},
};

/* RISC-V 32-bit calling convention (similar but different register sizes) */
static const int riscv32_int_arg_regs[] = {10, 11, 12, 13, 14, 15, 16, 17};  /* a0-a7 */
static const int riscv32_fp_arg_regs[] = {10, 11, 12, 13, 14, 15, 16, 17};  /* fa0-fa7 */
static const int riscv32_callee_saved_gpr[] = {8, 9, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27};  /* s0-s11 */

static const ISACallingConvention riscv32_conv = {
    .int_arg_regs = riscv32_int_arg_regs,
    .int_arg_reg_count = 8,
    .fp_arg_regs = riscv32_fp_arg_regs,
    .fp_arg_reg_count = 8,
    .callee_saved_gpr = riscv32_callee_saved_gpr,
    .callee_saved_gpr_count = 12,
    .callee_save_slot_offsets = (const int32_t[]){8, 16, 24, 32, 40, 48, 56, 64, 72, 80, 88, 96},
};

/* RISC-V instruction encoding helpers */
static inline void riscv_emit_u32(ZeroCCCompiler *c, uint32_t insn) {
    me_emit_u8(c, insn & 0xFF);
    me_emit_u8(c, (insn >> 8) & 0xFF);
    me_emit_u8(c, (insn >> 16) & 0xFF);
    me_emit_u8(c, (insn >> 24) & 0xFF);
}

/* RISC-V RV64 backend implementation */
static void riscv64_emit_prologue(ZeroCCCompiler *c, size_t stack_size, uint32_t callee_save_mask) {
    /* addi sp, sp, -stack_size */
    if (stack_size > 0) {
        uint32_t off = (stack_size + 15) & ~15;  /* align to 16 bytes */
        riscv_emit_u32(c, 0x00000113 | ((off & 0xFFF) << 20) | (2 << 7));  /* addi sp, sp, -off */
    }
    /* sd ra, 0(sp) */
    riscv_emit_u32(c, 0x00002023 | (1 << 15));  /* sd ra, 0(sp) */
    /* sd s0, 8(sp) */
    riscv_emit_u32(c, 0x00002023 | (8 << 7) | (1 << 15) | (8 << 20));
    /* addi s0, sp, stack_size */
    if (stack_size > 0) {
        riscv_emit_u32(c, 0x00000113 | ((stack_size & 0xFFF) << 20) | (8 << 15) | (2 << 7));
    }
    
    /* Emit callee-saved register stores (after prologue, when mask is populated) */
    /* RISC-V callee-saved registers: s0-s11 (physical regs 8-9, 18-27) */
    /* Note: s0 is already saved above, so we save s1-s11 */
    static const int32_t slot_offsets[] = {16, 24, 32, 40, 48, 56, 64, 72, 80, 88, 96};
    static const int regs[] = {9, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27};
    
    for (int i = 0; i < 11; i++) {
        int reg = regs[i];
        if ((callee_save_mask >> reg) & 1u) {
            /* sd reg, offset(sp) */
            riscv_emit_u32(c, 0x00002023 | ((reg & 0x1F) << 15) | ((reg & 0x1F) << 7) | ((slot_offsets[i] & 0x1FF) << 20));
        }
    }
}

static void riscv64_emit_epilogue(ZeroCCCompiler *c) {
    /* ld ra, 0(sp) */
    riscv_emit_u32(c, 0x00002003 | (1 << 15));
    /* ld s0, 8(sp) */
    riscv_emit_u32(c, 0x00002003 | (8 << 15) | (8 << 20));
    /* addi sp, sp, stack_size */
    /* TODO: track stack size */
    /* ret */
    riscv_emit_u32(c, 0x00008067);  /* jalr zero, 0(ra) */
}

static void riscv64_emit_leave(ZeroCCCompiler *c) {
    /* RISC-V doesn't have a direct 'leave' instruction
     * Equivalent: addi sp, s0, 0; ld ra, 0(sp); ld s0, 8(sp)
     * For simplicity, we'll restore s0 and ra without adjusting sp
     */
    /* ld ra, 0(sp) */
    riscv_emit_u32(c, 0x00002003 | (1 << 15));
    /* ld s0, 8(sp) */
    riscv_emit_u32(c, 0x00002003 | (8 << 15) | (8 << 20));
}

/* Assembly emission functions for text output - defined in riscv_asm.c */
/* static void riscv64_emit_prologue_asm(ZeroCCCompiler *c, size_t stack_size) { ... } */
/* static void riscv64_emit_epilogue_asm(ZeroCCCompiler *c) { ... } */
/* static const char *riscv64_get_hw_reg_name(int phys_reg) { ... } */

static size_t riscv64_get_code_offset(ZeroCCCompiler *c) {
    return me_get_offset(c);
}

static void riscv64_emit_mov_imm(ZeroCCCompiler *c, int dst_reg, int64_t imm) {
    /* Use lui + addi sequence for 64-bit immediates */
    if (imm <= 2047 && imm >= -2048) {
        riscv_emit_u32(c, 0x00000113 | (((uint64_t)imm & 0xFFF) << 20) | (RV_REG(dst_reg) << 7));
    } else {
        /* For larger immediates, use lui + addi */
        uint64_t hi = (imm + 0x800) >> 12;
        uint64_t lo = imm & 0xFFF;
        if (hi != 0) {
            riscv_emit_u32(c, 0x000003B7 | ((hi & 0xFFFFF) << 12) | (RV_REG(dst_reg) << 7));  /* lui */
        }
        if (lo != 0) {
            riscv_emit_u32(c, 0x00000113 | (lo << 20) | (RV_REG(dst_reg) << 15) | (RV_REG(dst_reg) << 7));  /* addi */
        }
    }
}

static void riscv64_emit_add(ZeroCCCompiler *c, int dst, int src1, int src2) {
    /* add dst, src1, src2 */
    riscv_emit_u32(c, 0x00000033 | (RV_REG(dst) << 7) | (RV_REG(src1) << 15) | (RV_REG(src2) << 20));
}

static void riscv64_emit_sub(ZeroCCCompiler *c, int dst, int src1, int src2) {
    /* sub dst, src1, src2 */
    riscv_emit_u32(c, 0x40000033 | (RV_REG(dst) << 7) | (RV_REG(src1) << 15) | (RV_REG(src2) << 20));
}

static void riscv64_emit_mul(ZeroCCCompiler *c, int dst, int src1, int src2) {
    /* mul dst, src1, src2 (M extension) */
    riscv_emit_u32(c, 0x02000033 | (RV_REG(dst) << 7) | (RV_REG(src1) << 15) | (RV_REG(src2) << 20) | (0 << 25));
}

static void riscv64_emit_div(ZeroCCCompiler *c, int dst, int src1, int src2) {
    /* div dst, src1, src2 (M extension) */
    riscv_emit_u32(c, 0x02000033 | (RV_REG(dst) << 7) | (RV_REG(src1) << 15) | (RV_REG(src2) << 20) | (3 << 25));
}

static void riscv64_emit_and(ZeroCCCompiler *c, int dst, int src1, int src2) {
    riscv_emit_u32(c, 0x00007033 | (RV_REG(dst) << 7) | (RV_REG(src1) << 15) | (RV_REG(src2) << 20));
}

static void riscv64_emit_or(ZeroCCCompiler *c, int dst, int src1, int src2) {
    riscv_emit_u32(c, 0x00006033 | (RV_REG(dst) << 7) | (RV_REG(src1) << 15) | (RV_REG(src2) << 20));
}

static void riscv64_emit_xor(ZeroCCCompiler *c, int dst, int src1, int src2) {
    riscv_emit_u32(c, 0x00004033 | (RV_REG(dst) << 7) | (RV_REG(src1) << 15) | (RV_REG(src2) << 20));
}

static void riscv64_emit_shl(ZeroCCCompiler *c, int dst, int src, int shift) {
    if (shift >= 0 && shift < 64) {
        riscv_emit_u32(c, 0x00001013 | (RV_REG(dst) << 7) | (RV_REG(src) << 15) | (shift << 20));
    }
}

static void riscv64_emit_cmp(ZeroCCCompiler *c, int src1, int src2) {
    /* Use slt/sltu to set a temporary register, or just use branch directly */
    /* For now, we'll use conditional branches directly */
    (void)c; (void)src1; (void)src2;
}

static void riscv64_emit_neg(ZeroCCCompiler *c, int dst, int src) {
    /* sub dst, zero, src */
    riscv_emit_u32(c, 0x40000033 | (RV_REG(dst) << 7) | (RV_REG(src) << 20));
}

static void riscv64_emit_load(ZeroCCCompiler *c, int dst_reg, int base_reg, int32_t offset) {
    /* ld dst, offset(base) */
    riscv_emit_u32(c, 0x00000003 | (RV_REG(dst_reg) << 7) | (RV_REG(base_reg) << 15) | ((offset & 0xFFF) << 20));
}

static void riscv64_emit_store(ZeroCCCompiler *c, int src_reg, int base_reg, int32_t offset) {
    /* sd src, offset(base) */
    riscv_emit_u32(c, 0x00002023 | (RV_REG(base_reg) << 15) | (RV_REG(src_reg) << 20) | ((offset & 0xFFF) << 7));
}

static size_t riscv64_emit_jmp(ZeroCCCompiler *c, const char *label) {
    size_t patch_pos = me_get_offset(c);
    /* jal zero, offset */
    riscv_emit_u32(c, 0x0000006F);
    (void)label;
    return patch_pos;
}

static size_t riscv64_emit_jeq(ZeroCCCompiler *c, const char *label) {
    size_t patch_pos = me_get_offset(c);
    /* beq x, zero, label */
    riscv_emit_u32(c, 0x00000063);
    (void)label;
    return patch_pos;
}

static size_t riscv64_emit_jne(ZeroCCCompiler *c, const char *label) {
    size_t patch_pos = me_get_offset(c);
    /* bne x, zero, label */
    riscv_emit_u32(c, 0x00001063);
    (void)label;
    return patch_pos;
}

static size_t riscv64_emit_jlt(ZeroCCCompiler *c, const char *label) {
    size_t patch_pos = me_get_offset(c);
    /* blt x, zero, label */
    riscv_emit_u32(c, 0x00004063);
    (void)label;
    return patch_pos;
}

static size_t riscv64_emit_jle(ZeroCCCompiler *c, const char *label) {
    size_t patch_pos = me_get_offset(c);
    /* ble x, zero, label */
    riscv_emit_u32(c, 0x00006063);
    (void)label;
    return patch_pos;
}

static size_t riscv64_emit_jgt(ZeroCCCompiler *c, const char *label) {
    size_t patch_pos = me_get_offset(c);
    /* bgt x, zero, label */
    riscv_emit_u32(c, 0x00005063);
    (void)label;
    return patch_pos;
}

static size_t riscv64_emit_jge(ZeroCCCompiler *c, const char *label) {
    size_t patch_pos = me_get_offset(c);
    /* bge x, zero, label */
    riscv_emit_u32(c, 0x00007063);
    (void)label;
    return patch_pos;
}

static void riscv64_emit_call(ZeroCCCompiler *c, const char *func_name) {
    /* jal ra, offset */
    riscv_emit_u32(c, 0x000000EF | (1 << 7));
    (void)func_name;
}

static void riscv64_emit_ret(ZeroCCCompiler *c) {
    /* jalr zero, 0(ra) */
    riscv_emit_u32(c, 0x00008067);
}

/* sar dst_reg, src1, src2 (arithmetic shift right: dst = src1 >> src2) */
static void riscv64_emit_sar(ZeroCCCompiler *c, int dst_reg, int src1, int src2) {
    if (dst_reg != src1) {
        /* mv dst, src1 */
        riscv_emit_u32(c, 0x00000033 | (RV_REG(dst_reg) << 7) | (RV_REG(src1) << 15));
    }
    /* sra dst, dst, src2 */
    riscv_emit_u32(c, 0x40000033 | (RV_REG(dst_reg) << 7) | (RV_REG(dst_reg) << 15) | (RV_REG(src2) << 20));
}

/* call [reg] - call through register */
static void riscv64_emit_call_reg(ZeroCCCompiler *c, int reg) {
    /* jalr ra, 0(reg) */
    riscv_emit_u32(c, 0x00000067 | (RV_REG(reg) << 15) | (1 << 7));
}

/* stack_align - ensure 16-byte stack alignment (RISC-V already handles this) */
static void riscv64_emit_stack_align(ZeroCCCompiler *c, uint8_t alignment) {
    /* RISC-V ABI requires 16-byte alignment. 
     * This is typically handled by prologue/epilogue. */
    (void)c;
    (void)alignment;
}

/* add_imm dst_reg, imm (add immediate) */
static void riscv64_emit_add_imm(ZeroCCCompiler *c, int dst_reg, int16_t imm) {
    if (imm >= 0) {
        /* addi dst, dst, imm */
        riscv_emit_u32(c, ((imm & 0xFFF) << 20) | (RV_REG(dst_reg) << 7) | (RV_REG(dst_reg) << 15));
    } else {
        /* addi dst, dst, imm (negative) */
        riscv_emit_u32(c, ((imm & 0xFFF) << 20) | (RV_REG(dst_reg) << 7) | (RV_REG(dst_reg) << 15));
    }
}

/* sub_imm dst_reg, imm (subtract immediate) */
static void riscv64_emit_sub_imm(ZeroCCCompiler *c, int dst_reg, int16_t imm) {
    if (imm >= 0) {
        /* addi dst, dst, -imm */
        riscv_emit_u32(c, ((-imm & 0xFFF) << 20) | (RV_REG(dst_reg) << 7) | (RV_REG(dst_reg) << 15) | (0x400 << 20));
    } else {
        /* addi dst, dst, -imm (negative = add) */
        riscv_emit_u32(c, ((-imm & 0xFFF) << 20) | (RV_REG(dst_reg) << 7) | (RV_REG(dst_reg) << 15));
    }
}

/* inc dst_reg (increment) */
static void riscv64_emit_inc(ZeroCCCompiler *c, int dst_reg) {
    /* addi dst, dst, 1 */
    riscv_emit_u32(c, (1 << 20) | (RV_REG(dst_reg) << 7) | (RV_REG(dst_reg) << 15));
}

/* dec dst_reg (decrement) */
static void riscv64_emit_dec(ZeroCCCompiler *c, int dst_reg) {
    /* addi dst, dst, -1 */
    riscv_emit_u32(c, ((-1 & 0xFFF) << 20) | (RV_REG(dst_reg) << 7) | (RV_REG(dst_reg) << 15));
}

/* Floating point operations */
static void riscv64_emit_load_fp(ZeroCCCompiler *c, int dst_reg, int base_reg, int32_t offset) {
    /* fld fa, offset(base) */
    riscv_emit_u32(c, 0x00000007 | (RV_REG(dst_reg) << 7) | (RV_REG(base_reg) << 15) | ((offset & 0xFFF) << 20) | (3 << 25));
}

static void riscv64_emit_store_fp(ZeroCCCompiler *c, int src_reg, int base_reg, int32_t offset) {
    /* fsd fa, offset(base) */
    riscv_emit_u32(c, 0x00002027 | (RV_REG(base_reg) << 15) | (RV_REG(src_reg) << 20) | ((offset & 0xFFF) << 7) | (3 << 25));
}

static void riscv64_emit_load_fp_rodata(ZeroCCCompiler *c, int dst_reg, size_t rodata_offset) {
    /* Use literal pool - store to memory and load */
    (void)c; (void)dst_reg; (void)rodata_offset;
}

static void riscv64_emit_fp_add(ZeroCCCompiler *c, int dst, int src1, int src2) {
    /* fadd.d fd, fs1, fs2 */
    riscv_emit_u32(c, 0x00000053 | (dst << 7) | (src1 << 15) | (src2 << 20));
}

static void riscv64_emit_fp_sub(ZeroCCCompiler *c, int dst, int src1, int src2) {
    riscv_emit_u32(c, 0x00001053 | (dst << 7) | (src1 << 15) | (src2 << 20));
}

static void riscv64_emit_fp_mul(ZeroCCCompiler *c, int dst, int src1, int src2) {
    riscv_emit_u32(c, 0x00002053 | (dst << 7) | (src1 << 15) | (src2 << 20));
}

static void riscv64_emit_fp_div(ZeroCCCompiler *c, int dst, int src1, int src2) {
    riscv_emit_u32(c, 0x00003053 | (dst << 7) | (src1 << 15) | (src2 << 20));
}

static void riscv64_emit_fp_neg(ZeroCCCompiler *c, int dst, int src) {
    /* fneg.d fd, fs = fsgnjx.d fd, fs, fs */
    riscv_emit_u32(c, 0x00005053 | (dst << 7) | (src << 15) | (src << 20));
}

static void riscv64_emit_fp_cmp(ZeroCCCompiler *c, int src1, int src2) {
    /* fle.d / flt.d / feq.d */
    riscv_emit_u32(c, 0x0000A053 | (src2 << 7) | (src1 << 15) | (1 << 25));  /* fle.d */
}

static void riscv64_emit_fp_cvtsi(ZeroCCCompiler *c, int int_dst, int fp_src) {
    /* fcvt.w.d rd, fs (round to zero) */
    riscv_emit_u32(c, 0x00006053 | (int_dst << 7) | (fp_src << 15) | (1 << 25));
}

static void riscv64_emit_fp_cvtsf(ZeroCCCompiler *c, int fp_dst, int int_src) {
    /* fcvt.d.w fd, rs */
    riscv_emit_u32(c, 0x00004053 | (fp_dst << 7) | (int_src << 15) | (1 << 25));
}

static void riscv64_emit_fp_mov(ZeroCCCompiler *c, int dst, int src) {
    if (dst == src) return;
    /* fmv.d fd, fs */
    riscv_emit_u32(c, 0x00000053 | (dst << 7) | (src << 15) | (src << 20));
}

static void riscv64_emit_const(ZeroCCCompiler *c, int64_t value) {
    riscv64_emit_mov_imm(c, 10, value);  /* a0 */
}

static void riscv64_emit_stack_adjust(ZeroCCCompiler *c, int32_t delta) {
    if (delta == 0) return;
    if (delta > 0) {
        riscv_emit_u32(c, 0x00000113 | ((delta & 0xFFF) << 20) | (2 << 7));  /* addi sp, sp, delta */
    } else {
        riscv_emit_u32(c, 0x00000113 | (((-delta) & 0xFFF) << 20) | (2 << 7));  /* addi sp, sp, -delta */
    }
}

static void riscv64_emit_push_reg(ZeroCCCompiler *c, int reg) {
    /* addi sp, sp, -8; sd reg, 0(sp) */
    riscv_emit_u32(c, 0xFF810113);  /* addi sp, sp, -8 */
    riscv_emit_u32(c, 0x00002023 | (reg << 20));
}

static void riscv64_emit_pop_reg(ZeroCCCompiler *c, int reg) {
    /* ld reg, 0(sp); addi sp, sp, 8 */
    riscv_emit_u32(c, 0x00000003 | (reg << 7) | (2 << 15));
    riscv_emit_u32(c, 0x00810113);  /* addi sp, sp, 8 */
}

static void riscv64_emit_label(ZeroCCCompiler *c, const char *name) {
    (void)c; (void)name;
}



static size_t riscv64_patch_jmp(ZeroCCCompiler *c, size_t patch_offset, size_t target_offset) {
    int32_t rel = (int32_t)(target_offset - patch_offset);
    if (patch_offset + 4 <= c->text_section.length) {
        uint8_t *buf = (uint8_t *)c->text_section.data;
        uint32_t *insn = (uint32_t*)(buf + patch_offset);
        *insn = (*insn & 0xFFF000FF) | ((rel & 0xFFFFF) << 12);
    }
    return patch_offset;
}



static void riscv64_emit_test(ZeroCCCompiler *c, int src1, int src2) {
    /* and t0, src1, src2 (use t0 as temp for test) */
    riscv_emit_u32(c, 0x00007033 | (5 << 7) | (src1 << 15) | (src2 << 20));
}

static void riscv64_emit_cqo(ZeroCCCompiler *c) {
    (void)c;
}

static void riscv64_emit_idiv(ZeroCCCompiler *c, int src) {
    /* div a0, a0, src */
    riscv_emit_u32(c, 0x02000033 | (10 << 7) | (10 << 15) | (src << 20) | (3 << 25));
}

static void riscv64_emit_lea(ZeroCCCompiler *c, int dst, int base, int32_t offset) {
    /* addi dst, base, offset */
    riscv_emit_u32(c, 0x00000113 | ((offset & 0xFFF) << 20) | ((base & 0x1F) << 15) | ((dst & 0x1F) << 7));
}



static void riscv64_emit_mod(ZeroCCCompiler *c, int dst, int src1, int src2) {
    /* Modulo: dst = src1 % src2
     * RISC-V rem instruction */
    riscv_emit_u32(c, 0x02000033 | (dst << 7) | (src1 << 15) | (src2 << 20) | (7 << 25));
}

/* Core emission functions */

/* mov_reg dst, src - register-to-register move */
static void riscv64_emit_mov_reg(ZeroCCCompiler *c, int dst, int src) {
    if (dst == src) return;
    /* addi dst, src, 0 */
    riscv_emit_u32(c, 0x00000013 | (dst << 7) | (src << 15));
}

/* not dst, src - bitwise NOT */
static void riscv64_emit_not(ZeroCCCompiler *c, int dst, int src) {
    /* xori dst, src, -1 */
    riscv_emit_u32(c, 0xFFF04013 | (dst << 7) | (src << 15));
}

/* shr dst, src, shift - logical shift right */
static void riscv64_emit_shr(ZeroCCCompiler *c, int dst, int src, int shift) {
    /* srli dst, src, shift */
    riscv_emit_u32(c, 0x00005013 | (dst << 7) | (src << 15) | ((shift & 0x1F) << 20));
}

/* Memory operations */

/* deref dst, [src] - pointer dereference (load) */
static void riscv64_emit_deref(ZeroCCCompiler *c, int dst, int src) {
    /* ld dst, 0(src) */
    riscv_emit_u32(c, 0x00000003 | (dst << 7) | (src << 15));
}

/* load_32 dst, [base, offset] - 32-bit load */
static void riscv64_emit_load_32(ZeroCCCompiler *c, int dst, int base, int32_t offset) {
    /* lw dst, offset(base) */
    riscv_emit_u32(c, 0x00000003 | (dst << 7) | (base << 15) | ((offset & 0xFFF) << 20));
}

/* load_16 dst, [base, offset] - 16-bit load */
static void riscv64_emit_load_16(ZeroCCCompiler *c, int dst, int base, int32_t offset) {
    /* lh dst, offset(base) */
    riscv_emit_u32(c, 0x00001003 | (dst << 7) | (base << 15) | ((offset & 0xFFF) << 20));
}

/* load_byte dst, [base, offset] - 8-bit load */
static void riscv64_emit_load_byte(ZeroCCCompiler *c, int dst, int base, int32_t offset) {
    /* lb dst, offset(base) */
    riscv_emit_u32(c, 0x00000003 | (dst << 7) | (base << 15) | ((offset & 0xFFF) << 20));
}

/* store_32 src, [base, offset] - 32-bit store */
static void riscv64_emit_store_32(ZeroCCCompiler *c, int src, int base, int32_t offset) {
    /* sw src, offset(base) */
    riscv_emit_u32(c, 0x00002023 | (base << 15) | (src << 20) | ((offset & 0xFFF) << 7));
}

/* store_16 src, [base, offset] - 16-bit store */
static void riscv64_emit_store_16(ZeroCCCompiler *c, int src, int base, int32_t offset) {
    /* sh src, offset(base) */
    riscv_emit_u32(c, 0x00001023 | (base << 15) | (src << 20) | ((offset & 0xFFF) << 7));
}

/* store_byte src, [base, offset] - 8-bit store */
static void riscv64_emit_store_byte(ZeroCCCompiler *c, int src, int base, int32_t offset) {
    /* sb src, offset(base) */
    riscv_emit_u32(c, 0x00000023 | (base << 15) | (src << 20) | ((offset & 0xFFF) << 7));
}

/* lea_rip dst, label - RIP-relative LEA (PC-relative on RISC-V) */
static void riscv64_emit_lea_rip(ZeroCCCompiler *c, int dst, const char *label) {
    /* auipc dst, offset; addi dst, dst, offset */
    (void)c; (void)dst; (void)label;
    /* Placeholder - would need label resolution */
    riscv_emit_u32(c, 0x000017);  /* auipc x1, 0 */
    riscv_emit_u32(c, 0x00000093);  /* addi x2, x1, 0 */
}

/* Comparisons */

/* cond_set dst, cond - set dst to 1 or 0 based on condition code
 * IR condition codes: CC_LT=0, CC_LE=1, CC_GT=2, CC_GE=3, CC_EQ=4, CC_NE=5
 * 
 * For RISC-V, we use SLT with the operands from the previous CMP instruction.
 * The CMP instruction stores the operands in c->lowering.cmp_src1/2.
 * 
 * Condition code mapping:
 * - CC_LT: slt dst, src1, src2 (dst = src1 < src2)
 * - CC_LE: slt dst, src2, src1; xnor dst, dst, zero (dst = !(src2 < src1) = src1 <= src2)
 * - CC_GT: slt dst, src2, src1; xnor dst, dst, zero (dst = !(src2 < src1) = src1 > src2)
 * - CC_GE: slt dst, src1, src2; xnor dst, dst, zero (dst = !(src1 < src2) = src1 >= src2)
 * - CC_EQ: seqz dst, src1; seqz dst, dst (src1 == 0)
 * - CC_NE: snez dst, src1; snez dst, dst (src1 != 0)
 */
static void riscv64_emit_cond_set(ZeroCCCompiler *c, int dst, int cond) {
    int src1 = c->lowering.cmp_src1;
    int src2 = c->lowering.cmp_src2;
    
    switch (cond) {
        case 0: /* CC_LT: slt dst, src1, src2 */
            riscv_emit_u32(c, 0x00200033 | (dst << 7) | (RV_REG(src1) << 20) | (RV_REG(src2) << 15));
            break;
        case 1: /* CC_LE: slt dst, src2, src1; xnor dst, dst, zero */
            riscv_emit_u32(c, 0x00100033 | (dst << 7) | (RV_REG(src2) << 20) | (RV_REG(src1) << 15));
            riscv_emit_u32(c, 0x40000013 | (dst << 7) | (dst << 15));
            break;
        case 2: /* CC_GT: slt dst, src2, src1; xnor dst, dst, zero */
            riscv_emit_u32(c, 0x00100033 | (dst << 7) | (RV_REG(src2) << 20) | (RV_REG(src1) << 15));
            riscv_emit_u32(c, 0x40000013 | (dst << 7) | (dst << 15));
            break;
        case 3: /* CC_GE: slt dst, src1, src2; xnor dst, dst, zero */
            riscv_emit_u32(c, 0x00200033 | (dst << 7) | (RV_REG(src1) << 20) | (RV_REG(src2) << 15));
            riscv_emit_u32(c, 0x40000013 | (dst << 7) | (dst << 15));
            break;
        case 4: /* CC_EQ: seqz dst, src1 (src1 == 0) */
            riscv_emit_u32(c, 0x10000013 | (dst << 7) | (RV_REG(src1) << 15));
            break;
        case 5: /* CC_NE: snez dst, src1 (src1 != 0) */
            riscv_emit_u32(c, 0x10000013 | (dst << 7) | (RV_REG(src1) << 15));
            riscv_emit_u32(c, 0x00000093 | (dst << 7) | (dst << 15) | 0xFFF); /* addi dst, dst, -1 (NOT) */
            break;
        default:
            riscv_emit_u32(c, 0x00000013 | (dst << 7));  /* addi dst, zero, 0 */
            break;
    }
}

/* jcond cond, label - conditional jump based on condition code
 * IR condition codes: CC_LT=0, CC_LE=1, CC_GT=2, CC_GE=3, CC_EQ=4, CC_NE=5
 * 
 * For RISC-V, we use Bcc instructions with the operands from the previous CMP.
 * The CMP instruction stores the operands in c->lowering.cmp_src1/2.
 * 
 * Condition code mapping:
 * - CC_LT: blt src1, src2, label
 * - CC_LE: ble src1, src2, label
 * - CC_GT: bgt src1, src2, label
 * - CC_GE: bge src1, src2, label
 * - CC_EQ: beq src1, src2, label
 * - CC_NE: bne src1, src2, label
 */
static size_t riscv64_emit_jcond(ZeroCCCompiler *c, int cond, const char *label) {
    int src1 = c->lowering.cmp_src1;
    int src2 = c->lowering.cmp_src2;
    size_t patch_pos = me_get_offset(c);
    
    switch (cond) {
        case 0: /* CC_LT: blt src1, src2, label */
            riscv_emit_u32(c, 0x00004063 | (RV_REG(src1) << 15) | (RV_REG(src2) << 20));
            break;
        case 1: /* CC_LE: ble src1, src2, label (bge src2, src1, label) */
            riscv_emit_u32(c, 0x00007063 | (RV_REG(src2) << 15) | (RV_REG(src1) << 20));
            break;
        case 2: /* CC_GT: bgt src1, src2, label (blt src2, src1, label) */
            riscv_emit_u32(c, 0x00004063 | (RV_REG(src2) << 15) | (RV_REG(src1) << 20));
            break;
        case 3: /* CC_GE: bge src1, src2, label */
            riscv_emit_u32(c, 0x00007063 | (RV_REG(src1) << 15) | (RV_REG(src2) << 20));
            break;
        case 4: /* CC_EQ: beq src1, src2, label */
            riscv_emit_u32(c, 0x00000063 | (RV_REG(src1) << 15) | (RV_REG(src2) << 20));
            break;
        case 5: /* CC_NE: bne src1, src2, label */
            riscv_emit_u32(c, 0x00001063 | (RV_REG(src1) << 15) | (RV_REG(src2) << 20));
            break;
        default:
            riscv_emit_u32(c, 0x00000063);  /* beq zero, zero, label */
            break;
    }
    
    (void)label;
    return patch_pos;
}

/* setl dst - set if less (signed) */
static void riscv64_emit_setl(ZeroCCCompiler *c, int dst) {
    /* slt dst, x0, x0 (placeholder - actual implementation uses branch) */
    riscv_emit_u32(c, 0x00000033 | (dst << 7));  /* addi dst, zero, 0 */
}

/* setle dst - set if less or equal (signed) */
static void riscv64_emit_setle(ZeroCCCompiler *c, int dst) {
    riscv_emit_u32(c, 0x00000033 | (dst << 7));  /* addi dst, zero, 0 */
}

/* setg dst - set if greater (signed) */
static void riscv64_emit_setg(ZeroCCCompiler *c, int dst) {
    riscv_emit_u32(c, 0x00000033 | (dst << 7));
}

/* setge dst - set if greater or equal (signed) */
static void riscv64_emit_setge(ZeroCCCompiler *c, int dst) {
    riscv_emit_u32(c, 0x00000033 | (dst << 7));
}

/* sete dst - set if equal */
static void riscv64_emit_sete(ZeroCCCompiler *c, int dst) {
    riscv_emit_u32(c, 0x00000033 | (dst << 7));
}

/* setne dst - set if not equal */
static void riscv64_emit_setne(ZeroCCCompiler *c, int dst) {
    riscv_emit_u32(c, 0x00000033 | (dst << 7));
}

/* cmp_zero src - compare to zero */
static void riscv64_emit_cmp_zero(ZeroCCCompiler *c, int src) {
    /* beq src, zero, label */
    riscv_emit_u32(c, 0x00000063 | (src << 15));
}

/* lnot dst, src - logical NOT (dst = !src) */
static void riscv64_emit_lnot(ZeroCCCompiler *c, int dst, int src) {
    /* seqz dst, src (set if equal to zero) */
    riscv_emit_u32(c, 0x10000013 | (dst << 7) | (src << 15));
}

/* Control flow */

/* jmp_cond cond, label - conditional jump based on condition code */
static size_t riscv64_emit_jmp_cond(ZeroCCCompiler *c, int cond, const char *label) {
    size_t patch_pos = me_get_offset(c);
    /* b.cond placeholder */
    riscv_emit_u32(c, 0x00000063);
    (void)label;
    return patch_pos;
}

/* Function ABI */

/* param_save_int reg, slot - save integer param to stack slot */
static void riscv64_emit_param_save_int(ZeroCCCompiler *c, int reg, int slot) {
    /* sd reg, offset(sp) */
    riscv_emit_u32(c, 0x00002023 | (2 << 15) | (reg << 20) | ((slot * 8 & 0xFFF) << 7));
}

/* param_save_fp reg, slot - save FP param to stack slot */
static void riscv64_emit_param_save_fp(ZeroCCCompiler *c, int reg, int slot) {
    /* fsd reg, offset(sp) */
    riscv_emit_u32(c, 0x00006023 | (2 << 15) | (reg << 20) | ((slot * 8 & 0xFFF) << 7));
}

/* variadic_spill - spill variadic arguments */
static void riscv64_emit_variadic_spill(ZeroCCCompiler *c, int param_count, int32_t base_offset) {
    (void)c; (void)param_count; (void)base_offset;
    /* Save a0-a7, fa0-fa7 to variadic spill area */
}

/* variadic_al_setup - setup for variadic arguments */
static void riscv64_emit_variadic_al_setup(ZeroCCCompiler *c, int num_fp_args) {
    (void)c; (void)num_fp_args;
}

/* Special operations */

/* vla_alloc dst, size - variable length array allocation */
static void riscv64_emit_vla_alloc(ZeroCCCompiler *c, int dst_reg, int size_reg) {
    /* sub sp, sp, size */
    riscv_emit_u32(c, 0x400001b3 | (2 << 7) | (2 << 15) | (size_reg << 20));
    if (dst_reg != size_reg) {
        riscv64_emit_mov_reg(c, dst_reg, size_reg);
    }
}

/* atomic_cmpxchg dst, addr, expected, desired, width - atomic compare and swap */
static void riscv64_emit_atomic_cmpxchg(ZeroCCCompiler *c, int dst, int addr, int expected, int desired, int width) {
    /* lr/sc loop */
    (void)width;
    riscv_emit_u32(c, 0x1000202F | (addr << 15) | dst);  /* lr.w dst, (addr) */
    riscv_emit_u32(c, 0x02000033 | (dst << 7) | (dst << 15) | (expected << 20));  /* bne check */
    riscv_emit_u32(c, 0x1000202F | (addr << 15) | (desired << 20));  /* sc.w */
}

/* atomic_xadd dst, addr, val, width - atomic add and swap */
static void riscv64_emit_atomic_xadd(ZeroCCCompiler *c, int dst, int addr, int val, int width) {
    /* lr/sc loop with add */
    (void)width;
    riscv_emit_u32(c, 0x1000202F | (addr << 15) | dst);  /* lr.w dst, (addr) */
    riscv_emit_u32(c, 0x00000033 | (dst << 7) | (dst << 15) | (val << 20));  /* add */
    riscv_emit_u32(c, 0x1000202F | (addr << 15) | (dst << 20));  /* sc.w */
}

/* atomic_xchg dst, addr, val, width - atomic exchange */
static void riscv64_emit_atomic_xchg(ZeroCCCompiler *c, int dst, int addr, int val, int width) {
    /* lr/sc loop */
    (void)width;
    riscv_emit_u32(c, 0x1000202F | (addr << 15) | dst);  /* lr.w dst, (addr) */
    riscv_emit_u32(c, 0x1000202F | (addr << 15) | (val << 20));  /* sc.w val, (addr) */
}

/* mfence - memory fence */
static void riscv64_emit_mfence(ZeroCCCompiler *c) {
    /* fence iorw, iorw */
    riscv_emit_u32(c, 0x0000000F);
}

/* RISC-V 64-bit backend instance */

/* RISC-V VREG mapping: map virtual register numbers to RISC-V encodings
 * VREG_0 (result) -> a0 (x10)
 * VREG_1-7 (temps/args) -> a1-a7 (x11-x17)
 * VREG_8-15 (temps) -> t0-t7 (x5-x7, x28-x30)
 * Special: use BE(c)->fp_reg (s0/x8) and BE(c)->sp_reg (sp/x2)
 */

ZeroCCISABackend riscv64_backend = {  /* writable: ASM emit pointers are filled in lazily */
    .isa = ISA_RISCV64,
    .regs = &riscv_regs,
    .conv = &riscv64_conv,
    .fp_reg = 8,   /* s0 */
    .sp_reg = 2,   /* sp */
    .scratch_reg1 = 5,                         /* t0 */
    .scratch_reg2 = 6,                         /* t1 */
    .callee_save_frame_reserve = 96,           /* 12 callee-saved regs: s0-s11 */
    
    .emit_prologue = riscv64_emit_prologue,
    .emit_leave = riscv64_emit_leave,
    .emit_epilogue = riscv64_emit_epilogue,
    .get_code_offset = riscv64_get_code_offset,
    
    .emit_prologue_asm = riscv_emit_prologue_asm,
    .emit_epilogue_asm = riscv_emit_epilogue_asm,
    .get_hw_reg_name = riscv_get_hw_reg_name,
    .get_vec_reg_name = riscv_get_vec_reg_name,
    
    .emit_mov_imm = riscv64_emit_mov_imm,
    .emit_mov_reg = riscv64_emit_mov_reg,
    .emit_add = riscv64_emit_add,
    .emit_sub = riscv64_emit_sub,
    .emit_mul = riscv64_emit_mul,
    .emit_div = riscv64_emit_div,
    .emit_mod = riscv64_emit_mod,
    .emit_and = riscv64_emit_and,
    .emit_or = riscv64_emit_or,
    .emit_xor = riscv64_emit_xor,
    .emit_not = riscv64_emit_not,
    .emit_shl = riscv64_emit_shl,
    .emit_shr = riscv64_emit_shr,
    .emit_cmp = riscv64_emit_cmp,
    .emit_cond_set = riscv64_emit_cond_set,
    .emit_jcond = riscv64_emit_jcond,
    .emit_neg = riscv64_emit_neg,
    .emit_test = riscv64_emit_test,
    .emit_cqo = riscv64_emit_cqo,
    .emit_idiv = riscv64_emit_idiv,
    .emit_lea = riscv64_emit_lea,
    .patch_jmp = riscv64_patch_jmp,
    .resolve_labels = NULL,
    
    .emit_load = riscv64_emit_load,
    .emit_store = riscv64_emit_store,
    .emit_deref = riscv64_emit_deref,
    .emit_load_32 = riscv64_emit_load_32,
    .emit_load_16 = riscv64_emit_load_16,
    .emit_load_byte = riscv64_emit_load_byte,
    .emit_store_32 = riscv64_emit_store_32,
    .emit_store_16 = riscv64_emit_store_16,
    .emit_store_byte = riscv64_emit_store_byte,
    .emit_lea_rip = riscv64_emit_lea_rip,
    
    .emit_jmp = riscv64_emit_jmp,
    .emit_jeq = riscv64_emit_jeq,
    .emit_jne = riscv64_emit_jne,
    .emit_jlt = riscv64_emit_jlt,
    .emit_jle = riscv64_emit_jle,
    .emit_jgt = riscv64_emit_jgt,
    .emit_jge = riscv64_emit_jge,
    .emit_call = riscv64_emit_call,
    .emit_call_reg = riscv64_emit_call_reg,
    .emit_ret = riscv64_emit_ret,
    
    .emit_setl = riscv64_emit_setl,
    .emit_setle = riscv64_emit_setle,
    .emit_setg = riscv64_emit_setg,
    .emit_setge = riscv64_emit_setge,
    .emit_sete = riscv64_emit_sete,
    .emit_setne = riscv64_emit_setne,
    .emit_cmp_zero = riscv64_emit_cmp_zero,
    .emit_lnot = riscv64_emit_lnot,
    
    .emit_param_save_int = riscv64_emit_param_save_int,
    .emit_param_save_fp = riscv64_emit_param_save_fp,
    .emit_variadic_spill = riscv64_emit_variadic_spill,
    .emit_variadic_al_setup = riscv64_emit_variadic_al_setup,
    
    .emit_vla_alloc = riscv64_emit_vla_alloc,
    .emit_atomic_cmpxchg = riscv64_emit_atomic_cmpxchg,
    .emit_atomic_xadd = riscv64_emit_atomic_xadd,
    .emit_atomic_xchg = riscv64_emit_atomic_xchg,
    .emit_mfence = riscv64_emit_mfence,
    
    .get_int_reg = riscv_get_int_reg,
    .get_fp_reg = riscv_get_fp_reg,
    
    .emit_load_fp = riscv64_emit_load_fp,
    .emit_store_fp = riscv64_emit_store_fp,
    .emit_load_fp_rodata = riscv64_emit_load_fp_rodata,
    .emit_fp_add = riscv64_emit_fp_add,
    .emit_fp_sub = riscv64_emit_fp_sub,
    .emit_fp_mul = riscv64_emit_fp_mul,
    .emit_fp_div = riscv64_emit_fp_div,
    .emit_fp_neg = riscv64_emit_fp_neg,
    .emit_fp_cmp = riscv64_emit_fp_cmp,
    .emit_fp_cvtsi = riscv64_emit_fp_cvtsi,
    .emit_fp_cvtsf = riscv64_emit_fp_cvtsf,
    .emit_fp_mov = riscv64_emit_fp_mov,
    
    .emit_const = riscv64_emit_const,
    .emit_stack_adjust = riscv64_emit_stack_adjust,
    .emit_stack_align = riscv64_emit_stack_align,
    .emit_push_reg = riscv64_emit_push_reg,
    .emit_pop_reg = riscv64_emit_pop_reg,
    .emit_add_imm = riscv64_emit_add_imm,
    .emit_sub_imm = riscv64_emit_sub_imm,
    .emit_inc = riscv64_emit_inc,
    .emit_dec = riscv64_emit_dec,
    .emit_sar = riscv64_emit_sar,
    .emit_label = riscv64_emit_label,
    
    /* Assembly emission for IR instructions (Option A) */
    .emit_mov_imm_asm = riscv_emit_mov_imm_asm,
    .emit_mov_reg_asm = riscv_emit_mov_reg_asm,
    .emit_load_asm = riscv_emit_load_asm,
    .emit_store_asm = riscv_emit_store_asm,
    .emit_add_asm = riscv_emit_add_asm,
    .emit_sub_asm = riscv_emit_sub_asm,
    .emit_mul_asm = riscv_emit_mul_asm,
    .emit_div_asm = riscv_emit_div_asm,
    .emit_and_asm = riscv_emit_and_asm,
    .emit_or_asm = riscv_emit_or_asm,
    .emit_xor_asm = riscv_emit_xor_asm,
    .emit_shl_asm = riscv_emit_shl_asm,
    .emit_shr_asm = riscv_emit_shr_asm,
    .emit_sar_asm = riscv_emit_sar_asm,
    .emit_cmp_asm = riscv_emit_cmp_asm,
    .emit_cond_set_asm = riscv_emit_cond_set_asm,
    .emit_neg_asm = riscv_emit_neg_asm,
    .emit_lea_asm = riscv_emit_lea_asm,
    .emit_deref_asm = riscv_emit_deref_asm,
    .emit_call_asm = riscv_emit_call_asm,
    .emit_ret_asm = riscv_emit_ret_asm,
    .emit_label_asm = riscv_emit_label_asm,
    .emit_jmp_asm = riscv_emit_jmp_asm,
    .emit_jeq_asm = riscv_emit_jeq_asm,
    .emit_jne_asm = riscv_emit_jne_asm,
    .emit_jlt_asm = riscv_emit_jlt_asm,
    .emit_jle_asm = riscv_emit_jle_asm,
    .emit_jgt_asm = riscv_emit_jgt_asm,
    .emit_jge_asm = riscv_emit_jge_asm,
    .emit_fp_add_asm = riscv_emit_fp_add_asm,
    .emit_fp_sub_asm = riscv_emit_fp_sub_asm,
    .emit_fp_mul_asm = riscv_emit_fp_mul_asm,
    .emit_fp_div_asm = riscv_emit_fp_div_asm,
    .emit_fp_mov_asm = riscv_emit_fp_mov_asm,
    .emit_vla_alloc_asm = riscv_emit_vla_alloc_asm,
    .emit_param_save_int_asm = riscv_emit_param_save_int_asm,
    .emit_param_save_fp_asm = riscv_emit_param_save_fp_asm,
};

/* RISC-V 32-bit backend (for ESP32-C3/C6/H2) - similar but 32-bit */
static void riscv32_emit_prologue(ZeroCCCompiler *c, size_t stack_size, uint32_t callee_save_mask) {
    if (stack_size > 0) {
        uint32_t off = (stack_size + 3) & ~3;  /* align to 4 bytes */
        riscv_emit_u32(c, 0x00000113 | ((off & 0xFFF) << 20) | (2 << 7));
    }
    riscv_emit_u32(c, 0x00002023 | (1 << 15));
    riscv_emit_u32(c, 0x00002023 | (4 << 7) | (1 << 15) | (8 << 20));
    if (stack_size > 0) {
        riscv_emit_u32(c, 0x00000113 | ((stack_size & 0xFFF) << 20) | (8 << 15) | (2 << 7));
    }
    
    /* Emit callee-saved register stores (after prologue, when mask is populated) */
    /* RISC-V callee-saved registers: s0-s11 (physical regs 8-9, 18-27) */
    static const int32_t slot_offsets[] = {16, 24, 32, 40, 48, 56, 64, 72, 80, 88, 96};
    static const int regs[] = {9, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27};
    
    for (int i = 0; i < 11; i++) {
        int reg = regs[i];
        if ((callee_save_mask >> reg) & 1u) {
            /* sw reg, offset(sp) */
            riscv_emit_u32(c, 0x00002023 | ((reg & 0x1F) << 15) | ((reg & 0x1F) << 7) | ((slot_offsets[i] & 0x1FF) << 20));
        }
    }
}

static void riscv32_emit_epilogue(ZeroCCCompiler *c) {
    riscv_emit_u32(c, 0x00002003 | (1 << 15));
    riscv_emit_u32(c, 0x00002003 | (4 << 15) | (8 << 20));
    riscv_emit_u32(c, 0x00008067);
}

static void riscv32_emit_leave(ZeroCCCompiler *c) {
    /* RISC-V doesn't have a direct 'leave' instruction
     * Restore ra and s0 without adjusting sp
     */
    riscv_emit_u32(c, 0x00002003 | (1 << 15));
    riscv_emit_u32(c, 0x00002003 | (4 << 15) | (8 << 20));
}

static size_t riscv32_get_code_offset(ZeroCCCompiler *c) {
    return me_get_offset(c);
}

static void riscv32_emit_mov_imm(ZeroCCCompiler *c, int dst_reg, int64_t imm) {
    int32_t val = (int32_t)imm;
    if (val <= 2047 && val >= -2048) {
        riscv_emit_u32(c, 0x00000113 | ((val & 0xFFF) << 20) | (RV_REG(dst_reg) << 7));
    } else {
        uint32_t hi = (val + 0x800) >> 12;
        uint32_t lo = val & 0xFFF;
        if (hi != 0) {
            riscv_emit_u32(c, 0x000003B7 | ((hi & 0xFFFFF) << 12) | (RV_REG(dst_reg) << 7));
        }
        if (lo != 0) {
            riscv_emit_u32(c, 0x00000113 | (lo << 20) | (RV_REG(dst_reg) << 15) | (RV_REG(dst_reg) << 7));
        }
    }
}

static void riscv32_emit_add(ZeroCCCompiler *c, int dst, int src1, int src2) {
    riscv_emit_u32(c, 0x00000033 | (RV_REG(dst) << 7) | (RV_REG(src1) << 15) | (RV_REG(src2) << 20));
}

static void riscv32_emit_sub(ZeroCCCompiler *c, int dst, int src1, int src2) {
    riscv_emit_u32(c, 0x40000033 | (RV_REG(dst) << 7) | (RV_REG(src1) << 15) | (RV_REG(src2) << 20));
}

static void riscv32_emit_mul(ZeroCCCompiler *c, int dst, int src1, int src2) {
    riscv_emit_u32(c, 0x02000033 | (RV_REG(dst) << 7) | (RV_REG(src1) << 15) | (RV_REG(src2) << 20) | (0 << 25));
}

static void riscv32_emit_div(ZeroCCCompiler *c, int dst, int src1, int src2) {
    riscv_emit_u32(c, 0x02000033 | (RV_REG(dst) << 7) | (RV_REG(src1) << 15) | (RV_REG(src2) << 20) | (3 << 25));
}

static void riscv32_emit_and(ZeroCCCompiler *c, int dst, int src1, int src2) {
    riscv_emit_u32(c, 0x00007033 | (RV_REG(dst) << 7) | (RV_REG(src1) << 15) | (RV_REG(src2) << 20));
}

static void riscv32_emit_or(ZeroCCCompiler *c, int dst, int src1, int src2) {
    riscv_emit_u32(c, 0x00006033 | (RV_REG(dst) << 7) | (RV_REG(src1) << 15) | (RV_REG(src2) << 20));
}

static void riscv32_emit_xor(ZeroCCCompiler *c, int dst, int src1, int src2) {
    riscv_emit_u32(c, 0x00004033 | (RV_REG(dst) << 7) | (RV_REG(src1) << 15) | (RV_REG(src2) << 20));
}

static void riscv32_emit_shl(ZeroCCCompiler *c, int dst, int src, int shift) {
    if (shift >= 0 && shift < 32) {
        riscv_emit_u32(c, 0x00001013 | (RV_REG(dst) << 7) | (RV_REG(src) << 15) | (shift << 20));
    }
}

static void riscv32_emit_neg(ZeroCCCompiler *c, int dst, int src) {
    riscv_emit_u32(c, 0x40000033 | (RV_REG(dst) << 7) | (RV_REG(src) << 20));
}

static void riscv32_emit_load(ZeroCCCompiler *c, int dst_reg, int base_reg, int32_t offset) {
    riscv_emit_u32(c, 0x00000003 | (RV_REG(dst_reg) << 7) | (RV_REG(base_reg) << 15) | ((offset & 0xFFF) << 20));
}

static void riscv32_emit_store(ZeroCCCompiler *c, int src_reg, int base_reg, int32_t offset) {
    riscv_emit_u32(c, 0x00002023 | (RV_REG(base_reg) << 15) | (RV_REG(src_reg) << 20) | ((offset & 0xFFF) << 7));
}

static size_t riscv32_emit_jmp(ZeroCCCompiler *c, const char *label) {
    size_t patch_pos = me_get_offset(c);
    riscv_emit_u32(c, 0x0000006F);
    (void)label;
    return patch_pos;
}

static size_t riscv32_emit_jeq(ZeroCCCompiler *c, const char *label) {
    size_t patch_pos = me_get_offset(c);
    riscv_emit_u32(c, 0x00000063);
    (void)label;
    return patch_pos;
}

static size_t riscv32_emit_jne(ZeroCCCompiler *c, const char *label) {
    size_t patch_pos = me_get_offset(c);
    riscv_emit_u32(c, 0x00001063);
    (void)label;
    return patch_pos;
}

static size_t riscv32_emit_jlt(ZeroCCCompiler *c, const char *label) {
    size_t patch_pos = me_get_offset(c);
    riscv_emit_u32(c, 0x00004063);
    (void)label;
    return patch_pos;
}

static size_t riscv32_emit_jle(ZeroCCCompiler *c, const char *label) {
    size_t patch_pos = me_get_offset(c);
    riscv_emit_u32(c, 0x00006063);
    (void)label;
    return patch_pos;
}

static size_t riscv32_emit_jgt(ZeroCCCompiler *c, const char *label) {
    size_t patch_pos = me_get_offset(c);
    riscv_emit_u32(c, 0x00005063);
    (void)label;
    return patch_pos;
}

static size_t riscv32_emit_jge(ZeroCCCompiler *c, const char *label) {
    size_t patch_pos = me_get_offset(c);
    riscv_emit_u32(c, 0x00007063);
    (void)label;
    return patch_pos;
}

static void riscv32_emit_call(ZeroCCCompiler *c, const char *func_name) {
    riscv_emit_u32(c, 0x000000EF | (1 << 7));
    (void)func_name;
}

static void riscv32_emit_ret(ZeroCCCompiler *c) {
    riscv_emit_u32(c, 0x00008067);
}

/* sar dst_reg, src1, src2 (arithmetic shift right: dst = src1 >> src2) */
static void riscv32_emit_sar(ZeroCCCompiler *c, int dst_reg, int src1, int src2) {
    if (dst_reg != src1) {
        /* mv dst, src1 */
        riscv_emit_u32(c, 0x00000033 | (RV_REG(dst_reg) << 7) | (RV_REG(src1) << 15));
    }
    /* sra dst, dst, src2 */
    riscv_emit_u32(c, 0x40000033 | (RV_REG(dst_reg) << 7) | (RV_REG(dst_reg) << 15) | (RV_REG(src2) << 20));
}

/* call [reg] - call through register */
static void riscv32_emit_call_reg(ZeroCCCompiler *c, int reg) {
    /* jalr ra, 0(reg) */
    riscv_emit_u32(c, 0x00000067 | (RV_REG(reg) << 15) | (1 << 7));
}

/* stack_align - ensure 16-byte stack alignment (RISC-V already handles this) */
static void riscv32_emit_stack_align(ZeroCCCompiler *c, uint8_t alignment) {
    /* RISC-V ABI requires 16-byte alignment. 
     * This is typically handled by prologue/epilogue. */
    (void)c;
    (void)alignment;
}

/* add_imm dst_reg, imm (add immediate) */
static void riscv32_emit_add_imm(ZeroCCCompiler *c, int dst_reg, int16_t imm) {
    if (imm >= 0) {
        /* addi dst, dst, imm */
        riscv_emit_u32(c, ((imm & 0xFFF) << 20) | (RV_REG(dst_reg) << 7) | (RV_REG(dst_reg) << 15));
    } else {
        /* addi dst, dst, imm (negative) */
        riscv_emit_u32(c, ((imm & 0xFFF) << 20) | (RV_REG(dst_reg) << 7) | (RV_REG(dst_reg) << 15));
    }
}

/* sub_imm dst_reg, imm (subtract immediate) */
static void riscv32_emit_sub_imm(ZeroCCCompiler *c, int dst_reg, int16_t imm) {
    if (imm >= 0) {
        /* addi dst, dst, -imm */
        riscv_emit_u32(c, ((-imm & 0xFFF) << 20) | (RV_REG(dst_reg) << 7) | (RV_REG(dst_reg) << 15) | (0x400 << 20));
    } else {
        /* addi dst, dst, -imm (negative = add) */
        riscv_emit_u32(c, ((-imm & 0xFFF) << 20) | (RV_REG(dst_reg) << 7) | (RV_REG(dst_reg) << 15));
    }
}

/* inc dst_reg (increment) */
static void riscv32_emit_inc(ZeroCCCompiler *c, int dst_reg) {
    /* addi dst, dst, 1 */
    riscv_emit_u32(c, (1 << 20) | (RV_REG(dst_reg) << 7) | (RV_REG(dst_reg) << 15));
}

/* dec dst_reg (decrement) */
static void riscv32_emit_dec(ZeroCCCompiler *c, int dst_reg) {
    /* addi dst, dst, -1 */
    riscv_emit_u32(c, ((-1 & 0xFFF) << 20) | (RV_REG(dst_reg) << 7) | (RV_REG(dst_reg) << 15));
}

static void riscv32_emit_load_fp(ZeroCCCompiler *c, int dst_reg, int base_reg, int32_t offset) {
    riscv_emit_u32(c, 0x00000007 | (RV_REG(dst_reg) << 7) | (RV_REG(base_reg) << 15) | ((offset & 0xFFF) << 20) | (3 << 25));
}

static void riscv32_emit_store_fp(ZeroCCCompiler *c, int src_reg, int base_reg, int32_t offset) {
    riscv_emit_u32(c, 0x00002027 | (RV_REG(base_reg) << 15) | (RV_REG(src_reg) << 20) | ((offset & 0xFFF) << 7) | (3 << 25));
}

static void riscv32_emit_load_fp_rodata(ZeroCCCompiler *c, int dst_reg, size_t rodata_offset) {
    (void)c; (void)dst_reg; (void)rodata_offset;
}

static void riscv32_emit_fp_add(ZeroCCCompiler *c, int dst, int src1, int src2) {
    riscv_emit_u32(c, 0x00000053 | (dst << 7) | (src1 << 15) | (src2 << 20));
}

static void riscv32_emit_fp_sub(ZeroCCCompiler *c, int dst, int src1, int src2) {
    riscv_emit_u32(c, 0x00001053 | (dst << 7) | (src1 << 15) | (src2 << 20));
}

static void riscv32_emit_fp_mul(ZeroCCCompiler *c, int dst, int src1, int src2) {
    riscv_emit_u32(c, 0x00002053 | (dst << 7) | (src1 << 15) | (src2 << 20));
}

static void riscv32_emit_fp_div(ZeroCCCompiler *c, int dst, int src1, int src2) {
    riscv_emit_u32(c, 0x00003053 | (dst << 7) | (src1 << 15) | (src2 << 20));
}

static void riscv32_emit_fp_neg(ZeroCCCompiler *c, int dst, int src) {
    riscv_emit_u32(c, 0x00005053 | (dst << 7) | (src << 15) | (src << 20));
}

static void riscv32_emit_fp_cmp(ZeroCCCompiler *c, int src1, int src2) {
    riscv_emit_u32(c, 0x0000A053 | (src2 << 7) | (src1 << 15) | (1 << 25));
}

static void riscv32_emit_fp_cvtsi(ZeroCCCompiler *c, int int_dst, int fp_src) {
    riscv_emit_u32(c, 0x00006053 | (int_dst << 7) | (fp_src << 15) | (1 << 25));
}

static void riscv32_emit_fp_cvtsf(ZeroCCCompiler *c, int fp_dst, int int_src) {
    riscv_emit_u32(c, 0x00004053 | (fp_dst << 7) | (int_src << 15) | (1 << 25));
}

static void riscv32_emit_fp_mov(ZeroCCCompiler *c, int dst, int src) {
    if (dst == src) return;
    riscv_emit_u32(c, 0x00000053 | (dst << 7) | (src << 15) | (src << 20));
}

static void riscv32_emit_const(ZeroCCCompiler *c, int64_t value) {
    riscv32_emit_mov_imm(c, 10, value);
}

static void riscv32_emit_stack_adjust(ZeroCCCompiler *c, int32_t delta) {
    if (delta == 0) return;
    if (delta > 0) {
        riscv_emit_u32(c, 0x00000113 | ((delta & 0xFFF) << 20) | (2 << 7));
    } else {
        riscv_emit_u32(c, 0x00000113 | (((-delta) & 0xFFF) << 20) | (2 << 7));
    }
}

static void riscv32_emit_push_reg(ZeroCCCompiler *c, int reg) {
    riscv_emit_u32(c, 0xFF810113);  /* addi sp, sp, -4 */
    riscv_emit_u32(c, 0x00002023 | (reg << 20) | (0 << 7));  /* sw reg, 0(sp) */
}

static void riscv32_emit_pop_reg(ZeroCCCompiler *c, int reg) {
    riscv_emit_u32(c, 0x00000003 | (reg << 7) | (0 << 20) | (2 << 15));  /* lw reg, 0(sp) */
    riscv_emit_u32(c, 0x00410113);  /* addi sp, sp, 4 */
}

static void riscv32_emit_label(ZeroCCCompiler *c, const char *name) {
    (void)c; (void)name;
}



static size_t riscv32_patch_jmp(ZeroCCCompiler *c, size_t patch_offset, size_t target_offset) {
    int32_t rel = (int32_t)(target_offset - patch_offset);
    if (patch_offset + 4 <= c->text_section.length) {
        uint8_t *buf = (uint8_t *)c->text_section.data;
        uint32_t *insn = (uint32_t*)(buf + patch_offset);
        *insn = (*insn & 0xFFF000FF) | ((rel & 0xFFFFF) << 12);
    }
    return patch_offset;
}



static void riscv32_emit_test(ZeroCCCompiler *c, int src1, int src2) {
    /* and t0, src1, src2 */
    riscv_emit_u32(c, 0x00007033 | (5 << 7) | (src1 << 15) | (src2 << 20));
}

static void riscv32_emit_cqo(ZeroCCCompiler *c) {
    (void)c;
}

static void riscv32_emit_idiv(ZeroCCCompiler *c, int src) {
    /* div a0, a0, src */
    riscv_emit_u32(c, 0x02000033 | (10 << 7) | (10 << 15) | (src << 20) | (3 << 25));
}

static void riscv32_emit_lea(ZeroCCCompiler *c, int dst, int base, int32_t offset) {
    /* addi dst, base, offset */
    riscv_emit_u32(c, 0x00000113 | ((offset & 0xFFF) << 20) | ((base & 0x1F) << 15) | ((dst & 0x1F) << 7));
}



static void riscv32_emit_mod(ZeroCCCompiler *c, int dst, int src1, int src2) {
    /* Modulo: dst = src1 % src2
     * RISC-V rem instruction */
    riscv_emit_u32(c, 0x02000033 | (dst << 7) | (src1 << 15) | (src2 << 20) | (7 << 25));
}

/* RISC-V 32-bit backend instance */
ZeroCCISABackend riscv32_backend = {  /* writable: ASM emit pointers are filled in lazily */
    .isa = ISA_RISCV32,
    .regs = &riscv_regs,
    .conv = &riscv32_conv,
    .fp_reg = 8,
    .sp_reg = 2,
    .scratch_reg1 = 5,                         /* t0 (non-argument/return) */
    .scratch_reg2 = 6,                         /* t1 (non-argument/return) */
    .callee_save_frame_reserve = 96,           /* 12 callee-saved regs: s0-s11 */
    
    .emit_prologue = riscv32_emit_prologue,
    .emit_leave = riscv32_emit_leave,
    .emit_epilogue = riscv32_emit_epilogue,
    .get_code_offset = riscv32_get_code_offset,
    
    .emit_mov_imm = riscv32_emit_mov_imm,
    .emit_add = riscv32_emit_add,
    .emit_sub = riscv32_emit_sub,
    .emit_mul = riscv32_emit_mul,
    .emit_div = riscv32_emit_div,
    .emit_mod = riscv32_emit_mod,
    .emit_and = riscv32_emit_and,
    .emit_or = riscv32_emit_or,
    .emit_xor = riscv32_emit_xor,
    .emit_shl = riscv32_emit_shl,
    .emit_cmp = riscv64_emit_cmp,
    .emit_cond_set = riscv64_emit_cond_set,
    .emit_jcond = riscv64_emit_jcond,
    .emit_neg = riscv32_emit_neg,
    .emit_test = riscv32_emit_test,
    .emit_cqo = riscv32_emit_cqo,
    .emit_idiv = riscv32_emit_idiv,
    .emit_lea = riscv32_emit_lea,
    .patch_jmp = riscv32_patch_jmp,
    .resolve_labels = NULL,  /* No label resolution needed for now */
    
    /* VREG mapping */
    .get_int_reg = riscv_get_int_reg,
    .get_fp_reg = riscv_get_fp_reg,
    
    .emit_load = riscv32_emit_load,
    .emit_store = riscv32_emit_store,
    
    .emit_jmp = riscv32_emit_jmp,
    .emit_jeq = riscv32_emit_jeq,
    .emit_jne = riscv32_emit_jne,
    .emit_jlt = riscv32_emit_jlt,
    .emit_jle = riscv32_emit_jle,
    .emit_jgt = riscv32_emit_jgt,
    .emit_jge = riscv32_emit_jge,
    .emit_call = riscv32_emit_call,
    .emit_call_reg = riscv32_emit_call_reg,
    .emit_ret = riscv32_emit_ret,
    
    .emit_load_fp = riscv32_emit_load_fp,
    .emit_store_fp = riscv32_emit_store_fp,
    .emit_load_fp_rodata = riscv32_emit_load_fp_rodata,
    .emit_fp_add = riscv32_emit_fp_add,
    .emit_fp_sub = riscv32_emit_fp_sub,
    .emit_fp_mul = riscv32_emit_fp_mul,
    .emit_fp_div = riscv32_emit_fp_div,
    .emit_fp_neg = riscv32_emit_fp_neg,
    .emit_fp_cmp = riscv32_emit_fp_cmp,
    .emit_fp_cvtsi = riscv32_emit_fp_cvtsi,
    .emit_fp_cvtsf = riscv32_emit_fp_cvtsf,
    .emit_fp_mov = riscv32_emit_fp_mov,
    
    .emit_const = riscv32_emit_const,
    .emit_stack_adjust = riscv32_emit_stack_adjust,
    .emit_stack_align = riscv32_emit_stack_align,
    .emit_push_reg = riscv32_emit_push_reg,
    .emit_pop_reg = riscv32_emit_pop_reg,
    .emit_add_imm = riscv32_emit_add_imm,
    .emit_sub_imm = riscv32_emit_sub_imm,
    .emit_inc = riscv32_emit_inc,
    .emit_dec = riscv32_emit_dec,
    .emit_sar = riscv32_emit_sar,
    .emit_label = riscv32_emit_label,
};
