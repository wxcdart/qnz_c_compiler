#include "zero_cc/isa_backend.h"
#include "zero_cc/parser/machine_emitter.h"
#include <stdint.h>

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
};

/* RISC-V instruction encoding helpers */
static inline void riscv_emit_u32(ZeroCCCompiler *c, uint32_t insn) {
    me_emit_u8(c, insn & 0xFF);
    me_emit_u8(c, (insn >> 8) & 0xFF);
    me_emit_u8(c, (insn >> 16) & 0xFF);
    me_emit_u8(c, (insn >> 24) & 0xFF);
}

/* RISC-V RV64 backend implementation */
static void riscv64_emit_prologue(ZeroCCCompiler *c, size_t stack_size) {
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

static void riscv64_emit_mov_imm(ZeroCCCompiler *c, int dst_reg, int64_t imm) {
    /* Use lui + addi sequence for 64-bit immediates */
    if (imm <= 2047 && imm >= -2048) {
        riscv_emit_u32(c, 0x00000113 | (((uint64_t)imm & 0xFFF) << 20) | (dst_reg << 7));
    } else {
        /* For larger immediates, use lui + addi */
        uint64_t hi = (imm + 0x800) >> 12;
        uint64_t lo = imm & 0xFFF;
        if (hi != 0) {
            riscv_emit_u32(c, 0x000003B7 | ((hi & 0xFFFFF) << 12) | (dst_reg << 7));  /* lui */
        }
        if (lo != 0) {
            riscv_emit_u32(c, 0x00000113 | (lo << 20) | (dst_reg << 15) | (dst_reg << 7));  /* addi */
        }
    }
}

static void riscv64_emit_add(ZeroCCCompiler *c, int dst, int src1, int src2) {
    /* add dst, src1, src2 */
    riscv_emit_u32(c, 0x00000033 | (dst << 7) | (src1 << 15) | (src2 << 20));
}

static void riscv64_emit_sub(ZeroCCCompiler *c, int dst, int src1, int src2) {
    /* sub dst, src1, src2 */
    riscv_emit_u32(c, 0x40000033 | (dst << 7) | (src1 << 15) | (src2 << 20));
}

static void riscv64_emit_mul(ZeroCCCompiler *c, int dst, int src1, int src2) {
    /* mul dst, src1, src2 (M extension) */
    riscv_emit_u32(c, 0x02000033 | (dst << 7) | (src1 << 15) | (src2 << 20) | (0 << 25));
}

static void riscv64_emit_div(ZeroCCCompiler *c, int dst, int src1, int src2) {
    /* div dst, src1, src2 (M extension) */
    riscv_emit_u32(c, 0x02000033 | (dst << 7) | (src1 << 15) | (src2 << 20) | (3 << 25));
}

static void riscv64_emit_and(ZeroCCCompiler *c, int dst, int src1, int src2) {
    riscv_emit_u32(c, 0x00007033 | (dst << 7) | (src1 << 15) | (src2 << 20));
}

static void riscv64_emit_or(ZeroCCCompiler *c, int dst, int src1, int src2) {
    riscv_emit_u32(c, 0x00006033 | (dst << 7) | (src1 << 15) | (src2 << 20));
}

static void riscv64_emit_xor(ZeroCCCompiler *c, int dst, int src1, int src2) {
    riscv_emit_u32(c, 0x00004033 | (dst << 7) | (src1 << 15) | (src2 << 20));
}

static void riscv64_emit_shl(ZeroCCCompiler *c, int dst, int src, int shift) {
    if (shift >= 0 && shift < 64) {
        riscv_emit_u32(c, 0x00001013 | (dst << 7) | (src << 15) | (shift << 20));
    }
}

static void riscv64_emit_cmp(ZeroCCCompiler *c, int src1, int src2) {
    /* Use slt/sltu to set a temporary register, or just use branch directly */
    /* For now, we'll use conditional branches directly */
    (void)src1; (void)src2;
}

static void riscv64_emit_neg(ZeroCCCompiler *c, int dst, int src) {
    /* sub dst, zero, src */
    riscv_emit_u32(c, 0x40000033 | (dst << 7) | (src << 20));
}

static void riscv64_emit_load(ZeroCCCompiler *c, int dst_reg, int base_reg, int32_t offset) {
    /* ld dst, offset(base) */
    riscv_emit_u32(c, 0x00000003 | (dst_reg << 7) | (base_reg << 15) | ((offset & 0xFFF) << 20));
}

static void riscv64_emit_store(ZeroCCCompiler *c, int src_reg, int base_reg, int32_t offset) {
    /* sd src, offset(base) */
    riscv_emit_u32(c, 0x00002023 | (base_reg << 15) | (src_reg << 20) | ((offset & 0xFFF) << 7));
}

static void riscv64_emit_jmp(ZeroCCCompiler *c, const char *label) {
    /* jal zero, offset */
    riscv_emit_u32(c, 0x0000006F);
    (void)label;
}

static void riscv64_emit_jeq(ZeroCCCompiler *c, const char *label) {
    /* beq x, zero, label */
    riscv_emit_u32(c, 0x00000063);
    (void)label;
}

static void riscv64_emit_jne(ZeroCCCompiler *c, const char *label) {
    /* bne x, zero, label */
    riscv_emit_u32(c, 0x00001063);
    (void)label;
}

static void riscv64_emit_jlt(ZeroCCCompiler *c, const char *label) {
    /* blt x, zero, label */
    riscv_emit_u32(c, 0x00004063);
    (void)label;
}

static void riscv64_emit_jle(ZeroCCCompiler *c, const char *label) {
    /* ble x, zero, label */
    riscv_emit_u32(c, 0x00006063);
    (void)label;
}

static void riscv64_emit_jgt(ZeroCCCompiler *c, const char *label) {
    /* bgt x, zero, label */
    riscv_emit_u32(c, 0x00005063);
    (void)label;
}

static void riscv64_emit_jge(ZeroCCCompiler *c, const char *label) {
    /* bge x, zero, label */
    riscv_emit_u32(c, 0x00007063);
    (void)label;
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

/* Floating point operations */
static void riscv64_emit_load_fp(ZeroCCCompiler *c, int dst_reg, int base_reg, int32_t offset) {
    /* fld fa, offset(base) */
    riscv_emit_u32(c, 0x00000007 | (dst_reg << 7) | (base_reg << 15) | ((offset & 0xFFF) << 20) | (3 << 25));
}

static void riscv64_emit_store_fp(ZeroCCCompiler *c, int src_reg, int base_reg, int32_t offset) {
    /* fsd fa, offset(base) */
    riscv_emit_u32(c, 0x00002027 | (base_reg << 15) | (src_reg << 20) | ((offset & 0xFFF) << 7) | (3 << 25));
}

static void riscv64_emit_load_fp_imm(ZeroCCCompiler *c, int dst_reg, double value) {
    /* Use literal pool - store to memory and load */
    (void)dst_reg; (void)value;
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

/* RISC-V 64-bit backend instance */
const ZeroCCISABackend riscv64_backend = {
    .isa = ISA_RISCV64,
    .regs = &riscv_regs,
    .fp_reg = 8,   /* s0 */
    .sp_reg = 2,   /* sp */
    
    .emit_prologue = riscv64_emit_prologue,
    .emit_epilogue = riscv64_emit_epilogue,
    
    .emit_mov_imm = riscv64_emit_mov_imm,
    .emit_add = riscv64_emit_add,
    .emit_sub = riscv64_emit_sub,
    .emit_mul = riscv64_emit_mul,
    .emit_div = riscv64_emit_div,
    .emit_and = riscv64_emit_and,
    .emit_or = riscv64_emit_or,
    .emit_xor = riscv64_emit_xor,
    .emit_shl = riscv64_emit_shl,
    .emit_cmp = riscv64_emit_cmp,
    .emit_neg = riscv64_emit_neg,
    
    .emit_load = riscv64_emit_load,
    .emit_store = riscv64_emit_store,
    
    .emit_jmp = riscv64_emit_jmp,
    .emit_jeq = riscv64_emit_jeq,
    .emit_jne = riscv64_emit_jne,
    .emit_jlt = riscv64_emit_jlt,
    .emit_jle = riscv64_emit_jle,
    .emit_jgt = riscv64_emit_jgt,
    .emit_jge = riscv64_emit_jge,
    .emit_call = riscv64_emit_call,
    .emit_ret = riscv64_emit_ret,
    
    .emit_load_fp = riscv64_emit_load_fp,
    .emit_store_fp = riscv64_emit_store_fp,
    .emit_load_fp_imm = riscv64_emit_load_fp_imm,
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
    .emit_push_reg = riscv64_emit_push_reg,
    .emit_pop_reg = riscv64_emit_pop_reg,
    .emit_label = riscv64_emit_label,
};

/* RISC-V 32-bit backend (for ESP32-C3/C6/H2) - similar but 32-bit */
static void riscv32_emit_prologue(ZeroCCCompiler *c, size_t stack_size) {
    if (stack_size > 0) {
        uint32_t off = (stack_size + 3) & ~3;  /* align to 4 bytes */
        riscv_emit_u32(c, 0x00000113 | ((off & 0xFFF) << 20) | (2 << 7));
    }
    riscv_emit_u32(c, 0x00002023 | (1 << 15));
    riscv_emit_u32(c, 0x00002023 | (4 << 7) | (1 << 15) | (8 << 20));
    if (stack_size > 0) {
        riscv_emit_u32(c, 0x00000113 | ((stack_size & 0xFFF) << 20) | (8 << 15) | (2 << 7));
    }
}

static void riscv32_emit_epilogue(ZeroCCCompiler *c) {
    riscv_emit_u32(c, 0x00002003 | (1 << 15));
    riscv_emit_u32(c, 0x00002003 | (4 << 15) | (8 << 20));
    riscv_emit_u32(c, 0x00008067);
}

static void riscv32_emit_mov_imm(ZeroCCCompiler *c, int dst_reg, int64_t imm) {
    int32_t val = (int32_t)imm;
    if (val <= 2047 && val >= -2048) {
        riscv_emit_u32(c, 0x00000113 | ((val & 0xFFF) << 20) | (dst_reg << 7));
    } else {
        uint32_t hi = (val + 0x800) >> 12;
        uint32_t lo = val & 0xFFF;
        if (hi != 0) {
            riscv_emit_u32(c, 0x000003B7 | ((hi & 0xFFFFF) << 12) | (dst_reg << 7));
        }
        if (lo != 0) {
            riscv_emit_u32(c, 0x00000113 | (lo << 20) | (dst_reg << 15) | (dst_reg << 7));
        }
    }
}

static void riscv32_emit_add(ZeroCCCompiler *c, int dst, int src1, int src2) {
    riscv_emit_u32(c, 0x00000033 | (dst << 7) | (src1 << 15) | (src2 << 20));
}

static void riscv32_emit_sub(ZeroCCCompiler *c, int dst, int src1, int src2) {
    riscv_emit_u32(c, 0x40000033 | (dst << 7) | (src1 << 15) | (src2 << 20));
}

static void riscv32_emit_mul(ZeroCCCompiler *c, int dst, int src1, int src2) {
    riscv_emit_u32(c, 0x02000033 | (dst << 7) | (src1 << 15) | (src2 << 20) | (0 << 25));
}

static void riscv32_emit_div(ZeroCCCompiler *c, int dst, int src1, int src2) {
    riscv_emit_u32(c, 0x02000033 | (dst << 7) | (src1 << 15) | (src2 << 20) | (3 << 25));
}

static void riscv32_emit_and(ZeroCCCompiler *c, int dst, int src1, int src2) {
    riscv_emit_u32(c, 0x00007033 | (dst << 7) | (src1 << 15) | (src2 << 20));
}

static void riscv32_emit_or(ZeroCCCompiler *c, int dst, int src1, int src2) {
    riscv_emit_u32(c, 0x00006033 | (dst << 7) | (src1 << 15) | (src2 << 20));
}

static void riscv32_emit_xor(ZeroCCCompiler *c, int dst, int src1, int src2) {
    riscv_emit_u32(c, 0x00004033 | (dst << 7) | (src1 << 15) | (src2 << 20));
}

static void riscv32_emit_shl(ZeroCCCompiler *c, int dst, int src, int shift) {
    if (shift >= 0 && shift < 32) {
        riscv_emit_u32(c, 0x00001013 | (dst << 7) | (src << 15) | (shift << 20));
    }
}

static void riscv32_emit_neg(ZeroCCCompiler *c, int dst, int src) {
    riscv_emit_u32(c, 0x40000033 | (dst << 7) | (src << 20));
}

static void riscv32_emit_load(ZeroCCCompiler *c, int dst_reg, int base_reg, int32_t offset) {
    riscv_emit_u32(c, 0x00000003 | (dst_reg << 7) | (base_reg << 15) | ((offset & 0xFFF) << 20));
}

static void riscv32_emit_store(ZeroCCCompiler *c, int src_reg, int base_reg, int32_t offset) {
    riscv_emit_u32(c, 0x00002023 | (base_reg << 15) | (src_reg << 20) | ((offset & 0xFFF) << 7));
}

static void riscv32_emit_jmp(ZeroCCCompiler *c, const char *label) {
    riscv_emit_u32(c, 0x0000006F);
    (void)label;
}

static void riscv32_emit_jeq(ZeroCCCompiler *c, const char *label) {
    riscv_emit_u32(c, 0x00000063);
    (void)label;
}

static void riscv32_emit_jne(ZeroCCCompiler *c, const char *label) {
    riscv_emit_u32(c, 0x00001063);
    (void)label;
}

static void riscv32_emit_jlt(ZeroCCCompiler *c, const char *label) {
    riscv_emit_u32(c, 0x00004063);
    (void)label;
}

static void riscv32_emit_jle(ZeroCCCompiler *c, const char *label) {
    riscv_emit_u32(c, 0x00006063);
    (void)label;
}

static void riscv32_emit_jgt(ZeroCCCompiler *c, const char *label) {
    riscv_emit_u32(c, 0x00005063);
    (void)label;
}

static void riscv32_emit_jge(ZeroCCCompiler *c, const char *label) {
    riscv_emit_u32(c, 0x00007063);
    (void)label;
}

static void riscv32_emit_call(ZeroCCCompiler *c, const char *func_name) {
    riscv_emit_u32(c, 0x000000EF | (1 << 7));
    (void)func_name;
}

static void riscv32_emit_ret(ZeroCCCompiler *c) {
    riscv_emit_u32(c, 0x00008067);
}

static void riscv32_emit_load_fp(ZeroCCCompiler *c, int dst_reg, int base_reg, int32_t offset) {
    riscv_emit_u32(c, 0x00000007 | (dst_reg << 7) | (base_reg << 15) | ((offset & 0xFFF) << 20) | (3 << 25));
}

static void riscv32_emit_store_fp(ZeroCCCompiler *c, int src_reg, int base_reg, int32_t offset) {
    riscv_emit_u32(c, 0x00002027 | (base_reg << 15) | (src_reg << 20) | ((offset & 0xFFF) << 7) | (3 << 25));
}

static void riscv32_emit_load_fp_imm(ZeroCCCompiler *c, int dst_reg, double value) {
    (void)dst_reg; (void)value;
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

/* RISC-V 32-bit backend instance */
const ZeroCCISABackend riscv32_backend = {
    .isa = ISA_RISCV32,
    .regs = &riscv_regs,
    .fp_reg = 8,
    .sp_reg = 2,
    
    .emit_prologue = riscv32_emit_prologue,
    .emit_epilogue = riscv32_emit_epilogue,
    
    .emit_mov_imm = riscv32_emit_mov_imm,
    .emit_add = riscv32_emit_add,
    .emit_sub = riscv32_emit_sub,
    .emit_mul = riscv32_emit_mul,
    .emit_div = riscv32_emit_div,
    .emit_and = riscv32_emit_and,
    .emit_or = riscv32_emit_or,
    .emit_xor = riscv32_emit_xor,
    .emit_shl = riscv32_emit_shl,
    .emit_cmp = riscv64_emit_cmp,
    .emit_neg = riscv32_emit_neg,
    
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
    .emit_ret = riscv32_emit_ret,
    
    .emit_load_fp = riscv32_emit_load_fp,
    .emit_store_fp = riscv32_emit_store_fp,
    .emit_load_fp_imm = riscv32_emit_load_fp_imm,
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
    .emit_push_reg = riscv32_emit_push_reg,
    .emit_pop_reg = riscv32_emit_pop_reg,
    .emit_label = riscv32_emit_label,
};