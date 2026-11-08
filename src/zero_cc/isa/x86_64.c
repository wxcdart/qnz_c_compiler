#include "zero_cc/isa/isa_backend.h"
#include "zero_cc/ir/machine_emitter.h"
#include "zero_cc/isa/x86_64_asm.c"  /* Include assembly emission functions */
#include <string.h>

/* x86-64 register info */
static const ISARegInfo x86_64_regs = {
    .int_regs = {
        "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "r8", "r9",
        "r10", "r11", "r12", "r13", "r14", "r15", "rsp", "rbp",
    },
    .fp_regs = {
        "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7",
        "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15",
    },
    .int_arg_regs = {"rdi", "rsi", "rdx", "rcx", "r8", "r9"},
    .fp_arg_regs = {"xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7"},
    .int_ret_reg = "rax",
    .fp_ret_reg = "xmm0",
    .num_int_regs = 16,
    .num_fp_regs = 16,
    .vec_regs = {"xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15"},
    .num_vec_regs = 16,
};

/* x86-64 System V ABI calling convention */
/* Register numbers: 0=rax 1=rcx 2=rdx 3=rbx 4=rsp 5=rbp 6=rsi 7=rdi 8-15=r8-r15 */

static const int x86_64_int_arg_regs[] = {7, 6, 2, 1, 8, 9};  /* rdi, rsi, rdx, rcx, r8, r9 */
static const int x86_64_fp_arg_regs[] = {0, 1, 2, 3, 4, 5};  /* xmm0-xmm5 (SysV: 6 FP arg regs) */
static const int x86_64_callee_saved_gpr[] = {3, 12, 13, 14, 15};  /* rbx, r12-r15 (rbp handled by prologue/epilogue) */

/* Callee-save slot offsets (indexed by array position in callee_saved_gpr) */
static const int32_t x86_64_callee_save_slot_offset[] = {
    /* rbx */ 8,
    /* r12 */ 16,
    /* r13 */ 24,
    /* r14 */ 32,
    /* r15 */ 40,
};

static const ISACallingConvention x86_64_conv = {
    .int_arg_regs = x86_64_int_arg_regs,
    .int_arg_reg_count = 6,
    .fp_arg_regs = x86_64_fp_arg_regs,
    .fp_arg_reg_count = 6,
    .callee_saved_gpr = x86_64_callee_saved_gpr,
    .callee_saved_gpr_count = 5,
    .callee_save_slot_offsets = x86_64_callee_save_slot_offset,  /* indexed by array position */
};

/* Register numbers used by this backend are the hardware encodings:
 * 0=rax 1=rcx 2=rdx 3=rbx 4=rsp 5=rbp 6=rsi 7=rdi 8..15=r8..r15 */
#define X86_RAX 0
#define X86_RDX 2
#define X86_RSP 4
#define X86_RBP 5
#define X86_R11 11

/* Helper: emit ModR/M byte for x86-64 */
static inline void x86_modrm(ZeroCCCompiler *c, int mod, int reg, int rm) {
    me_emit_u8(c, (mod << 6) | ((reg & 7) << 3) | (rm & 7));
}

/* REX prefix: w=64-bit, r extends ModRM.reg, x extends SIB.index, b extends ModRM.rm/base */
static inline void x86_rex(ZeroCCCompiler *c, int w, int r, int x, int b) {
    if (w || r || x || b) {
        me_emit_u8(c, 0x40 | (w << 3) | (r << 2) | (x << 1) | b);
    }
}

/* Emit a 64-bit register-register instruction: REX.W <op...> ModRM(11, reg, rm) */
static void x86_rr(ZeroCCCompiler *c, const uint8_t *op, int oplen, int reg, int rm) {
    x86_rex(c, 1, (reg >> 3) & 1, 0, (rm >> 3) & 1);
    for (int i = 0; i < oplen; i++) me_emit_u8(c, op[i]);
    x86_modrm(c, 3, reg, rm);
}

/* Emit a 64-bit register-memory instruction: REX.W <op> ModRM/SIB [base + disp] */
static void x86_rm(ZeroCCCompiler *c, uint8_t op, int reg, int base, int32_t disp) {
    x86_rex(c, 1, (reg >> 3) & 1, 0, (base >> 3) & 1);
    me_emit_u8(c, op);
    int mod;
    if (disp == 0 && (base & 7) != X86_RBP) mod = 0;          /* [base] (rbp/r13 need a disp) */
    else if (disp >= -128 && disp <= 127)  mod = 1;           /* [base + disp8] */
    else                                   mod = 2;           /* [base + disp32] */
    x86_modrm(c, mod, reg, base);
    if ((base & 7) == X86_RSP) me_emit_u8(c, 0x24);           /* SIB: base=rsp/r12, no index */
    if (mod == 1) me_emit_u8(c, (uint8_t)(int8_t)disp);
    else if (mod == 2) me_emit_u32(c, (uint32_t)disp);
}

/* F7 /4: mul [base+disp] -- rdx:rax = rax * [base+disp]. Used by the
 * spilled-MUL128 path where one factor lives in a frame slot. */
static void x86_mul_mem(ZeroCCCompiler *c, int base, int32_t disp) {
    x86_rm(c, 0xF7, 4, base, disp);
}

/* 01 /00: add [base+disp], reg. Used by the spilled-MUL128 path to
 * accumulate partial products into the d_hi frame slot. */
static void x86_add_mem(ZeroCCCompiler *c, int reg, int base, int32_t disp) {
    x86_rm(c, 0x01, reg, base, disp);
}

/* Spilled MUL128 (Sprint 109): all three operands live in frame slots.
 * Only rax/rdx are used as working registers (both are reserved around
 * OP_MUL128 by the VREG_0/VREG_2 pseudo-intervals, so no live value can
 * be in them). */
static void x86_emit_mul128_spilled(ZeroCCCompiler *c,
                                    int32_t d_lo, int32_t d_hi,
                                    int32_t a_lo, int32_t a_hi,
                                    int32_t b_lo, int32_t b_hi) {
    int base = zero_cc_frame_base_reg(c);
    int rax = 0, rdx = 2;
    x86_rm(c, 0x8B, rax, base, a_lo);   /* rax = a_lo */
    x86_mul_mem(c, base, b_lo);         /* rdx:rax = a_lo * b_lo (P0) */
    x86_rm(c, 0x89, rax, base, d_lo);   /* [d_lo] = P0.lo */
    x86_rm(c, 0x89, rdx, base, d_hi);   /* [d_hi] = P0.hi */
    x86_rm(c, 0x8B, rax, base, a_lo);   /* rax = a_lo */
    x86_mul_mem(c, base, b_hi);         /* rdx:rax = a_lo * b_hi (P1) */
    x86_add_mem(c, rax, base, d_hi);    /* [d_hi] += P1.lo */
    x86_rm(c, 0x8B, rax, base, a_hi);   /* rax = a_hi */
    x86_mul_mem(c, base, b_lo);         /* rdx:rax = a_hi * b_lo (P2) */
    x86_add_mem(c, rax, base, d_hi);    /* [d_hi] += P2.lo */
}

/* mov dst, src */
static void x86_mov_rr(ZeroCCCompiler *c, int dst, int src) {
    static const uint8_t op[] = {0x89};  /* mov r/m64, r64 */
    if (dst != src) x86_rr(c, op, 1, src, dst);
}

/* dst = dst <op> src for "op r/m64, r64" style ALU instructions (add/sub/and/or/xor) */
static void x86_alu(ZeroCCCompiler *c, uint8_t opcode, int dst, int src) {
    x86_rr(c, &opcode, 1, src, dst);
}

/* Three-operand wrapper for commutative ALU ops: dst = src1 <op> src2 */
static void x86_alu3_comm(ZeroCCCompiler *c, uint8_t opcode, int dst, int src1, int src2) {
    if (dst == src2) {           /* avoid clobbering src2 with the initial mov */
        int t = src1; src1 = src2; src2 = t;
    }
    x86_mov_rr(c, dst, src1);
    x86_alu(c, opcode, dst, src2);
}

/* MUL128 with an arbitrary mix of register-pair / frame-slot operands
 * (Sprint 109 1-scratch-pair fix). Each of d/a/b is either REG (physical
 * registers in *_lo/*_hi) or MEM (frame displacements in *_lo/*_hi, selected
 * by the corresponding *_mem flag). Only rax/rdx are used as working
 * registers (both reserved around OP_MUL128), so d may alias a or b: every
 * source half is re-read from its home before being consumed. */
static void x86_emit_mul128_mixed(ZeroCCCompiler *c,
                                  int d_mem, int d_lo, int d_hi,
                                  int a_mem, int a_lo, int a_hi,
                                  int b_mem, int b_lo, int b_hi) {
    int base = zero_cc_frame_base_reg(c);
    int rax = 0, rdx = 2;
    static const uint8_t mul_op[] = {0xFF};  /* 0xFF /4: mul r/m64 */
    static const uint8_t add_op[] = {0x01};  /* 0x01 /r: add r/m64, r64 */
    /* P0 = a_lo * b_lo */
    if (a_mem) x86_rm(c, 0x8B, rax, base, a_lo); else x86_mov_rr(c, rax, a_lo);
    if (b_mem) x86_mul_mem(c, base, b_lo);      else x86_rr(c, mul_op, 1, 4, b_lo);
    if (d_mem) { x86_rm(c, 0x89, rax, base, d_lo); x86_rm(c, 0x89, rdx, base, d_hi); }
    else       { x86_mov_rr(c, d_lo, rax);         x86_mov_rr(c, d_hi, rdx); }
    /* P1 = a_lo * b_hi; d_hi += P1.lo */
    if (a_mem) x86_rm(c, 0x8B, rax, base, a_lo); else x86_mov_rr(c, rax, a_lo);
    if (b_mem) x86_mul_mem(c, base, b_hi);      else x86_rr(c, mul_op, 1, 4, b_hi);
    if (d_mem) x86_add_mem(c, rax, base, d_hi); else x86_rr(c, add_op, 1, rax, d_hi);
    /* P2 = a_hi * b_lo; d_hi += P2.lo */
    if (a_mem) x86_rm(c, 0x8B, rax, base, a_hi); else x86_mov_rr(c, rax, a_hi);
    if (b_mem) x86_mul_mem(c, base, b_lo);      else x86_rr(c, mul_op, 1, 4, b_lo);
    if (d_mem) x86_add_mem(c, rax, base, d_hi); else x86_rr(c, add_op, 1, rax, d_hi);
}

/* Spilled ADD128/SUB128/AND128/OR128/XOR128 (Sprint 109 1-scratch-pair
 * fix): b and d live in frame slots, a in registers (or a slot). Uses only
 * the reserved 128-bit scratch pair (r10/r11 on x86-64 - rax/rdx are NOT
 * reserved around these ops). The hi half of add/sub chains the
 * carry/borrow from the lo half. */
static void x86_emit_bin128_sp(ZeroCCCompiler *c, int op, int a_mem,
                               int a_lo, int a_hi,
                               int32_t b_lo, int32_t b_hi,
                               int32_t d_lo, int32_t d_hi) {
    int base = zero_cc_frame_base_reg(c);
    int plo = BE(c)->g128_scratch_lo[0], phi = BE(c)->g128_scratch_hi[0];
    uint8_t lo_op, hi_op;
    switch (op) {
        case OP_SUB128: lo_op = 0x29; hi_op = 0x19; break;  /* sub /r, sbb /r */
        case OP_AND128: lo_op = 0x21; hi_op = 0x21; break;  /* and /r */
        case OP_OR128:  lo_op = 0x09; hi_op = 0x09; break;  /* or  /r */
        case OP_XOR128: lo_op = 0x31; hi_op = 0x31; break;  /* xor /r */
        case OP_ADD128:
        default:        lo_op = 0x01; hi_op = 0x11; break;  /* add /r, adc /r */
    }
    if (a_mem) x86_rm(c, 0x8B, plo, base, a_lo); else x86_mov_rr(c, plo, a_lo);
    x86_rm(c, 0x8B, phi, base, b_lo);             /* phi = b_lo */
    x86_rr(c, &lo_op, 1, phi, plo);               /* plo = a_lo OP b_lo (CF) */
    x86_rm(c, 0x89, plo, base, d_lo);             /* [d_lo] = plo */
    if (a_mem) x86_rm(c, 0x8B, plo, base, a_hi); else x86_mov_rr(c, plo, a_hi);
    x86_rm(c, 0x8B, phi, base, b_hi);             /* phi = b_hi */
    x86_rr(c, &hi_op, 1, phi, plo);               /* plo = a_hi OP b_hi (+CF) */
    x86_rm(c, 0x89, plo, base, d_hi);             /* [d_hi] = plo */
}

/* x86-64 backend implementation */
static void x86_emit_prologue(ZeroCCCompiler *c, size_t stack_size, uint32_t callee_save_mask) {
    if (c->lowering.is_frameless) return;

    if (c->lowering.omit_frame_pointer) {
        /* Option 4: no frame pointer. Frame slots are addressed from RSP
         * with a constant delta (LoweringState.frame_delta = X - 8, where
         * X is the SP adjustment below). X = frame_delta + 8, i.e.
         *   - red-zone leaf:            X = 0   (no SP adjustment at all)
         *   - alloc == 0, non-leaf:     X = 8   (keep 16-byte call alignment)
         *   - normal:                   X = alloc + 8
         * _Alignas(N) realign mode (frame_align > 16): the prologue first
         * does `push rbp; mov rbp, rsp; and $-N, %rsp`, then subtracts the
         * N-rounded frame (X = frame_delta, NOT frame_delta + 8): the frame
         * is anchored to the realigned SP' and slots are [SP' - k]. */
        if (c->lowering.frame_align > 16) {
            /* push rbp */
            me_emit_u8(c, 0x55);
            /* mov rbp, rsp */
            me_emit_u8(c, 0x48); me_emit_u8(c, 0x89); me_emit_u8(c, 0xE5);
            /* and $-N, rsp (imm32 two's complement) */
            me_emit_u8(c, 0x48); me_emit_u8(c, 0x81); me_emit_u8(c, 0xE4);
            me_emit_u32(c, (uint32_t)(-(int32_t)c->lowering.frame_align));
        }
        size_t X = (c->lowering.frame_align > 16)
            ? (size_t)c->lowering.frame_delta
            : (size_t)c->lowering.frame_delta + 8;
        (void)stack_size;
        (void)callee_save_mask;  /* CSR stores are emitted by emit_prologue_mc() */
        if (X > 4096) {
            size_t top_page = (X - 1) / 4096;
            for (size_t k = top_page; k >= 1; k--) {
                size_t off = k * 4096;
                /* mov rax, rsp */
                me_emit_u8(c, 0x48); me_emit_u8(c, 0x89); me_emit_u8(c, 0xE0);
                /* sub rax, off */
                me_emit_u8(c, 0x48); me_emit_u8(c, 0x81); me_emit_u8(c, 0xE8);
                me_emit_u32(c, (uint32_t)off);
                x86_rm(c, 0x80, 0, X86_RAX, 0);
                me_emit_u8(c, 0);
            }
        }
        if (X > 0) {
            /* sub rsp, X (imm32) */
            me_emit_u8(c, 0x48);
            me_emit_u8(c, 0x81);
            me_emit_u8(c, 0xEC);
            me_emit_u32(c, (uint32_t)X);
        }
        return;
    }

    /* push rbp */
    me_emit_u8(c, 0x55);
    /* mov rbp, rsp */
    me_emit_u8(c, 0x48);
    me_emit_u8(c, 0x89);
    me_emit_u8(c, 0xe5);
    /* Stack probing (Linux guard page): a bare `sub rsp, N` for N > 4KB
     * lands RSP across the guard page and SIGSEGVs. Touch every page of
     * the frame, deepest first, before lowering RSP. RAX is scratch
     * (caller-saved and not a SysV argument register at entry). */
    if (stack_size > 4096) {
        size_t top_page = (stack_size - 1) / 4096;
        for (size_t k = top_page; k >= 1; k--) {
            size_t off = k * 4096;
            /* mov rax, rsp */
            me_emit_u8(c, 0x48); me_emit_u8(c, 0x89); me_emit_u8(c, 0xE0);
            /* sub rax, off */
            me_emit_u8(c, 0x48); me_emit_u8(c, 0x81); me_emit_u8(c, 0xE8);
            me_emit_u32(c, (uint32_t)off);
            /* cmpb $0, (rax) - via x86_rm (hand-rolled ModRM 0x78 was
             * mod=01/disp8 form and swallowed the next REX byte). REX.W is
             * ignored by the CPU on the 8-bit 0x80 opcode. */
            x86_rm(c, 0x80, 0, X86_RAX, 0);
            me_emit_u8(c, 0);
        }
    }
    /* sub rsp, stack_size (align to 16 bytes) */
    if (stack_size > 0) {
        me_emit_u8(c, 0x48);
        me_emit_u8(c, 0x81);
        me_emit_u8(c, 0xec);
        me_emit_u32(c, (uint32_t)stack_size);
    }

    /* Emit callee-saved register stores (after prologue, when mask is populated) */
    /* x86-64 callee-saved registers: rbx(3), r12(12), r13(13), r14(14), r15(15) */
    static const int callee_saved_regs[] = {3, 12, 13, 14, 15};
    static const int32_t slot_offsets[] = {8, 16, 24, 32, 40};

    for (int i = 0; i < 5; i++) {
        int reg = callee_saved_regs[i];
        if ((callee_save_mask >> reg) & 1u) {
            /* mov [rbp - offset], reg */
            static const uint8_t mov_rm64_r64[] = {0x89};
            x86_rm(c, mov_rm64_r64[0], reg, X86_RBP, -slot_offsets[i]);
        }
    }
}

/* Option 4: restore SP by the prologue's adjustment X = frame_delta + 8
 * (X == 0 for the red-zone leaf case: SP was never lowered).
 * _Alignas(N) realign mode: `leave` (mov rsp, rbp; pop rbp) restores the
 * exact entry state regardless of the runtime realign amount. */
static void x86_emit_frame_teardown(ZeroCCCompiler *c) {
    if (c->lowering.frame_align > 16) {
        me_emit_u8(c, 0xC9);  /* leave */
        return;
    }
    size_t X = (size_t)c->lowering.frame_delta + 8;
    if (X == 0) return;
    if (X <= 127) {
        /* add rsp, imm8 */
        me_emit_u8(c, 0x48); me_emit_u8(c, 0x83); me_emit_u8(c, 0xC4);
        me_emit_u8(c, (uint8_t)X);
    } else {
        /* add rsp, imm32 */
        me_emit_u8(c, 0x48); me_emit_u8(c, 0x81); me_emit_u8(c, 0xC4);
        me_emit_u32(c, (uint32_t)X);
    }
}

static void x86_emit_epilogue(ZeroCCCompiler *c) {
    if (!c->lowering.is_frameless) {
        if (c->lowering.omit_frame_pointer) {
            x86_emit_frame_teardown(c);
        } else {
            /* leave */
            me_emit_u8(c, 0xc9);
        }
    }
    /* ret */
    me_emit_u8(c, 0xc3);
}

static void x86_emit_leave(ZeroCCCompiler *c) {
    if (c->lowering.is_frameless) return;
    if (c->lowering.omit_frame_pointer) {
        x86_emit_frame_teardown(c);
        return;
    }
    /* leave */
    me_emit_u8(c, 0xc9);
}

/* Assembly emission functions for text output - defined in x86_64_asm.c */
/* static void x86_emit_prologue_asm(ZeroCCCompiler *c, size_t stack_size) { ... } */
/* static void x86_emit_epilogue_asm(ZeroCCCompiler *c) { ... } */
/* static const char *x86_get_hw_reg_name(int phys_reg) { ... } */

static size_t x86_get_code_offset(ZeroCCCompiler *c) {
    return me_get_offset(c);
}

static void x86_emit_mov_imm(ZeroCCCompiler *c, int dst_reg, int64_t imm) {
    /* mov reg, imm64 */
    x86_rex(c, 1, 0, 0, (dst_reg >> 3) & 1);
    me_emit_u8(c, 0xB8 | (dst_reg & 7));
    me_emit_u64(c, imm);
}

static void x86_emit_add(ZeroCCCompiler *c, int dst, int src1, int src2) {
    x86_alu3_comm(c, 0x01, dst, src1, src2);
}

/* Vector operations for JIT binary emission (SSE2) */
static void x86_emit_vload(ZeroCCCompiler *c, int dst, int base, int32_t offset) {
    /* movups xmm_dest, [base + offset] - unaligned move (works for any alignment) */
    x86_rex(c, 0, (dst >> 3) & 1, 0, (base >> 3) & 1);
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x10);  /* movups */
    int mod;
    if (offset == 0 && (base & 7) != X86_RBP) mod = 0;
    else if (offset >= -128 && offset <= 127) mod = 1;
    else mod = 2;
    x86_modrm(c, mod, dst & 7, base & 7);
    if ((base & 7) == X86_RSP) me_emit_u8(c, 0x24);
    if (mod == 1) me_emit_u8(c, (uint8_t)(int8_t)offset);
    else if (mod == 2) me_emit_u32(c, (uint32_t)offset);
}

static void x86_emit_vstore(ZeroCCCompiler *c, int src, int base, int32_t offset) {
    /* movups [base + offset], xmm_src - unaligned move */
    x86_rex(c, 0, (src >> 3) & 1, 0, (base >> 3) & 1);
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x11);  /* movups */
    int mod;
    if (offset == 0 && (base & 7) != X86_RBP) mod = 0;
    else if (offset >= -128 && offset <= 127) mod = 1;
    else mod = 2;
    x86_modrm(c, mod, src & 7, base & 7);
    if ((base & 7) == X86_RSP) me_emit_u8(c, 0x24);
    if (mod == 1) me_emit_u8(c, (uint8_t)(int8_t)offset);
    else if (mod == 2) me_emit_u32(c, (uint32_t)offset);
}

/* Scalar 4-byte float load: movss xmm, [base + offset] - F3 0F 10 /r,
 * followed by in-register cvtss2sd (F3 0F 5A /r) to widen the float to the
 * 64-bit double form used by the rest of the scalar FP pipeline. */
static void x86_emit_vloadss(ZeroCCCompiler *c, int dst, int base, int32_t offset) {
    me_emit_u8(c, 0xF3);
    x86_rex(c, 0, (dst >> 3) & 1, 0, (base >> 3) & 1);
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x10);  /* movss */
    int mod;
    if (offset == 0 && (base & 7) != X86_RBP) mod = 0;
    else if (offset >= -128 && offset <= 127) mod = 1;
    else mod = 2;
    x86_modrm(c, mod, dst & 7, base & 7);
    if ((base & 7) == X86_RSP) me_emit_u8(c, 0x24);
    if (mod == 1) me_emit_u8(c, (uint8_t)(int8_t)offset);
    else if (mod == 2) me_emit_u32(c, (uint32_t)offset);
    /* cvtss2sd xmm, xmm (same register) */
    me_emit_u8(c, 0xF3);
    x86_rex(c, 0, (dst >> 3) & 1, 0, (dst >> 3) & 1);
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x5A);
    x86_modrm(c, 3, dst & 7, dst & 7);
}

/* Scalar 8-byte float load: movsd xmm, [base + offset] - F2 0F 10 /r */
static void x86_emit_vloadsd(ZeroCCCompiler *c, int dst, int base, int32_t offset) {
    me_emit_u8(c, 0xF2);
    x86_rex(c, 0, (dst >> 3) & 1, 0, (base >> 3) & 1);
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x10);  /* movsd */
    int mod;
    if (offset == 0 && (base & 7) != X86_RBP) mod = 0;
    else if (offset >= -128 && offset <= 127) mod = 1;
    else mod = 2;
    x86_modrm(c, mod, dst & 7, base & 7);
    if ((base & 7) == X86_RSP) me_emit_u8(c, 0x24);
    if (mod == 1) me_emit_u8(c, (uint8_t)(int8_t)offset);
    else if (mod == 2) me_emit_u32(c, (uint32_t)offset);
}

/* Scalar 4-byte float store: the register holds a 64-bit double (ZeroCC's
 * scalar FP model), so narrow via the xmm7 scratch (in-register cvttsd2ss,
 * F2 0F 5A /r) without clobbering the source, then movss [base + offset],
 * xmm - F3 0F 11 /r. */
static void x86_emit_vstoress(ZeroCCCompiler *c, int src, int base, int32_t offset) {
    const int scratch = 7;
    if (scratch != src) {
        /* movdqa xmm7, xmm_src (0x66 before REX: gas order; REX.R=0 -
         * dst is xmm7; only REX.B can be set, for src >= xmm8) */
        me_emit_u8(c, 0x66);
        x86_rex(c, 0, 0, 0, (src >> 3) & 1);
        me_emit_u8(c, 0x0F);
        me_emit_u8(c, 0x6F);  /* movdqa */
        x86_modrm(c, 3, 7 & 7, src & 7);
    }
    me_emit_u8(c, 0xF2);
    x86_rex(c, 0, (scratch >> 3) & 1, 0, (scratch >> 3) & 1);
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x5A);  /* cvttsd2ss xmm, xmm */
    x86_modrm(c, 3, scratch & 7, scratch & 7);
    me_emit_u8(c, 0xF3);
    x86_rex(c, 0, (scratch >> 3) & 1, 0, (base >> 3) & 1);
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x11);  /* movss */
    int mod;
    if (offset == 0 && (base & 7) != X86_RBP) mod = 0;
    else if (offset >= -128 && offset <= 127) mod = 1;
    else mod = 2;
    x86_modrm(c, mod, scratch & 7, base & 7);
    if ((base & 7) == X86_RSP) me_emit_u8(c, 0x24);
    if (mod == 1) me_emit_u8(c, (uint8_t)(int8_t)offset);
    else if (mod == 2) me_emit_u32(c, (uint32_t)offset);
}

/* Sprint 115m: raw 4-byte ABI-float access - the FP register's low 32 bits
 * ARE the float pattern (no cvtss2sd widen / cvttsd2ss narrow). x86's
 * movss zero-extends the upper bits on load, so VLOADSS_RAW leaves a clean
 * low-32 float in the register, exactly the SysV float-return/param form. */

/* Scalar 4-byte float load WITHOUT widen: movss xmm, [base + offset] */
static void x86_emit_vloadss_raw(ZeroCCCompiler *c, int dst, int base, int32_t offset) {
    me_emit_u8(c, 0xF3);
    x86_rex(c, 0, (dst >> 3) & 1, 0, (base >> 3) & 1);
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x10);  /* movss */
    int mod;
    if (offset == 0 && (base & 7) != X86_RBP) mod = 0;
    else if (offset >= -128 && offset <= 127) mod = 1;
    else mod = 2;
    x86_modrm(c, mod, dst & 7, base & 7);
    if ((base & 7) == X86_RSP) me_emit_u8(c, 0x24);
    if (mod == 1) me_emit_u8(c, (uint8_t)(int8_t)offset);
    else if (mod == 2) me_emit_u32(c, (uint32_t)offset);
}

/* Scalar 4-byte float store WITHOUT narrow: movss [base + offset], xmm */
static void x86_emit_vstoress_raw(ZeroCCCompiler *c, int src, int base, int32_t offset) {
    me_emit_u8(c, 0xF3);
    x86_rex(c, 0, (src >> 3) & 1, 0, (base >> 3) & 1);
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x11);  /* movss (mem <- reg) */
    int mod;
    if (offset == 0 && (base & 7) != X86_RBP) mod = 0;
    else if (offset >= -128 && offset <= 127) mod = 1;
    else mod = 2;
    x86_modrm(c, mod, src & 7, base & 7);
    if ((base & 7) == X86_RSP) me_emit_u8(c, 0x24);
    if (mod == 1) me_emit_u8(c, (uint8_t)(int8_t)offset);
    else if (mod == 2) me_emit_u32(c, (uint32_t)offset);
}

/* In-register model double -> ABI float: cvttsd2ss dst, src (F2 0F 5A /r). */
static void x86_emit_cvttsd2ss(ZeroCCCompiler *c, int dst, int src) {
    me_emit_u8(c, 0xF2);
    x86_rex(c, 0, (dst >> 3) & 1, 0, (src >> 3) & 1);
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x5A);  /* cvttsd2ss */
    x86_modrm(c, 3, dst & 7, src & 7);
}


/* Scalar 8-byte float store: movsd [base + offset], xmm - F2 0F 11 /r */
static void x86_emit_vstoredsd(ZeroCCCompiler *c, int src, int base, int32_t offset) {
    me_emit_u8(c, 0xF2);
    x86_rex(c, 0, (src >> 3) & 1, 0, (base >> 3) & 1);
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x11);  /* movsd */
    int mod;
    if (offset == 0 && (base & 7) != X86_RBP) mod = 0;
    else if (offset >= -128 && offset <= 127) mod = 1;
    else mod = 2;
    x86_modrm(c, mod, src & 7, base & 7);
    if ((base & 7) == X86_RSP) me_emit_u8(c, 0x24);
    if (mod == 1) me_emit_u8(c, (uint8_t)(int8_t)offset);
    else if (mod == 2) me_emit_u32(c, (uint32_t)offset);
}

static void x86_emit_vmovaps(ZeroCCCompiler *c, int dst, int src);

static void x86_emit_vaddps(ZeroCCCompiler *c, int dst, int src1, int src2) {
    /* addps xmm_dest, xmm_src - packed single-precision add */
    if (dst == src2 && dst != src1) {
        /* Commutative: swap operands so dst = src2 + src1 */
        int tmp = src1; src1 = src2; src2 = tmp;
    }
    if (dst != src1) {
        x86_emit_vmovaps(c, dst, src1);
    }
    x86_rex(c, 0, (dst >> 3) & 1, 0, (src2 >> 3) & 1);
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x58);  /* addps */
    x86_modrm(c, 3, dst & 7, src2 & 7);
}

static void x86_emit_vsubps(ZeroCCCompiler *c, int dst, int src1, int src2) {
    /* subps xmm_dest, xmm_src - packed single-precision subtract */
    if (dst == src2 && dst != src1) {
        /* dst == src2: use scratch register or swap isn't commutative */
        /* If dst != src1, we can copy src1 to dst, but if dst == src2, copy clobbers src2 */
        /* ZeroCC regalloc ensures separate temps or dst == src1 */
    }
    if (dst != src1) {
        x86_emit_vmovaps(c, dst, src1);
    }
    x86_rex(c, 0, (dst >> 3) & 1, 0, (src2 >> 3) & 1);
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x5C);  /* subps */
    x86_modrm(c, 3, dst & 7, src2 & 7);
}

static void x86_emit_vmulps(ZeroCCCompiler *c, int dst, int src1, int src2) {
    /* mulps xmm_dest, xmm_src - packed single-precision multiply */
    if (dst == src2 && dst != src1) {
        int tmp = src1; src1 = src2; src2 = tmp;
    }
    if (dst != src1) {
        x86_emit_vmovaps(c, dst, src1);
    }
    x86_rex(c, 0, (dst >> 3) & 1, 0, (src2 >> 3) & 1);
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x59);  /* mulps */
    x86_modrm(c, 3, dst & 7, src2 & 7);
}

static void x86_emit_vdivps(ZeroCCCompiler *c, int dst, int src1, int src2) {
    /* divps xmm_dest, xmm_src - packed single-precision divide */
    if (dst != src1) {
        x86_emit_vmovaps(c, dst, src1);
    }
    x86_rex(c, 0, (dst >> 3) & 1, 0, (src2 >> 3) & 1);
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x5E);  /* divps */
    x86_modrm(c, 3, dst & 7, src2 & 7);
}

static void x86_emit_vmovaps(ZeroCCCompiler *c, int dst, int src) {
    /* movaps xmm_dest, xmm_src - aligned packed move */
    if (dst == src) return;
    x86_rex(c, 0, (dst >> 3) & 1, 0, (src >> 3) & 1);
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x28);  /* movaps */
    x86_modrm(c, 3, dst & 7, src & 7);
}

static void x86_emit_vaddpd(ZeroCCCompiler *c, int dst, int src1, int src2) {
    /* addpd xmm_dest, xmm_src - packed double-precision add (66 0F 58 /r) */
    if (dst == src2 && dst != src1) {
        int tmp = src1; src1 = src2; src2 = tmp;
    }
    if (dst != src1) {
        x86_emit_vmovaps(c, dst, src1);
    }
    me_emit_u8(c, 0x66);
    x86_rex(c, 0, (dst >> 3) & 1, 0, (src2 >> 3) & 1);
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x58);  /* addpd */
    x86_modrm(c, 3, dst & 7, src2 & 7);
}

static void x86_emit_vsubpd(ZeroCCCompiler *c, int dst, int src1, int src2) {
    /* subpd xmm_dest, xmm_src - packed double-precision subtract (66 0F 5C /r) */
    if (dst != src1) {
        x86_emit_vmovaps(c, dst, src1);
    }
    me_emit_u8(c, 0x66);
    x86_rex(c, 0, (dst >> 3) & 1, 0, (src2 >> 3) & 1);
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x5C);  /* subpd */
    x86_modrm(c, 3, dst & 7, src2 & 7);
}

static void x86_emit_vmulpd(ZeroCCCompiler *c, int dst, int src1, int src2) {
    /* mulpd xmm_dest, xmm_src - packed double-precision multiply (66 0F 59 /r) */
    if (dst == src2 && dst != src1) {
        int tmp = src1; src1 = src2; src2 = tmp;
    }
    if (dst != src1) {
        x86_emit_vmovaps(c, dst, src1);
    }
    me_emit_u8(c, 0x66);
    x86_rex(c, 0, (dst >> 3) & 1, 0, (src2 >> 3) & 1);
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x59);  /* mulpd */
    x86_modrm(c, 3, dst & 7, src2 & 7);
}

static void x86_emit_vdivpd(ZeroCCCompiler *c, int dst, int src1, int src2) {
    /* divpd xmm_dest, xmm_src - packed double-precision divide (66 0F 5E /r) */
    if (dst != src1) {
        x86_emit_vmovaps(c, dst, src1);
    }
    me_emit_u8(c, 0x66);
    x86_rex(c, 0, (dst >> 3) & 1, 0, (src2 >> 3) & 1);
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x5E);  /* divpd */
    x86_modrm(c, 3, dst & 7, src2 & 7);
}

static void x86_emit_vpaddd(ZeroCCCompiler *c, int dst, int src1, int src2) {
    /* paddd xmm_dest, xmm_src - packed 32-bit int add (66 0F FE /r) */
    if (dst == src2 && dst != src1) {
        int tmp = src1; src1 = src2; src2 = tmp;
    }
    if (dst != src1) {
        x86_emit_vmovaps(c, dst, src1);
    }
    me_emit_u8(c, 0x66);
    x86_rex(c, 0, (dst >> 3) & 1, 0, (src2 >> 3) & 1);
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0xFE);  /* paddd */
    x86_modrm(c, 3, dst & 7, src2 & 7);
}

static void x86_emit_vpsubd(ZeroCCCompiler *c, int dst, int src1, int src2) {
    /* psubd xmm_dest, xmm_src - packed 32-bit int subtract (66 0F FA /r) */
    if (dst != src1) {
        x86_emit_vmovaps(c, dst, src1);
    }
    me_emit_u8(c, 0x66);
    x86_rex(c, 0, (dst >> 3) & 1, 0, (src2 >> 3) & 1);
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0xFA);  /* psubd */
    x86_modrm(c, 3, dst & 7, src2 & 7);
}

static void x86_emit_vpmulld(ZeroCCCompiler *c, int dst, int src1, int src2) {
    /* pmulld xmm_dest, xmm_src - packed 32-bit int multiply (66 0F 38 40 /r) */
    if (dst == src2 && dst != src1) {
        int tmp = src1; src1 = src2; src2 = tmp;
    }
    if (dst != src1) {
        x86_emit_vmovaps(c, dst, src1);
    }
    me_emit_u8(c, 0x66);
    x86_rex(c, 0, (dst >> 3) & 1, 0, (src2 >> 3) & 1);
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x38);
    me_emit_u8(c, 0x40);  /* pmulld */
    x86_modrm(c, 3, dst & 7, src2 & 7);
}

static void x86_emit_neg(ZeroCCCompiler *c, int dst, int src);

static void x86_emit_sub(ZeroCCCompiler *c, int dst, int src1, int src2) {
    if (dst == src2 && dst != src1) {
        /* dst = src1 - dst  ==>  dst = -dst + src1 */
        x86_emit_neg(c, dst, dst);
        x86_alu(c, 0x01, dst, src1);
        return;
    }
    x86_mov_rr(c, dst, src1);
    x86_alu(c, 0x29, dst, src2);  /* sub dst, src2 */
}

static void x86_emit_mul(ZeroCCCompiler *c, int dst, int src1, int src2) {
    static const uint8_t imul[] = {0x0F, 0xAF};  /* imul r64, r/m64 */
    if (dst == src2) { int t = src1; src1 = src2; src2 = t; }
    x86_mov_rr(c, dst, src1);
    x86_rr(c, imul, 2, dst, src2);
}

static void x86_emit_cqo(ZeroCCCompiler *c) {
    /* cqo - sign extend rax into rdx:rax */
    me_emit_u8(c, 0x48);
    me_emit_u8(c, 0x99);
}

static void x86_emit_idiv(ZeroCCCompiler *c, int src) {
    /* idiv r/m64  (F7 /7) */
    x86_rex(c, 1, 0, 0, (src >> 3) & 1);
    me_emit_u8(c, 0xF7);
    x86_modrm(c, 3, 7, src);
}

/* Shared signed division: quotient (want_rem=0) or remainder (want_rem=1) of src1/src2 into dst.
 * Clobbers rax, rdx and r11.  No stack scratch: LRA reserves VREG_0/VREG_2
 * (rax/rdx) at every OP_DIV/OP_MOD, so no user temp is live in rax/rdx here
 * (a push/pop save would write into the red zone and corrupt frame slots in
 * frameless -O0 functions - c-torture pr94412.c). */
static void x86_divmod(ZeroCCCompiler *c, int dst, int src1, int src2, int want_rem) {
    int divisor = src2;
    if (divisor == X86_RAX || divisor == X86_RDX) {
        x86_mov_rr(c, X86_R11, divisor);  /* keep divisor out of rax/rdx */
        divisor = X86_R11;
    }

    x86_mov_rr(c, X86_RAX, src1);
    if (c->lowering.div_is_unsigned) {
        /* xor %edx, %edx (31 D2) + div r/m64 (REX.W F7 /6) */
        me_emit_u8(c, 0x31);
        me_emit_u8(c, 0xD2);
        x86_rex(c, 1, 0, 0, (divisor >> 3) & 1);
        me_emit_u8(c, 0xF7);
        x86_modrm(c, 3, 6, divisor);
    } else {
        x86_emit_cqo(c);
        x86_emit_idiv(c, divisor);
    }

    /* Result: quotient in rax (div) or remainder in rdx (mod).  Move it to
     * dst unless dst already IS the result register — dst==rdx for a DIV
     * holds the remainder, and dst==rax for a MOD holds the quotient, so
     * those two cases still need the move (pre-fix bug: `8/4` with the
     * result temp on rdx silently returned 8%4). */
    int result_reg = want_rem ? X86_RDX : X86_RAX;
    if (dst != result_reg) {
        x86_mov_rr(c, dst, result_reg);
    }
}

static void x86_emit_div(ZeroCCCompiler *c, int dst, int src1, int src2) {
    x86_divmod(c, dst, src1, src2, 0);
}

static void x86_emit_mod(ZeroCCCompiler *c, int dst, int src1, int src2) {
    x86_divmod(c, dst, src1, src2, 1);
}

static void x86_emit_and(ZeroCCCompiler *c, int dst, int src1, int src2) {
    x86_alu3_comm(c, 0x21, dst, src1, src2);
}

static void x86_emit_or(ZeroCCCompiler *c, int dst, int src1, int src2) {
    x86_alu3_comm(c, 0x09, dst, src1, src2);
}

static void x86_emit_xor(ZeroCCCompiler *c, int dst, int src1, int src2) {
    x86_alu3_comm(c, 0x31, dst, src1, src2);
}

/* Register-register shift shared by shl/shr/sar: <op> dst_reg, cl.
 * The x86 shift count must live in RCX (physical register 1).
 * Aliasing-safe: src1 (value) and src2 (count) may alias dst_reg or each
 * other (e.g. compound assignment `x <<= y` lowers to SHL rX, rVal, rX).
 * Strategy: materialize the count into RCX before clobbering anything that
 * may hold it, then place the value at the shift destination, then shift.
 * reg_field: 4 = shl, 5 = shr, 7 = sar. */
static void x86_emit_shift(ZeroCCCompiler *c, int dst_reg, int src1, int src2, int reg_field) {
    if (dst_reg == 1) {
        /* Result lands in RCX, which must hold the count at shift time. */
        if (src1 == 1) {
            if (src2 == 1) {
                /* RCX holds both value and count: shift in place. */
                x86_rex(c, 1, 0, 0, 0);
                me_emit_u8(c, 0xd3);
                x86_modrm(c, 3, reg_field, 1); /* shl rcx, cl */
                return;
            }
            /* Value in RCX, count elsewhere: copy value to scratch, move
             * count to RCX, shift scratch, move result back to RCX. */
            int temp_reg = (src2 == 10) ? 11 : 10;  /* r10/r11 scratch, avoid src2 */
            x86_mov_rr(c, temp_reg, 1);
            x86_mov_rr(c, 1, src2);
            x86_rex(c, 1, 0, 0, (temp_reg >> 3) & 1);
            me_emit_u8(c, 0xd3);
            x86_modrm(c, 3, reg_field, temp_reg & 7);
            x86_mov_rr(c, 1, temp_reg);
            return;
        }
        /* Value not in RCX: shift src1 in place (safe, result goes to RCX
         * anyway), then move the result into RCX. */
        if (src2 != 1) {
            x86_mov_rr(c, 1, src2);
        }
        x86_rex(c, 1, 0, 0, (src1 >> 3) & 1);
        me_emit_u8(c, 0xd3);
        x86_modrm(c, 3, reg_field, src1 & 7); /* <op> src1, cl */
        x86_mov_rr(c, 1, src1);
        return;
    }

    /* dst_reg != RCX. */
    if (src1 == 1) {
        if (src2 == 1) {
            /* Self-shift: value == count == RCX. */
            x86_mov_rr(c, dst_reg, 1);
            x86_rex(c, 1, 0, 0, (dst_reg >> 3) & 1);
            me_emit_u8(c, 0xd3);
            x86_modrm(c, 3, reg_field, dst_reg & 7);
            return;
        }
        /* Value in RCX, count elsewhere (src2 may alias dst): copy value out
         * first, then move the count in. */
        int temp_reg = (src2 == 10) ? 11 : 10;  /* r10/r11 scratch, avoid src2 */
        x86_mov_rr(c, temp_reg, 1);
        x86_mov_rr(c, 1, src2);
        x86_mov_rr(c, dst_reg, temp_reg);
        x86_rex(c, 1, 0, 0, (dst_reg >> 3) & 1);
        me_emit_u8(c, 0xd3);
        x86_modrm(c, 3, reg_field, dst_reg & 7);
        return;
    }

    /* src1 != RCX: move count first (dst != RCX, so this cannot destroy the
     * count's source even when src2 == dst_reg), then the value. */
    if (src2 != 1) {
        x86_mov_rr(c, 1, src2);
    }
    if (dst_reg != src1) {
        x86_mov_rr(c, dst_reg, src1);
    }
    x86_rex(c, 1, 0, 0, (dst_reg >> 3) & 1);
    me_emit_u8(c, 0xd3);
    x86_modrm(c, 3, reg_field, dst_reg & 7); /* <op> dst_reg, cl */
}

/* shl dst_reg, cl (dst = src1 << src2) */
static void x86_emit_shl(ZeroCCCompiler *c, int dst_reg, int src1, int src2) {
    x86_emit_shift(c, dst_reg, src1, src2, 4);
}

static void x86_emit_cmp(ZeroCCCompiler *c, int src1, int src2) {
    /* cmp src1, src2  (flags from src1 - src2) */
    x86_alu(c, 0x39, src1, src2);
}

static void x86_emit_setcc(ZeroCCCompiler *c, int dst_reg, uint8_t cond_byte);

static void x86_emit_cond_set(ZeroCCCompiler *c, int dst, int cond) {
    /* setcc dst based on condition code (flags set by previous cmp).
     * 6-9 are the float-safe CF/ZF-only variants (C12 remap):
     * 6=setb, 7=setbe, 8=seta, 9=setae.
     * 10=FP_EQ (sete && !parity), 11=FP_NE (setne || parity),
     * 12=FP_UNORD (setp / parity), 13=FP_ORD (setnp / !parity). */
    static const uint8_t setcc_codes[] =
        {0x9C, 0x9E, 0x9F, 0x9D, 0x94, 0x95, 0x92, 0x96, 0x97, 0x93};
    if (cond >= 0 && cond <= 9) {
        x86_emit_setcc(c, dst, setcc_codes[cond]);
    } else if (cond == 10) { /* CC_FP_EQ: sete dst8; jnp skip; movb $0, dst8; skip: movzx dst64, dst8 */
        x86_emit_setcc(c, dst, 0x94);
        me_emit_u8(c, 0x7B); /* jnp */
        size_t p = me_get_offset(c);
        me_emit_u8(c, 0);
        size_t m_start = me_get_offset(c);
        if (dst >= 4) {
            me_emit_u8(c, 0x40 | ((dst >> 3) & 1));
        }
        me_emit_u8(c, 0xB0 | (dst & 7));
        me_emit_u8(c, 0);
        size_t m_end = me_get_offset(c);
        c->text_section.data[p] = (uint8_t)(m_end - m_start);
    } else if (cond == 11) { /* CC_FP_NE: setne dst8; jnp skip; movb $1, dst8; skip: movzx dst64, dst8 */
        x86_emit_setcc(c, dst, 0x95);
        me_emit_u8(c, 0x7B); /* jnp */
        size_t p = me_get_offset(c);
        me_emit_u8(c, 0);
        size_t m_start = me_get_offset(c);
        if (dst >= 4) {
            me_emit_u8(c, 0x40 | ((dst >> 3) & 1));
        }
        me_emit_u8(c, 0xB0 | (dst & 7));
        me_emit_u8(c, 1);
        size_t m_end = me_get_offset(c);
        c->text_section.data[p] = (uint8_t)(m_end - m_start);
    } else if (cond == 12) { /* CC_FP_UNORD: setp */
        x86_emit_setcc(c, dst, 0x9A);
    } else if (cond == 13) { /* CC_FP_ORD: setnp */
        x86_emit_setcc(c, dst, 0x9B);
    }
}

static void x86_emit_neg(ZeroCCCompiler *c, int dst, int src) {
    x86_mov_rr(c, dst, src);
    x86_rex(c, 1, 0, 0, (dst >> 3) & 1);
    me_emit_u8(c, 0xF7);                /* neg r/m64 (F7 /3) */
    x86_modrm(c, 3, 3, dst);
}

/* In-place width extension (dst == src expected). Verified encodings:
 * zext32: mov r32, r32 (0x89) - any 32-bit write clears the upper 32 bits
 *         (x86-64 has no MOVZX r64, r/m32!)
 * zext16: movzx r64, r/m16 (REX.W 0F B7)
 * zext8:  movzx r64, r/m8  (REX.W 0F B6)
 * sext32: movsxd r64, r/m32 (REX.W 63)
 * sext16: movsx r32, r/m16 (0F BF) then movsxd r64, r32 (REX.W 63)
 * sext8:  movsx r32, r/m8  (0F BE) then movsxd r64, r32 (REX.W 63) */
static void x86_emit_extend(ZeroCCCompiler *c, int dst, int src, int width, int sign) {
    if (width >= 8 || width == 0) {
        /* Not a narrow extension: width 0 means "no mark" per the
         * Instr.width encoding (ir.h) and 8+ the zero-extend flag; the
         * value already fills the register, so at most a move is needed
         * (they previously fell through to the 16-bit path and truncated
         * the value). */
        if (dst != src) {
            x86_rex(c, 1, (src >> 3) & 1, 0, (dst >> 3) & 1);
            me_emit_u8(c, 0x89);
            x86_modrm(c, 3, src, dst);
        }
        return;
    }
    if (width == 4) {
        if (sign) {
            /* MOVSXD r64, r/m32: destination in the REG field, source in the
             * RM field. Using dst for the RM field mis-encodes whenever
             * dst >= 8 and src < 8 (REX.B stays clear but the rm nibble comes
             * from dst) - e.g. r12 <- sext(rdx) emitted as "movslq %esp,%r12". */
            x86_rex(c, 1, (dst >> 3) & 1, 0, (src >> 3) & 1);
            me_emit_u8(c, 0x63);
            x86_modrm(c, 3, dst, src);
        } else {
            /* MOV r/m32, r32 (clears upper 32 bits of dst): destination in
             * the RM field, source in the REG field. */
            x86_rex(c, 0, (src >> 3) & 1, 0, (dst >> 3) & 1);
            me_emit_u8(c, 0x89);
            x86_modrm(c, 3, src, dst);
        }
        return;
    }
    if (width == 1) {
        /* Byte register r/m8 encoding (verified against the GNU assembler:
         * movzbl %bl -> 0F B6 C3, %dh -> 0F B6 C6, %sil -> 40 0F B6 C6,
         * %dil -> 40 0F B6 C7, %bpl -> 40 0F B6 C5, %spl -> 40 0F B6 C4).
         * The modrm 0-7 field is NOT 1:1 with the register index:
         * phys 4-7 (rsp/rbp/rsi/rdi) need the REX X-bit, 8-15 the B-bit. */
        static const int byte_rex_x[] = {0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0};
        static const int byte_rex_b[] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1};
        int rm = src & 7;
        int rex_x = byte_rex_x[src];
        int rex_b = byte_rex_b[src];
        int rex_r = (dst >> 3) & 1;
        if (sign) {
            /* movsx r32, r/m8 (byte-mapped rm), then movsxd r64, r32
             * (32-bit register operand: normal modrm index). */
            x86_rex(c, 0, rex_r, rex_x, rex_b);
            me_emit_u8(c, 0x0F);
            me_emit_u8(c, 0xBE);
            x86_modrm(c, 3, dst, rm);
            x86_rex(c, 1, rex_r, 0, (dst >> 3) & 1);
            me_emit_u8(c, 0x63);
            x86_modrm(c, 3, dst, dst);
        } else {
            /* movzx r64, r/m8 */
            x86_rex(c, 1, rex_r, rex_x, rex_b);
            me_emit_u8(c, 0x0F);
            me_emit_u8(c, 0xB6);
            x86_modrm(c, 3, dst, rm);
        }
        return;
    }
    /* width 2 */
    if (!sign) {
        /* movzx r64, r/m16 - single instruction (dest = REG, src = RM) */
        x86_rex(c, 1, (dst >> 3) & 1, 0, (src >> 3) & 1);
        me_emit_u8(c, 0x0F);
        me_emit_u8(c, 0xB7);
        x86_modrm(c, 3, dst, src);
        return;
    }
    /* sext8/16: movsx r32, src then movsxd r64, r32.
     * 0F BE = MOVSX r32,r/m8 (byte); 0F BF = MOVSX r32,r/m16 (word). */
    x86_rex(c, 0, (dst >> 3) & 1, 0, (src >> 3) & 1);
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0xBF);
    x86_modrm(c, 3, dst, src);
    x86_rex(c, 1, (dst >> 3) & 1, 0, (dst >> 3) & 1);
    me_emit_u8(c, 0x63);
    x86_modrm(c, 3, dst, dst);
}

static void x86_emit_load(ZeroCCCompiler *c, int dst_reg, int base_reg, int32_t offset) {
    /* mov dst, [base + offset] */
    x86_rm(c, 0x8B, dst_reg, base_reg, offset);
}

static void x86_emit_store(ZeroCCCompiler *c, int src_reg, int base_reg, int32_t offset) {
    /* mov [base + offset], src */
    x86_rm(c, 0x89, src_reg, base_reg, offset);
}

static size_t x86_emit_jmp(ZeroCCCompiler *c, const char *label) {
    me_emit_u8(c, 0xE9);
    size_t patch_pos = me_get_offset(c);  /* rel32 field */
    me_emit_u32(c, 0);  /* placeholder for rel32 patch */
    /* Record this patch for later resolution */
    for (int i = 0; i < c->label_count; i++) {
        if (c->labels[i].name && strcmp(c->labels[i].name, label) == 0) {
            if (c->labels[i].patch_count < 16) {
                c->labels[i].patches[c->labels[i].patch_count++] = patch_pos;
            }
            break;
        }
    }
    return patch_pos;
}

static size_t x86_emit_jeq(ZeroCCCompiler *c, const char *label) {
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x84);
    size_t patch_pos = me_get_offset(c);  /* rel32 field */
    me_emit_u32(c, 0);
    for (int i = 0; i < c->label_count; i++) {
        if (c->labels[i].name && strcmp(c->labels[i].name, label) == 0) {
            if (c->labels[i].patch_count < 16) {
                c->labels[i].patches[c->labels[i].patch_count++] = patch_pos;
            }
            break;
        }
    }
    return patch_pos;
}

static size_t x86_emit_jne(ZeroCCCompiler *c, const char *label) {
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x85);
    size_t patch_pos = me_get_offset(c);  /* rel32 field */
    me_emit_u32(c, 0);
    for (int i = 0; i < c->label_count; i++) {
        if (c->labels[i].name && strcmp(c->labels[i].name, label) == 0) {
            if (c->labels[i].patch_count < 16) {
                c->labels[i].patches[c->labels[i].patch_count++] = patch_pos;
            }
            break;
        }
    }
    return patch_pos;
}

static size_t x86_emit_jlt(ZeroCCCompiler *c, const char *label) {
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x8C);
    size_t patch_pos = me_get_offset(c);  /* rel32 field */
    me_emit_u32(c, 0);
    for (int i = 0; i < c->label_count; i++) {
        if (c->labels[i].name && strcmp(c->labels[i].name, label) == 0) {
            if (c->labels[i].patch_count < 16) {
                c->labels[i].patches[c->labels[i].patch_count++] = patch_pos;
            }
            break;
        }
    }
    return patch_pos;
}

static size_t x86_emit_jle(ZeroCCCompiler *c, const char *label) {
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x8E);
    size_t patch_pos = me_get_offset(c);  /* rel32 field */
    me_emit_u32(c, 0);
    for (int i = 0; i < c->label_count; i++) {
        if (c->labels[i].name && strcmp(c->labels[i].name, label) == 0) {
            if (c->labels[i].patch_count < 16) {
                c->labels[i].patches[c->labels[i].patch_count++] = patch_pos;
            }
            break;
        }
    }
    return patch_pos;
}

static size_t x86_emit_jgt(ZeroCCCompiler *c, const char *label) {
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x8F);
    size_t patch_pos = me_get_offset(c);  /* rel32 field */
    me_emit_u32(c, 0);
    for (int i = 0; i < c->label_count; i++) {
        if (c->labels[i].name && strcmp(c->labels[i].name, label) == 0) {
            if (c->labels[i].patch_count < 16) {
                c->labels[i].patches[c->labels[i].patch_count++] = patch_pos;
            }
            break;
        }
    }
    return patch_pos;
}

static size_t x86_emit_jge(ZeroCCCompiler *c, const char *label) {
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x8D);
    size_t patch_pos = me_get_offset(c);  /* rel32 field */
    me_emit_u32(c, 0);
    for (int i = 0; i < c->label_count; i++) {
        if (c->labels[i].name && strcmp(c->labels[i].name, label) == 0) {
            if (c->labels[i].patch_count < 16) {
                c->labels[i].patches[c->labels[i].patch_count++] = patch_pos;
            }
            break;
        }
    }
    return patch_pos;
}

/* Conditional jump with explicit condition code */
static size_t x86_emit_jcond(ZeroCCCompiler *c, int cond, const char *label) {
    /* Condition code mapping: CC_LT=0, CC_LE=1, CC_GT=2, CC_GE=3, CC_EQ=4, CC_NE=5 */
    static const uint8_t jcc_codes[] = {0x8C, 0x8D, 0x8F, 0x8E, 0x84, 0x85};

    if (cond < 0 || cond > 5) {
        fprintf(stderr, "Invalid condition code in x86_emit_jcond: %d\n", cond);
        abort();
    }

    me_emit_u8(c, 0x0F);
    me_emit_u8(c, jcc_codes[cond]);
    size_t patch_pos = me_get_offset(c);  /* rel32 field */
    me_emit_u32(c, 0);
    for (int i = 0; i < c->label_count; i++) {
        if (c->labels[i].name && strcmp(c->labels[i].name, label) == 0) {
            if (c->labels[i].patch_count < 16) {
                c->labels[i].patches[c->labels[i].patch_count++] = patch_pos;
            }
            break;
        }
    }
    return patch_pos;
}


static void x86_emit_ret(ZeroCCCompiler *c) {
    me_emit_u8(c, 0xC3);
}

/* Function call - direct call with relative offset */
static void x86_emit_call(ZeroCCCompiler *c, const char *fn_name) {
    ZeroCCFunction *fn = zero_cc_find_function_by_symbol(c, fn_name);
    bool is_external = (!fn || fn->is_prototype);

    /* In JIT mode there is no linker/PLT, and the code image is mmap'd far
     * from external libraries, so a relative `call rel32` cannot reach an
     * external function >2GB away (the int32 offset overflows). Emit an
     * absolute call (`mov r11, imm64; call *r11`) for external calls in JIT
     * mode; the 8-byte imm64 is patched with the resolved address at load
     * time (is_abs_call reloc). Internal calls stay relative (near the call
     * site). AOT objects keep the relative call + R_X86_64_PLT32 (the host
     * linker uses the PLT for far targets). */
    if (is_external && c->jit_mode) {
        me_emit_u8(c, 0x49);              /* REX.WR */
        me_emit_u8(c, 0xBB);              /* mov r11, imm64 */
        size_t patch_pos = me_get_offset(c);
        me_emit_u64(c, 0);                /* placeholder target address (8 bytes) */
        me_emit_u8(c, 0x41);              /* REX.B */
        me_emit_u8(c, 0xFF);              /* call *r/m64 */
        me_emit_u8(c, 0xD3);              /* r11 (scratch_reg2) */
        if (c->reloc_count < ZERO_CC_MAX_RELOCS) {
            ZeroCCReloc *r = &c->relocs[c->reloc_count++];
            r->text_patch_offset = patch_pos;
            snprintf(r->target_symbol, sizeof(r->target_symbol), "%s", fn_name);
            r->is_call = false;
            r->is_abs_call = true;
        } else {
            zero_cc_reloc_overflow_note(c);
        }
        return;
    }

    /* Relative call (E8 rel32): internal calls always; external calls in AOT
     * mode, where the host linker patches the PLT entry. */
    me_emit_u8(c, 0xE8);
    size_t patch_pos = me_get_offset(c);

    /* Emit placeholder offset (0 for external, calculated for internal) */
    int32_t rel = 0;
    if (fn && !fn->is_prototype) {
        rel = (int32_t)(fn->text_offset - (patch_pos + 4));
    }
    me_emit_u32(c, (uint32_t)rel);

    /* Add relocation entry for linker to patch. The cap must be the full
     * array size (ZERO_CC_MAX_RELOCS), matching every other reloc-emitting
     * site: the shared c->relocs array also receives data relocations
     * (OP_MOV_GLOBAL etc.), so a small hardcoded cap silently drops call
     * relocations once a TU accumulates enough of them — leaving `call +0`
     * instructions that skip the callee's body (Sprint 49: zero_rt printf
     * segfault root cause). */
    if (c->reloc_count < ZERO_CC_MAX_RELOCS) {
        ZeroCCReloc *r = &c->relocs[c->reloc_count++];
        r->text_patch_offset = patch_pos;
        snprintf(r->target_symbol, sizeof(r->target_symbol), "%s", fn_name);
        r->is_call = true;
        /* For external functions, linker will use PLT; for internal, it's a direct call */
    } else {
        zero_cc_reloc_overflow_note(c);
    }
}

/* Floating point operations */
static void x86_emit_load_fp(ZeroCCCompiler *c, int dst_reg, int base_reg, int32_t offset) {
    /* movsd xmm, [base + offset] */
    me_emit_u8(c, 0xF2);
    x86_rex(c, 0, (dst_reg >> 3) & 1, 0, (base_reg >> 3) & 1);
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x10);
    x86_modrm(c, 2, dst_reg & 7, base_reg & 7);
    if ((base_reg & 7) == X86_RSP) me_emit_u8(c, 0x24);
    me_emit_u32(c, (uint32_t)offset);
}

static void x86_emit_store_fp(ZeroCCCompiler *c, int src_reg, int base_reg, int32_t offset) {
    /* movsd [base + offset], xmm */
    me_emit_u8(c, 0xF2);
    x86_rex(c, 0, (src_reg >> 3) & 1, 0, (base_reg >> 3) & 1);
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x11);
    x86_modrm(c, 2, src_reg & 7, base_reg & 7);
    if ((base_reg & 7) == X86_RSP) me_emit_u8(c, 0x24);
    me_emit_u32(c, (uint32_t)offset);
}

static void x86_emit_load_fp_rodata(ZeroCCCompiler *c, int dst_reg, size_t rodata_offset) {
    /* movsd xmm, [rip + disp32] */
    me_emit_u8(c, 0xF2);
    if (dst_reg >= 8) {
        me_emit_u8(c, 0x44);  /* REX.R for xmm8-xmm15 */
    }
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x10);  /* movsd xmm, m64 */
    x86_modrm(c, 0, dst_reg & 7, 5);  /* Mod=00, rm=5 (RIP-relative) */
    size_t patch_pos = me_get_offset(c);
    me_emit_u32(c, 0);

    if (c->str_reloc_count < ZERO_CC_MAX_STR_RELOCS) {
        c->str_relocs[c->str_reloc_count].text_patch_offset = patch_pos;
        c->str_relocs[c->str_reloc_count].rodata_offset = rodata_offset;
        c->str_relocs[c->str_reloc_count].dst_reg = dst_reg;
        c->str_reloc_count++;
    }
}

static void x86_emit_fp_mov(ZeroCCCompiler *c, int dst, int src);

static void x86_emit_fp_add(ZeroCCCompiler *c, int dst, int src1, int src2) {
    if (dst == src2 && dst != src1) {
        /* Commutative: dst = src1 + dst is the same as dst = dst + src1 */
        me_emit_u8(c, 0xF2);
        x86_rex(c, 0, (dst >> 3) & 1, 0, (src1 >> 3) & 1);
        me_emit_u8(c, 0x0F);
        me_emit_u8(c, 0x58);  /* addsd */
        x86_modrm(c, 3, dst & 7, src1 & 7);
        return;
    }
    if (dst != src1) {
        x86_emit_fp_mov(c, dst, src1);
    }
    me_emit_u8(c, 0xF2);
    x86_rex(c, 0, (dst >> 3) & 1, 0, (src2 >> 3) & 1);
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x58);  /* addsd */
    x86_modrm(c, 3, dst & 7, src2 & 7);
}

static void x86_emit_fp_sub(ZeroCCCompiler *c, int dst, int src1, int src2) {
    if (dst == src2 && dst != src1) {
        /* Non-commutative: moving src1 to dst would overwrite src2. Use xmm7 as scratch. */
        int scratch = 7;
        x86_emit_fp_mov(c, scratch, src2);
        x86_emit_fp_mov(c, dst, src1);
        src2 = scratch;
    } else if (dst != src1) {
        x86_emit_fp_mov(c, dst, src1);
    }
    me_emit_u8(c, 0xF2);
    x86_rex(c, 0, (dst >> 3) & 1, 0, (src2 >> 3) & 1);
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x5C);  /* subsd */
    x86_modrm(c, 3, dst & 7, src2 & 7);
}

static void x86_emit_fp_mul(ZeroCCCompiler *c, int dst, int src1, int src2) {
    if (dst == src2 && dst != src1) {
        /* Commutative */
        me_emit_u8(c, 0xF2);
        x86_rex(c, 0, (dst >> 3) & 1, 0, (src1 >> 3) & 1);
        me_emit_u8(c, 0x0F);
        me_emit_u8(c, 0x59);  /* mulsd */
        x86_modrm(c, 3, dst & 7, src1 & 7);
        return;
    }
    if (dst != src1) {
        x86_emit_fp_mov(c, dst, src1);
    }
    me_emit_u8(c, 0xF2);
    x86_rex(c, 0, (dst >> 3) & 1, 0, (src2 >> 3) & 1);
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x59);  /* mulsd */
    x86_modrm(c, 3, dst & 7, src2 & 7);
}

static void x86_emit_fp_div(ZeroCCCompiler *c, int dst, int src1, int src2) {
    if (dst == src2 && dst != src1) {
        /* Non-commutative: moving src1 to dst would overwrite src2. Use xmm7 as scratch. */
        int scratch = 7;
        x86_emit_fp_mov(c, scratch, src2);
        x86_emit_fp_mov(c, dst, src1);
        src2 = scratch;
    } else if (dst != src1) {
        x86_emit_fp_mov(c, dst, src1);
    }
    me_emit_u8(c, 0xF2);
    x86_rex(c, 0, (dst >> 3) & 1, 0, (src2 >> 3) & 1);
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x5E);  /* divsd */
    x86_modrm(c, 3, dst & 7, src2 & 7);
}

static void x86_emit_fp_neg(ZeroCCCompiler *c, int dst, int src) {
    /* xorps with sign bit mask: xorpd dst, [mask] */
    /* First move src to dst if different */
    if (dst != src) {
        /* movsd dst, src (0xF2 before REX: gas order) */
        me_emit_u8(c, 0xF2);
        x86_rex(c, 0, (dst >> 3) & 1, 0, (src >> 3) & 1);
        me_emit_u8(c, 0x0F);
        me_emit_u8(c, 0x10);  /* movsd */
        x86_modrm(c, 3, dst & 7, src & 7);
    }
    /* Use xorpd with sign bit: xorpd xmm_dst, [neg_mask] (RIP-relative) */
    static const double neg_mask = -0.0;
    size_t off = c->rodata_section.length;
    zero_cc_buf_put_bytes(&c->rodata_section, (const uint8_t*)&neg_mask, sizeof(neg_mask));
    me_emit_u8(c, 0x66);
    x86_rex(c, 0, (dst >> 3) & 1, 0, 0);
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x57);  /* xorpd xmm, m128 */
    me_emit_u8(c, 0x05);  /* ModRM: mod=00, reg=dst[0:2], rm=101 (RIP-relative) */
    size_t p = me_get_offset(c);
    me_emit_u32(c, 0);
    if (c->str_reloc_count < ZERO_CC_MAX_STR_RELOCS) {
        c->str_relocs[c->str_reloc_count].text_patch_offset = p;
        c->str_relocs[c->str_reloc_count].rodata_offset = off;
        c->str_reloc_count++;
    }
}

static void x86_emit_fp_cmp(ZeroCCCompiler *c, int src1, int src2) {
    /* ucomisd xmm1, xmm2/m64: reg=src1 (first operand), rm=src2.
     * C13: src1/src2 may be the FP scratch regs (xmm14/15) when the value
     * was spilled - REX.R/REX.B must encode the high bits separately, and
     * REX must be emitted AFTER the 0x66 operand prefix (gas order:
     * 66 44 0F 2F) or the CPU ignores the extension bits. */
    me_emit_u8(c, 0x66);
    x86_rex(c, 0, (src1 >> 3) & 1, 0, (src2 >> 3) & 1);
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x2E);  /* ucomisd (0x2E is ucomisd; 0x2F is comisd) */
    x86_modrm(c, 3, src1 & 7, src2 & 7);
}

static void x86_emit_fp_cvtsi(ZeroCCCompiler *c, int int_dst, int fp_src) {
    /* cvttsd2si reg, xmm: F2 REX.W 0F 2C /r */
    me_emit_u8(c, 0xF2);
    x86_rex(c, 1, (int_dst >> 3) & 1, 0, (fp_src >> 3) & 1);
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x2C);
    x86_modrm(c, 3, int_dst & 7, fp_src & 7);
}

static void x86_emit_fp_cvtsf(ZeroCCCompiler *c, int fp_dst, int int_src) {
    /* cvtsi2sd xmm, reg: F2 REX.W 0F 2A /r */
    me_emit_u8(c, 0xF2);
    x86_rex(c, 1, (fp_dst >> 3) & 1, 0, (int_src >> 3) & 1);
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x2A);
    x86_modrm(c, 3, fp_dst & 7, int_src & 7);
}

static void x86_emit_fp_mov(ZeroCCCompiler *c, int dst, int src) {
    if (dst == src) return;
    me_emit_u8(c, 0xF2);
    x86_rex(c, 0, (dst >> 3) & 1, 0, (src >> 3) & 1);
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x10);  /* movsd */
    x86_modrm(c, 3, dst & 7, src & 7);
}

/* cvtss2sd xmm, xmm: F3 0F 5A /r (dest <- (double)src, low single).
 * NOTE: F3 0F 2A /r is CVTSI2SS (integer -> single), NOT cvtss2sd. */
static void x86_emit_fp_cvtss2sd(ZeroCCCompiler *c, int fp_dst, int fp_src) {
    me_emit_u8(c, 0xF3);
    x86_rex(c, 0, (fp_dst >> 3) & 1, 0, (fp_src >> 3) & 1);
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x5A);
    x86_modrm(c, 3, fp_dst & 7, fp_src & 7);
}

/* cvtsi2ss xmm, r: F3 0F 2A /r (dest <- (float)src, low 32/64-bit int).
 * NOTE: the F2 prefix is CVTSI2SD (int -> double), NOT cvtsi2ss. */
static void x86_emit_fp_cvtsi2ss(ZeroCCCompiler *c, int fp_dst, int int_src) {
    me_emit_u8(c, 0xF3);
    x86_rex(c, 0, (fp_dst >> 3) & 1, 0, (int_src >> 3) & 1);
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x2A);
    x86_modrm(c, 3, fp_dst & 7, int_src & 7);
}

/* cvtsd2ss xmm, xmm: F2 0F 5A /r (dest <- (float)src, low double).
 * NOTE: F3 0F 5A is cvtss2sd; the F2 prefix selects cvtsd2ss. */
static void x86_emit_fp_cvtsd2ss(ZeroCCCompiler *c, int fp_dst, int fp_src) {
    me_emit_u8(c, 0xF2);
    x86_rex(c, 0, (fp_dst >> 3) & 1, 0, (fp_src >> 3) & 1);
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x5A);
    x86_modrm(c, 3, fp_dst & 7, fp_src & 7);
}

static void x86_emit_const(ZeroCCCompiler *c, int64_t value) {
    x86_emit_mov_imm(c, 0, value);  /* rax */
}

static void x86_emit_stack_adjust(ZeroCCCompiler *c, int32_t delta) {
    if (delta == 0) return;
    me_emit_u8(c, 0x48);
    me_emit_u8(c, 0x81);
    me_emit_u8(c, delta > 0 ? 0xC4 : 0xEC);  /* add/sub rsp */
    me_emit_u32(c, (uint32_t)(delta > 0 ? delta : -delta));
}

static void x86_emit_push_reg(ZeroCCCompiler *c, int reg) {
    /* push is 64-bit by default; REX.B only for r8-r15 */
    x86_rex(c, 0, 0, 0, (reg >> 3) & 1);
    me_emit_u8(c, 0x50 | (reg & 7));
}

static void x86_emit_pop_reg(ZeroCCCompiler *c, int reg) {
    x86_rex(c, 0, 0, 0, (reg >> 3) & 1);
    me_emit_u8(c, 0x58 | (reg & 7));
}

/* add dst_reg, imm */
static void x86_emit_add_imm(ZeroCCCompiler *c, int dst_reg, int16_t imm) {
    x86_rex(c, 1, 0, 0, (dst_reg >> 3) & 1);
    if (imm >= -128 && imm <= 127) {
        me_emit_u8(c, 0x83);
        x86_modrm(c, 3, 0, dst_reg & 7);
        me_emit_u8(c, (uint8_t)imm);
    } else {
        me_emit_u8(c, 0x81);
        x86_modrm(c, 3, 0, dst_reg & 7);
        me_emit_u32(c, (uint32_t)(int32_t)imm);
    }
}

/* sub dst_reg, imm */
static void x86_emit_sub_imm(ZeroCCCompiler *c, int dst_reg, int16_t imm) {
    x86_rex(c, 1, 0, 0, (dst_reg >> 3) & 1);
    if (imm >= -128 && imm <= 127) {
        me_emit_u8(c, 0x83);
        x86_modrm(c, 3, 5, dst_reg & 7);
        me_emit_u8(c, (uint8_t)imm);
    } else {
        me_emit_u8(c, 0x81);
        x86_modrm(c, 3, 5, dst_reg & 7);
        me_emit_u32(c, (uint32_t)(int32_t)imm);
    }
}

/* inc dst_reg */
static void x86_emit_inc(ZeroCCCompiler *c, int dst_reg) {
    x86_rex(c, 1, 0, 0, (dst_reg >> 3) & 1);
    me_emit_u8(c, 0xff);
    x86_modrm(c, 3, 0, dst_reg & 7);
}

/* dec dst_reg */
static void x86_emit_dec(ZeroCCCompiler *c, int dst_reg) {
    x86_rex(c, 1, 0, 0, (dst_reg >> 3) & 1);
    me_emit_u8(c, 0xff);
    x86_modrm(c, 3, 1, dst_reg & 7);
}

/* sar dst_reg, cl (arithmetic shift right) */
static void x86_emit_sar(ZeroCCCompiler *c, int dst_reg, int src1, int src2) {
    x86_emit_shift(c, dst_reg, src1, src2, 7);
}

/* call [reg] - call through register */
static void x86_emit_call_reg(ZeroCCCompiler *c, int reg) {
    x86_rex(c, 1, 0, 0, (reg >> 3) & 1);
    me_emit_u8(c, 0xff);
    x86_modrm(c, 3, 2, reg & 7); /* call r/m64 */
}

/* jmp [reg] - indirect jump through register */
static void x86_emit_jmp_reg(ZeroCCCompiler *c, int reg) {
    x86_rex(c, 1, 0, 0, (reg >> 3) & 1);
    me_emit_u8(c, 0xff);
    x86_modrm(c, 3, 4, reg & 7); /* jmp r/m64 */
}

/* leaq disp32(%rip), dst_reg - load label address */
static size_t x86_emit_lea_label(ZeroCCCompiler *c, int dst_reg) {
    x86_rex(c, 1, (dst_reg >> 3) & 1, 0, 0);
    me_emit_u8(c, 0x8D);
    x86_modrm(c, 0, dst_reg & 7, 5);  /* Mod=00, R/M=5 (RIP-relative) */
    size_t patch_pos = me_get_offset(c);
    me_emit_u32(c, 0);
    return patch_pos;
}

/* stack_align - ensure 16-byte stack alignment (x86-64 already handles this) */
static void x86_emit_stack_align(ZeroCCCompiler *c, uint8_t alignment) {
    /* x86-64 ABI requires 16-byte alignment before call.
     * This is typically handled by prologue/epilogue. */
    (void)c;
    (void)alignment;
}

/* shr dst_reg, cl (dst = src1 >> src2) */
static void x86_emit_shr(ZeroCCCompiler *c, int dst_reg, int src1, int src2) {
    x86_emit_shift(c, dst_reg, src1, src2, 5);
}

/* Generic condition code setting into dst_reg (zero-extended to 64-bit) */
static void x86_emit_setcc(ZeroCCCompiler *c, int dst_reg, uint8_t cond_byte) {
    /* setcc dst8 */
    me_emit_u8(c, 0x40 | ((dst_reg >> 3) & 1));
    me_emit_u8(c, 0x0f);
    me_emit_u8(c, cond_byte);
    x86_modrm(c, 3, 0, dst_reg & 7);
    /* movzx dst64, dst8 */
    x86_rex(c, 1, (dst_reg >> 3) & 1, 0, (dst_reg >> 3) & 1);
    me_emit_u8(c, 0x0f);
    me_emit_u8(c, 0xb6);
    x86_modrm(c, 3, dst_reg & 7, dst_reg & 7);
}

/* setl dst_reg */
static void x86_emit_setl(ZeroCCCompiler *c, int dst_reg) {
    x86_emit_setcc(c, dst_reg, 0x9c);
}

/* setle dst_reg */
static void x86_emit_setle(ZeroCCCompiler *c, int dst_reg) {
    x86_emit_setcc(c, dst_reg, 0x9e);
}

/* setg dst_reg */
static void x86_emit_setg(ZeroCCCompiler *c, int dst_reg) {
    x86_emit_setcc(c, dst_reg, 0x9f);
}

/* setge dst_reg */
static void x86_emit_setge(ZeroCCCompiler *c, int dst_reg) {
    x86_emit_setcc(c, dst_reg, 0x9d);
}

/* sete dst_reg */
static void x86_emit_sete(ZeroCCCompiler *c, int dst_reg) {
    x86_emit_setcc(c, dst_reg, 0x94);
}

/* setne dst_reg */
static void x86_emit_setne(ZeroCCCompiler *c, int dst_reg) {
    x86_emit_setcc(c, dst_reg, 0x95);
}

/* deref: mov dst, [src] */
static void x86_emit_deref(ZeroCCCompiler *c, int dst_reg, int src_reg) {
    x86_rex(c, 1, (dst_reg >> 3) & 1, 0, (src_reg >> 3) & 1);
    me_emit_u8(c, 0x8B);
    int mod = ((src_reg & 7) == X86_RBP) ? 1 : 0;
    x86_modrm(c, mod, dst_reg & 7, src_reg & 7);
    if ((src_reg & 7) == X86_RSP) me_emit_u8(c, 0x24);
    if (mod == 1) me_emit_u8(c, 0);
}

/* load_byte: movzx dst, byte ptr [src] */
static void x86_emit_load_byte(ZeroCCCompiler *c, int dst_reg, int src_reg) {
    x86_rex(c, 1, (dst_reg >> 3) & 1, 0, (src_reg >> 3) & 1);
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0xB6);
    int mod = ((src_reg & 7) == X86_RBP) ? 1 : 0;
    x86_modrm(c, mod, dst_reg & 7, src_reg & 7);
    if ((src_reg & 7) == X86_RSP) me_emit_u8(c, 0x24);
    if (mod == 1) me_emit_u8(c, 0);
}

static void x86_emit_load_16(ZeroCCCompiler *c, int dst_reg, int src_reg) {
    x86_rex(c, 0, (dst_reg >> 3) & 1, 0, (src_reg >> 3) & 1);
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0xB7);
    int mod = ((src_reg & 7) == X86_RBP) ? 1 : 0;
    x86_modrm(c, mod, dst_reg & 7, src_reg & 7);
    if ((src_reg & 7) == X86_RSP) me_emit_u8(c, 0x24);
    if (mod == 1) me_emit_u8(c, 0);
}

static void x86_emit_load_32(ZeroCCCompiler *c, int dst_reg, int src_reg) {
    x86_rex(c, 0, (dst_reg >> 3) & 1, 0, (src_reg >> 3) & 1);
    me_emit_u8(c, 0x8B);
    int mod = ((src_reg & 7) == X86_RBP) ? 1 : 0;
    x86_modrm(c, mod, dst_reg & 7, src_reg & 7);
    if ((src_reg & 7) == X86_RSP) me_emit_u8(c, 0x24);
    if (mod == 1) me_emit_u8(c, 0);
}

static void x86_emit_store_byte(ZeroCCCompiler *c, int src_reg, int base_reg) {
    /* x86-64: SIL/DIL/SPL/BPL (regs 4-7) and r8b-r15b (regs >= 8) require a REX prefix to encode as byte registers */
    if (src_reg >= 4 || base_reg >= 8) {
        me_emit_u8(c, 0x40 | (((src_reg >> 3) & 1) << 2) | ((base_reg >> 3) & 1));
    }
    me_emit_u8(c, 0x88);
    int mod = ((base_reg & 7) == X86_RBP) ? 1 : 0;
    x86_modrm(c, mod, src_reg & 7, base_reg & 7);
    if ((base_reg & 7) == X86_RSP) me_emit_u8(c, 0x24);
    if (mod == 1) me_emit_u8(c, 0);
}

static void x86_emit_store_16(ZeroCCCompiler *c, int src_reg, int base_reg) {
    me_emit_u8(c, 0x66);
    x86_rex(c, 0, (src_reg >> 3) & 1, 0, (base_reg >> 3) & 1);
    me_emit_u8(c, 0x89);
    int mod = ((base_reg & 7) == X86_RBP) ? 1 : 0;
    x86_modrm(c, mod, src_reg & 7, base_reg & 7);
    if ((base_reg & 7) == X86_RSP) me_emit_u8(c, 0x24);
    if (mod == 1) me_emit_u8(c, 0);
}

static void x86_emit_store_32(ZeroCCCompiler *c, int src_reg, int base_reg) {
    x86_rex(c, 0, (src_reg >> 3) & 1, 0, (base_reg >> 3) & 1);
    me_emit_u8(c, 0x89);
    int mod = ((base_reg & 7) == X86_RBP) ? 1 : 0;
    x86_modrm(c, mod, src_reg & 7, base_reg & 7);
    if ((base_reg & 7) == X86_RSP) me_emit_u8(c, 0x24);
    if (mod == 1) me_emit_u8(c, 0);
}

static void x86_emit_store_64(ZeroCCCompiler *c, int src_reg, int base_reg) {
    x86_rex(c, 1, (src_reg >> 3) & 1, 0, (base_reg >> 3) & 1);
    me_emit_u8(c, 0x89);
    int mod = ((base_reg & 7) == X86_RBP) ? 1 : 0;
    x86_modrm(c, mod, src_reg & 7, base_reg & 7);
    if ((base_reg & 7) == X86_RSP) me_emit_u8(c, 0x24);
    if (mod == 1) me_emit_u8(c, 0);
}

static void x86_emit_vla_alloc(ZeroCCCompiler *c, int dst_reg, int size_reg) {
    int32_t sp_disp = c->lowering.vla.cur_sp_slot_disp;
    if (sp_disp != 0) {
        /* cmpq $0, sp_disp(%rbp): 48 83 BD <disp32> 00 (8B) */
        me_emit_u8(c, 0x48);
        me_emit_u8(c, 0x83);
        me_emit_u8(c, 0xBD);
        me_emit_u32(c, (uint32_t)sp_disp);
        me_emit_u8(c, 0x00);
        /* jne .Lrestore (+9B): 75 09 (2B) */
        me_emit_u8(c, 0x75);
        me_emit_u8(c, 0x09);
        /* movq %rsp, sp_disp(%rbp): 48 89 A5 <disp32> (7B) */
        me_emit_u8(c, 0x48);
        me_emit_u8(c, 0x89);
        me_emit_u8(c, 0xA5);
        me_emit_u32(c, (uint32_t)sp_disp);
        /* jmp .Lalloc (+7B): EB 07 (2B) */
        me_emit_u8(c, 0xEB);
        me_emit_u8(c, 0x07);
        /* .Lrestore: movq sp_disp(%rbp), %rsp: 48 8B A5 <disp32> (7B) */
        me_emit_u8(c, 0x48);
        me_emit_u8(c, 0x8B);
        me_emit_u8(c, 0xA5);
        me_emit_u32(c, (uint32_t)sp_disp);
    }
    /* sub rsp, size_reg: 48 29 /r */
    x86_rex(c, 1, (size_reg >> 3) & 1, 0, 0);
    me_emit_u8(c, 0x29);
    x86_modrm(c, 3, size_reg & 7, X86_RSP);
    /* mov dst_reg, rsp: 48 89 /r */
    x86_mov_rr(c, dst_reg, X86_RSP);
}

/* Byte-width atomics: unextended ModRM fields 4-7 name AH/CH/DH/BH, so a
 * value register of RSP/RBP/RSI/RDI (4-7) must be relocated to scratch R11
 * (whose byte BBL encodes via REX.R) before use. */
static void x86_emit_atomic_xadd(ZeroCCCompiler *c, int dst_reg, int addr_reg, int val_reg, int width) {
    if (width == 1 && val_reg >= 4 && val_reg < 8) {
        x86_mov_rr(c, X86_R11, val_reg);
        val_reg = X86_R11;
    }
    /* lock prefix: 0xF0 */
    me_emit_u8(c, 0xF0);
    if (width == 2) me_emit_u8(c, 0x66);
    x86_rex(c, (width == 8) ? 1 : 0, (val_reg >> 3) & 1, 0, (addr_reg >> 3) & 1);
    if (width == 1) {
        me_emit_u8(c, 0x0F);
        me_emit_u8(c, 0xC0);
    } else {
        me_emit_u8(c, 0x0F);
        me_emit_u8(c, 0xC1);
    }
    int mod = ((addr_reg & 7) == X86_RBP) ? 1 : 0;
    x86_modrm(c, mod, val_reg & 7, addr_reg & 7);
    if ((addr_reg & 7) == X86_RSP) me_emit_u8(c, 0x24);
    if (mod == 1) me_emit_u8(c, 0);

    /* After xadd, val_reg holds the old value. For width 1 only the low byte
     * changed (upper bits stale), so zero-extend from the byte register. */
    if (width == 1) {
        x86_rex(c, 1, (dst_reg >> 3) & 1, 0, (val_reg >> 3) & 1);
        me_emit_u8(c, 0x0F);
        me_emit_u8(c, 0xB6);
        x86_modrm(c, 3, dst_reg & 7, val_reg & 7);
    } else if (dst_reg != val_reg) {
        x86_mov_rr(c, dst_reg, val_reg);
    }
}

/* Shared emitter for OP_ATOMIC_OR/AND/XOR (C11 __atomic_fetch_or/and/xor,
 * GCC's pattern): snapshot [addr] into dst, then atomically update memory
 * in place with `lock or/and/xor val, [addr]`. After the sequence, memory
 * holds the updated value and dst holds the pre-update snapshot. */
static void x86_emit_atomic_bitwise(ZeroCCCompiler *c, int dst_reg, int addr_reg, int val_reg, int width, uint8_t opcode32, uint8_t opcode8) {
    /* 1) dst = [addr] (pre-update snapshot) */
    if (width == 1) me_emit_u8(c, 0x66);
    x86_rex(c, (width == 8) ? 1 : 0, (dst_reg >> 3) & 1, 0, (addr_reg >> 3) & 1);
    me_emit_u8(c, (width == 1) ? 0x8A : 0x8B);
    int mod = ((addr_reg & 7) == X86_RBP) ? 1 : 0;
    x86_modrm(c, mod, dst_reg & 7, addr_reg & 7);
    if ((addr_reg & 7) == X86_RSP) me_emit_u8(c, 0x24);
    if (mod == 1) me_emit_u8(c, 0);

    /* 2) lock <op> val, [addr] (atomic in-place update). The /r source of a
     * byte op must be AL/CL/DL/BL/R8B..R15B - relocate RSI/RDI into R11. */
    if (width == 1 && val_reg >= 4 && val_reg < 8) {
        x86_mov_rr(c, X86_R11, val_reg);
        val_reg = X86_R11;
    }
    me_emit_u8(c, 0xF0);
    if (width <= 2) me_emit_u8(c, 0x66);
    x86_rex(c, (width == 8) ? 1 : 0, (val_reg >> 3) & 1, 0, (addr_reg >> 3) & 1);
    me_emit_u8(c, (width == 1) ? opcode8 : opcode32);
    x86_modrm(c, mod, val_reg & 7, addr_reg & 7);
    if ((addr_reg & 7) == X86_RSP) me_emit_u8(c, 0x24);
    if (mod == 1) me_emit_u8(c, 0);
}

static void x86_emit_atomic_or(ZeroCCCompiler *c, int dst_reg, int addr_reg, int val_reg, int width) {
    x86_emit_atomic_bitwise(c, dst_reg, addr_reg, val_reg, width, 0x09, 0x08); /* or r/m, r */
}

static void x86_emit_atomic_and(ZeroCCCompiler *c, int dst_reg, int addr_reg, int val_reg, int width) {
    x86_emit_atomic_bitwise(c, dst_reg, addr_reg, val_reg, width, 0x21, 0x10); /* and r/m, r */
}

static void x86_emit_atomic_xor(ZeroCCCompiler *c, int dst_reg, int addr_reg, int val_reg, int width) {
    x86_emit_atomic_bitwise(c, dst_reg, addr_reg, val_reg, width, 0x31, 0x30); /* xor r/m, r */
}

static void x86_emit_atomic_cmpxchg(ZeroCCCompiler *c, int dst_reg, int addr_reg, int expected_reg, int desired_reg, int width) {
    /* If addr_reg or desired_reg is RAX, preserve them in a scratch register before RAX is overwritten with expected_reg */
    if (addr_reg == X86_RAX) {
        x86_mov_rr(c, 10 /* R10 */, addr_reg);
        addr_reg = 10;
    }
    if (desired_reg == X86_RAX) {
        x86_mov_rr(c, 11 /* R11 */, desired_reg);
        desired_reg = 11;
    }
    /* Byte cmpxchg source must be AL/CL/DL/BL/R8B..R15B: relocate RSI/RDI
     * (fields 6-7 name DH/BH unextended) into scratch R11 (BBL). */
    if (width == 1 && desired_reg >= 4 && desired_reg < 8) {
        x86_mov_rr(c, 11 /* R11 */, desired_reg);
        desired_reg = 11;
    }
    if (expected_reg != X86_RAX) {
        x86_mov_rr(c, X86_RAX, expected_reg);
    }
    /* lock prefix: 0xF0 */
    me_emit_u8(c, 0xF0);
    if (width == 2) me_emit_u8(c, 0x66);
    x86_rex(c, (width == 8) ? 1 : 0, (desired_reg >> 3) & 1, 0, (addr_reg >> 3) & 1);
    if (width == 1) {
        me_emit_u8(c, 0x0F);
        me_emit_u8(c, 0xB0);
    } else {
        me_emit_u8(c, 0x0F);
        me_emit_u8(c, 0xB1);
    }
    int mod = ((addr_reg & 7) == X86_RBP) ? 1 : 0;
    x86_modrm(c, mod, desired_reg & 7, addr_reg & 7);
    if ((addr_reg & 7) == X86_RSP) me_emit_u8(c, 0x24);
    if (mod == 1) me_emit_u8(c, 0);

    /* Old value from memory is in RAX. For width 1 only AL is defined
     * (byte cmpxchg leaves the upper bits stale), so zero-extend from AL. */
    if (width == 1) {
        x86_rex(c, 1, (dst_reg >> 3) & 1, 0, 0);
        me_emit_u8(c, 0x0F);
        me_emit_u8(c, 0xB6);
        x86_modrm(c, 3, dst_reg & 7, 0);
    } else if (dst_reg != X86_RAX) {
        x86_mov_rr(c, dst_reg, X86_RAX);
    }
}

static void x86_emit_atomic_cmpxchg_bool(ZeroCCCompiler *c, int dst_reg, int addr_reg, int expected_reg, int desired_reg, int width) {
    /* If addr_reg or desired_reg is RAX, preserve them in a scratch register before RAX is overwritten with expected_reg */
    if (addr_reg == X86_RAX) {
        x86_mov_rr(c, 10 /* R10 */, addr_reg);
        addr_reg = 10;
    }
    if (desired_reg == X86_RAX) {
        x86_mov_rr(c, 11 /* R11 */, desired_reg);
        desired_reg = 11;
    }
    /* Byte cmpxchg source must be a valid byte register (see cmpxchg). */
    if (width == 1 && desired_reg >= 4 && desired_reg < 8) {
        x86_mov_rr(c, 11 /* R11 */, desired_reg);
        desired_reg = 11;
    }
    if (expected_reg != X86_RAX) {
        x86_mov_rr(c, X86_RAX, expected_reg);
    }
    /* lock prefix: 0xF0 */
    me_emit_u8(c, 0xF0);
    if (width == 2) me_emit_u8(c, 0x66);
    x86_rex(c, (width == 8) ? 1 : 0, (desired_reg >> 3) & 1, 0, (addr_reg >> 3) & 1);
    if (width == 1) {
        me_emit_u8(c, 0x0F);
        me_emit_u8(c, 0xB0);
    } else {
        me_emit_u8(c, 0x0F);
        me_emit_u8(c, 0xB1);
    }
    int mod = ((addr_reg & 7) == X86_RBP) ? 1 : 0;
    x86_modrm(c, mod, desired_reg & 7, addr_reg & 7);
    if ((addr_reg & 7) == X86_RSP) me_emit_u8(c, 0x24);
    if (mod == 1) me_emit_u8(c, 0);

    /* sete al: 0F 94 C0 */
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x94);
    me_emit_u8(c, 0xC0);
    /* movzx eax, al: 0F B6 C0 */
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0xB6);
    me_emit_u8(c, 0xC0);

    if (dst_reg != X86_RAX) {
        x86_mov_rr(c, dst_reg, X86_RAX);
    }
}

static void x86_emit_atomic_xchg(ZeroCCCompiler *c, int dst_reg, int addr_reg, int val_reg, int width) {
    if (width == 1 && val_reg >= 4 && val_reg < 8) {
        x86_mov_rr(c, X86_R11, val_reg);
        val_reg = X86_R11;
    }
    if (width == 2) me_emit_u8(c, 0x66);
    x86_rex(c, (width == 8) ? 1 : 0, (val_reg >> 3) & 1, 0, (addr_reg >> 3) & 1);
    if (width == 1) {
        me_emit_u8(c, 0x86);
    } else {
        me_emit_u8(c, 0x87);
    }
    int mod = ((addr_reg & 7) == X86_RBP) ? 1 : 0;
    x86_modrm(c, mod, val_reg & 7, addr_reg & 7);
    if ((addr_reg & 7) == X86_RSP) me_emit_u8(c, 0x24);
    if (mod == 1) me_emit_u8(c, 0);

    if (width == 1) {
        x86_rex(c, 1, (dst_reg >> 3) & 1, 0, (val_reg >> 3) & 1);
        me_emit_u8(c, 0x0F);
        me_emit_u8(c, 0xB6);
        x86_modrm(c, 3, dst_reg & 7, val_reg & 7);
    } else if (dst_reg != val_reg) {
        x86_mov_rr(c, dst_reg, val_reg);
    }
}

static void x86_emit_mfence(ZeroCCCompiler *c) {
    /* mfence: 0F AE F0 */
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0xAE);
    me_emit_u8(c, 0xF0);
}

static void x86_emit_trap(ZeroCCCompiler *c) {
    /* ud2: 0F 0B (__builtin_trap / __builtin_unreachable) */
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x0B);
}

static void x86_emit_trap_asm(ZeroCCCompiler *c, FILE *out) {
    (void)c;
    fprintf(out, "\tud2\n");
}

/* OS syscall (SysV AMD64): eax = number, rdi/rsi/rdx/r10/r8/r9 = args 1-6.
 * The result is left in rax (VREG_0). Argument temps are resolved first
 * (a spilled argument is reloaded into scratch_reg2 = r11, a non-syscall
 * register that the syscall itself clobbers, so any value the allocator
 * placed there is already spilled across this instruction), then moved into
 * the syscall registers with a push/pop parallel move - an argument temp may
 * share a register with ANOTHER argument's syscall slot (e.g. the count in
 * rax while the number also targets rax), so naive sequential movs clobber
 * live argument values. The syscall instruction zero-extends eax on entry,
 * so a plain 64-bit move of the number is correct. */
static void x86_emit_syscall(ZeroCCCompiler *c, int arg_count, const int *arg_temps) {
    /* syscall register per argument slot: [number, a1, a2, a3, a4, a5, a6] */
    static const int sysv_syscall_regs[7] = {0, 7, 6, 2, 10, 8, 9}; /* rax rdi rsi rdx r10 r8 r9 */
    int32_t spill_base_offset = c->regalloc.spill_base_offset;
    int src_regs[8] = {0};
    /* Pass 1: resolve every source (spill reloads interleaved), push the
     * ones that still need a move. */
    for (int a = 0; a < arg_count && a < 7; a++) {
        int temp = arg_temps[a];
        int src_reg;
        int sp = regalloc_get_spill_slot_for_temp(&c->regalloc, temp);
        if (sp >= 0) {
            int32_t off = spill_base_offset + (sp + 1) * 8;
            if (BE(c)->emit_load) BE(c)->emit_load(c, BE(c)->scratch_reg2, BE(c)->fp_reg, -off);
            src_reg = BE(c)->scratch_reg2;
        } else {
            src_reg = regalloc_get_reg_for_temp(&c->regalloc, temp);
        }
        src_regs[a] = src_reg;
        if (src_reg != sysv_syscall_regs[a] && BE(c)->emit_push_reg) {
            BE(c)->emit_push_reg(c, src_reg);
        }
    }
    /* Pass 2: pop into the syscall registers (reverse order - LIFO). */
    for (int a = (arg_count < 7) ? arg_count - 1 : 6; a >= 0; a--) {
        if (src_regs[a] == sysv_syscall_regs[a]) continue;
        if (BE(c)->emit_pop_reg) BE(c)->emit_pop_reg(c, sysv_syscall_regs[a]);
    }
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x05); /* syscall */
}

/* cmp src_reg, 0 */
static void x86_emit_cmp_zero(ZeroCCCompiler *c, int src_reg) {
    x86_alu(c, 0x85, src_reg, src_reg);  /* test src_reg, src_reg */
}

/* not dst_reg */
static void x86_emit_not(ZeroCCCompiler *c, int dst_reg, int src_reg) {
    if (dst_reg != src_reg) {
        x86_mov_rr(c, dst_reg, src_reg);
    }
    x86_rex(c, 1, 0, 0, (dst_reg >> 3) & 1);
    me_emit_u8(c, 0xf7);
    x86_modrm(c, 3, 2, dst_reg & 7);
}

/* lnot: !src_reg */
static void x86_emit_lnot(ZeroCCCompiler *c, int dst_reg, int src_reg) {
    x86_alu(c, 0x85, src_reg, src_reg);  /* test src_reg, src_reg */
    x86_emit_sete(c, dst_reg);
}

static void x86_emit_label(ZeroCCCompiler *c, const char *name) {
    /* Record label position for jump patching */
    if (c->label_count < 32) {
        c->labels[c->label_count].name = name;
        c->labels[c->label_count].offset = me_get_offset(c);
        c->labels[c->label_count].patch_count = 0;
        c->label_count++;
    }
}

static void x86_emit_test(ZeroCCCompiler *c, int src1, int src2) {
    /* test src1, src2 */
    x86_alu(c, 0x85, src1, src2);
}

static void x86_emit_lea(ZeroCCCompiler *c, int dst, int base, int32_t offset) {
    /* lea dst, [base + offset] */
    x86_rm(c, 0x8D, dst, base, offset);
}

/* RIP-relative addressing for string literals */
static void x86_emit_lea_rip(ZeroCCCompiler *c, int dst_reg, size_t rodata_offset) {
    x86_rex(c, 1, (dst_reg >> 3) & 1, 0, 0);
    me_emit_u8(c, 0x8D);
    x86_modrm(c, 0, dst_reg & 7, 5);  /* Mod=00, R/M=5 (RIP-relative) */
    size_t patch_pos = me_get_offset(c);
    me_emit_u32(c, 0);
    if (c->str_reloc_count < ZERO_CC_MAX_STR_RELOCS) {
        c->str_relocs[c->str_reloc_count].text_patch_offset = patch_pos;
        c->str_relocs[c->str_reloc_count].rodata_offset = rodata_offset;
        c->str_relocs[c->str_reloc_count].dst_reg = dst_reg;
        c->str_reloc_count++;
    }
}

/* Parameter spilling: integer register to stack */
static void x86_emit_param_save_int(ZeroCCCompiler *c, int reg_idx, int32_t offset) {
    /* mov base,disp <- reg; base = FP (or SP with delta, Option 4) */
    if (reg_idx >= 0 && reg_idx < 6) {
        static const uint8_t arg_regs[6] = {7, 6, 2, 1, 8, 9}; /* rdi rsi rdx rcx r8 r9 */
        x86_rm(c, 0x89, arg_regs[reg_idx], zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -offset));
    }
}

/* Parameter spilling: float register (XMM) to stack */
static void x86_emit_param_save_fp(ZeroCCCompiler *c, int reg_idx, int32_t offset, int is_single) {
    if (reg_idx < 0 || reg_idx >= 8) return;
    int base = zero_cc_frame_base_reg(c);
    int32_t disp = zero_cc_frame_disp(c, -offset);
    if (is_single) {
        /* Sprint 115m: single-precision params are saved RAW - the low 32
         * bits of the incoming register ARE the ABI float pattern. x86 keeps
         * scalar floats as doubles in registers, so the old narrowing store
         * (vstoress = cvttsd2ss + movss) interpreted the low-32 float as a
         * model double and destroyed the value an external caller (glibc
         * csinf/sinf) placed in the register. */
        x86_emit_vstoress_raw(c, reg_idx, base, disp);
    } else {
        x86_emit_store_fp(c, reg_idx, base, disp);
    }
}

/* Variadic spill: spill remaining integer registers for va_start */
static void x86_emit_variadic_spill(ZeroCCCompiler *c, int param_count, int32_t base_offset) {
    (void)param_count;
    /* Save area (16 8-byte slots above the locals; see the variadic spill
     * reservation in zero_cc_lower_ast): slots 0..5 = rdi rsi rdx rcx r8
     * r9, slots 6..13 = lower 64 bits of xmm0..xmm7. All registers are
     * spilled unconditionally - va_start points the streams just past the
     * slots consumed by named parameters. The FP spill is what makes
     * va_arg(ap, float/double) work: without it the xmm arg regs were never
     * copied into the va area and va_arg read stale stack memory. */
    static const uint8_t arg_regs[6] = {7, 6, 2, 1, 8, 9}; /* rdi rsi rdx rcx r8 r9 */
    int base = zero_cc_frame_base_reg(c);
    int gp_regs = BE(c)->conv->int_arg_reg_count;
    int fp_regs = BE(c)->conv->fp_arg_reg_count;
    for (int i = 0; i < gp_regs; i++) {
        x86_rm(c, 0x89, arg_regs[i], base, zero_cc_frame_disp(c, -(base_offset + (int32_t)i * 8)));
    }
    for (int i = 0; i < fp_regs; i++) {
        x86_emit_store_fp(c, i, base, zero_cc_frame_disp(c, -(base_offset + (int32_t)(gp_regs + i) * 8)));
    }
}

/* Variadic AL setup: set %al to number of SSE registers used */
static void x86_emit_variadic_al_setup(ZeroCCCompiler *c, int num_fp_args) {
    if (num_fp_args > 0) {
        zero_cc_emit_u8(&c->text_section, 0xB0);  /* mov al, imm8 */
        zero_cc_emit_u8(&c->text_section, (uint8_t)num_fp_args);
    }
}

/* Resolve all recorded label patches */
static void x86_resolve_labels(ZeroCCCompiler *c) {
    for (int i = 0; i < c->label_count; i++) {
        size_t target = c->labels[i].offset;
        for (int j = 0; j < c->labels[i].patch_count; j++) {
            size_t patch_offset = c->labels[i].patches[j];
            int32_t rel = (int32_t)(target - (patch_offset + 4));
            if (patch_offset + 4 <= c->text_section.length) {
                me_patch_u32(c, patch_offset, (uint32_t)rel);
            }
        }
    }
}

/* patch_offset is the position of the rel32 field (as returned by emit_j*) */
static size_t x86_patch_jmp(ZeroCCCompiler *c, size_t patch_offset, size_t target_offset) {
    int32_t rel = (int32_t)(target_offset - (patch_offset + 4));
    if (patch_offset + 4 <= c->text_section.length) {
        me_patch_u32(c, patch_offset, (uint32_t)rel);
    }
    return patch_offset;
}

/* x86-64 backend instance */

/* x86-64 VREG mapping: direct pass-through (VREG numbers = x86 encodings) */
static int x86_get_int_reg(int vreg) { return vreg; }
static int x86_get_fp_reg(int vreg) { return vreg; }

/* =====================================================================
 * Phase 3b: trampoline stub for a capturing nested function.
 *
 * `fp = f2; fp(x)` cannot call the mangled nested function directly: its
 * ABI has the hidden capture parameters. The stub (emitted right after
 * the nested fn in the same text image) takes the USER arguments, loads
 * the captures from the static ctx object, re-places everything in the
 * nested fn's argument slots and `call`s the real function. The real
 * fn's `ret` lands on `leave; ret` below, returning to the ORIGINAL
 * caller (the stub's frame is what the target sees as its caller).
 *
 * Argument model (mirrors the CALL lowering): an 8-byte unit stream per
 * class - integer units take consecutive GPR slots (a 9-16B struct is
 * 2 units, 17-32B is 4; >32B is a hidden pointer = 1 unit), FP units
 * take XMM slots (complex = 2 units, else 1). Units past the 6th GPR /
 * 8th XMM slot go on the call stack (8 bytes each). Stack layout
 * matches zerocc's CALL emitter: the first stack unit is pushed LAST
 * (lowest address) and, at the `call`, sits at [rsp_call+8] so the
 * callee reads it from [rsp+8] (frameless) / [rbp+16] (framed).
 * A >16B struct return adds the sret pointer as the FIRST integer unit
 * (it arrives in RDI and passes straight through to final slot 0).
 *
 * Frame layout (rbp-relative):
 *   [rbp-8..-48]    entry GPR arg regs (rdi,rsi,rdx,rcx,r8,r9)
 *   [rbp-64-16x]    entry XMM arg regs xmm0..7 (16-aligned)
 *   [rbp-192-8m]    entry stack units (8B each)
 * ===================================================================== */

#define TRAMP_MAX_UNITS 24

typedef struct {
    bool is_fp;       /* FP unit (XMM stream) vs integer unit (GPR stream) */
    int  fin_slot;    /* final register slot (0..5 GPR / 0..7 XMM), -1 = stack */
    int  fin_stack;   /* final stack unit index, -1 = register */
    int  src;         /* value source: 0=ctx, 1=frame8, 2=xmmframe16, 3=stacksave8 */
    int  src_off;     /* rbp-relative frame offset (src 1/2/3) or ctx offset (src 0) */
} ZeroCCTrampUnit;

static int tramp_unit_gpr_slots(const ZeroCCType *t) {
    if ((t->tag == TYPE_STRUCT || t->tag == TYPE_UNION) && t->is_vla_struct)
        return 2; /* (hidden pointer, total size) GPR pair */
    if (t->size > 32) return 1; /* hidden pointer convention */
    size_t s = t->size ? t->size : 8;
    return (int)((s + 7) / 8);
}
static bool tramp_unit_is_fp(const ZeroCCType *t) {
    return (t->tag == TYPE_FLOAT || t->tag == TYPE_DOUBLE);
}
static int tramp_unit_slots(const ZeroCCType *t) {
    if (tramp_unit_is_fp(t)) return t->is_complex ? 2 : 1;
    return tramp_unit_gpr_slots(t);
}

/* Load xmm from [base + disp]: 16B via movdqu (F3 0F 6F /r, no alignment
 * requirement) or 8B via movsd (F2 0F 10 /r). NOTE: F2 0F 6E/6F is NOT a
 * valid 16-byte XMM load - that was a latent Sprint-93 bug (corrupted
 * final FP placement for nested functions with FP user args). */
static void tramp_mov_xmm_mem(ZeroCCCompiler *c, int xmm, int base, int32_t off, bool wide16) {
    x86_rex(c, 1, (xmm >> 3) & 1, 0, (base >> 3) & 1);
    me_emit_u8(c, wide16 ? 0xF3 : 0xF2);
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, wide16 ? 0x6F : 0x10);
    if (off >= -128 && off <= 127 && off != 0 || (off == 0 && (base & 7) == X86_RBP)) {
        x86_modrm(c, 1, xmm & 7, base & 7);
        me_emit_u8(c, (uint8_t)(int8_t)off);
    } else {
        x86_modrm(c, 2, xmm & 7, base & 7);
        if ((base & 7) == X86_RSP) me_emit_u8(c, 0x24);
        me_emit_u32(c, (uint32_t)off);
    }
}

/* push [rbp|rm + disp] (REX.W FF /6) */
static void tramp_push_mem(ZeroCCCompiler *c, int base, int32_t off) {
    x86_rex(c, 1, 0, 0, (base >> 3) & 1);
    me_emit_u8(c, 0xFF);
    if (off >= -128 && off <= 127) {
        x86_modrm(c, 1, 6, base & 7);
        me_emit_u8(c, (uint8_t)(int8_t)off);
    } else {
        x86_modrm(c, 2, 6, base & 7);
        if ((base & 7) == X86_RSP) me_emit_u8(c, 0x24);
        me_emit_u32(c, (uint32_t)off);
    }
}

void zero_cc_x86_emit_tramp_stub(ZeroCCCompiler *c, const ZeroCCFunction *nf) {
    size_t C = nf->nested_captured_count;
    size_t U = nf->param_count - C;
    bool has_sret = (nf->ret_struct_sz > 16);

    /* ---- build the final unit list: [sret?] + captures + user params ----
     * Final slots are assigned in stream order per class; overflows become
     * final stack units in combined declaration order (captures first). */
    ZeroCCTrampUnit u[TRAMP_MAX_UNITS];
    int n = 0, gpr_run = 0, xmm_run = 0, decl_stack_run = 0;

    int gpr_entry = has_sret ? 1 : 0, xmm_entry = 0, entry_stack_n = 0;

    /* sret pointer: entry RDI -> final GPR slot 0 (passes through) */
    if (has_sret) {
        u[n].is_fp = false;
        u[n].fin_slot = gpr_run < 6 ? gpr_run++ : -1;
        u[n].fin_stack = u[n].fin_slot < 0 ? decl_stack_run++ : -1;
        u[n].src = 1; u[n].src_off = -8;      /* the saved RDI */
        n++;
    }
    /* captures: values come from ctx (32 bytes per capture, word j at
     * 32*ci + 8j). */
    for (size_t ci = 0; ci < C; ci++) {
        int slots;
        bool isfp;
        if (nf->nested_capture_byref[ci]) {
            slots = 1; isfp = false;
        } else {
            const ZeroCCType *t = &nf->nested_capture_types[ci];
            slots = tramp_unit_slots(t);
            isfp = tramp_unit_is_fp(t);
        }
        for (int w = 0; w < slots; w++) {
            if (n >= TRAMP_MAX_UNITS) return;
            u[n].is_fp = isfp;
            if (isfp)      u[n].fin_slot = xmm_run < 8 ? xmm_run++ : -1;
            else           u[n].fin_slot = gpr_run < 6 ? gpr_run++ : -1;
            u[n].fin_stack = u[n].fin_slot < 0 ? decl_stack_run++ : -1;
            u[n].src = 0;
            u[n].src_off = (int)(32 * ci) + 8 * w;
            n++;
        }
    }
    /* Sprint 99: nonlocal goto pairs, between captures and user params:
     * pair i holds (rbp, rsp) of the i-th ancestor scope; ctx words at
     * 32*C + 16*i (rbp) and +8 (rsp). */
    for (int pi = 0; pi < nf->goto_pair_count; pi++) {
        for (int w = 0; w < 2; w++) {
            if (n >= TRAMP_MAX_UNITS) return;
            u[n].is_fp = false;
            u[n].fin_slot = gpr_run < 6 ? gpr_run++ : -1;
            u[n].fin_stack = u[n].fin_slot < 0 ? decl_stack_run++ : -1;
            u[n].src = 0;
            u[n].src_off = (int)(32 * C + 16 * pi) + 8 * w;
            n++;
        }
    }
    /* user params: values come from the entry layout (register saves or
     * entry stack saves). Entry slots: gpr unit k -> k + sret_shift;
     * xmm unit k -> k; past the cap -> entry stack units in declaration
     * order. */
    int ugpr = 0, uxmm = 0;
    for (size_t ui = 0; ui < U; ui++) {
        const ZeroCCType *t = &nf->param_types[C + ui];
        int slots = tramp_unit_slots(t);
        bool isfp = tramp_unit_is_fp(t);
        for (int w = 0; w < slots; w++) {
            if (n >= TRAMP_MAX_UNITS) return;
            u[n].is_fp = isfp;
            int entry_slot, entry_stack = -1;
            if (isfp) {
                entry_slot = uxmm++;
                if (entry_slot >= 8) { entry_slot = -1; entry_stack = entry_stack_n++; }
            } else {
                entry_slot = ugpr++ + (has_sret ? 1 : 0);
                if (entry_slot >= 6) { entry_slot = -1; entry_stack = entry_stack_n++; }
            }
            if (isfp)      u[n].fin_slot = xmm_run < 8 ? xmm_run++ : -1;
            else           u[n].fin_slot = gpr_run < 6 ? gpr_run++ : -1;
            u[n].fin_stack = u[n].fin_slot < 0 ? decl_stack_run++ : -1;
            if (isfp) {
                if (entry_slot >= 0) { u[n].src = 2; u[n].src_off = -64 - 16 * entry_slot; }
                else                 { u[n].src = 3; u[n].src_off = -192 - 8 * entry_stack; }
            } else {
                if (entry_slot >= 0) { u[n].src = 1; u[n].src_off = -8 * (entry_slot + 1); }
                else                 { u[n].src = 3; u[n].src_off = -192 - 8 * entry_stack; }
            }
            n++;
        }
    }
    int fin_stack_n = decl_stack_run;

    int fin_stack_n_pre = decl_stack_run; /* for the pre-push alignment pad */
    int frame = 192 + 8 * entry_stack_n;
    if (fin_stack_n_pre & 1) frame += 8;  /* pad BEFORE the pushes (CALL model) */
    if (frame & 15) frame += 16 - (frame & 15);

    /* ---- prologue ---- */
    me_emit_u8(c, 0x55);                 /* push rbp */
    me_emit_u8(c, 0x48); me_emit_u8(c, 0x89); me_emit_u8(c, 0xE5); /* mov rbp, rsp */
    if (frame >= 128) {
        me_emit_u8(c, 0x48); me_emit_u8(c, 0x81); me_emit_u8(c, 0xEC);
        me_emit_u32(c, (uint32_t)frame);
    } else {
        me_emit_u8(c, 0x48); me_emit_u8(c, 0x83); me_emit_u8(c, 0xEC);
        me_emit_u8(c, (uint8_t)frame);
    }

    /* save entry GPR arg regs -> [rbp-8..-48] */
    static const int gpr_arg_reg[6] = {7, 6, 2, 1, 8, 9};
    for (int s = 0; s < 6; s++) x86_rm(c, 0x89, gpr_arg_reg[s], X86_RBP, -8 * (s + 1));
    /* save entry XMM arg regs -> [rbp-64-16x]. 0F 29 /r is the STORE form
     * (movaps m128, xmm); 0F 28 /r would LOAD (frame garbage -> xmm0-7),
     * which is what the Sprint-93 code accidentally emitted. x>=5 needs
     * disp32: -64-16*5 = -144 overflows disp8. */
    for (int x = 0; x < 8; x++) {
        int32_t off = -64 - 16 * x;
        x86_rex(c, 1, x >> 3, 0, 0);
        me_emit_u8(c, 0x0F);
        me_emit_u8(c, 0x29);
        if (off >= -128 && off <= 127) {
            x86_modrm(c, 1, x & 7, X86_RBP);
            me_emit_u8(c, (uint8_t)(int8_t)off);
        } else {
            x86_modrm(c, 2, x & 7, X86_RBP);
            me_emit_u32(c, (uint32_t)off);
        }
    }
    /* save entry stack units: [rbp+16+8m] -> [rbp-192-8m] (via r11).
     * rbp = entry_rsp - 8 (push rbp ran first), so the return address is
     * at [rbp+8] and the caller's first stack arg - zerocc's CALL
     * lowering puts it at [rsp_call+8] - at [rbp+16]. */
    for (int m = 0; m < entry_stack_n; m++) {
        x86_rm(c, 0x8B, X86_R11, X86_RBP, 16 + 8 * m);
        x86_rm(c, 0x89, X86_R11, X86_RBP, -192 - 8 * m);
    }

    /* ctx base: movabs r10, 0 + 8-byte absolute data reloc.
     * r10 = BA (B8+2) with the register extended by REX.B (not REX.R). */
    x86_rex(c, 1, 0, 0, 1);
    me_emit_u8(c, 0xBA);                 /* movabs r10, imm64 */
    size_t ctx_patch = me_get_offset(c);
    me_emit_u64(c, 0);
    if (c->reloc_count < ZERO_CC_MAX_RELOCS) {
        ZeroCCReloc *r = &c->relocs[c->reloc_count++];
        r->text_patch_offset = ctx_patch;
        snprintf(r->target_symbol, sizeof(r->target_symbol), "%s", nf->tramp_ctx_name);
        r->is_call = false;
    } else {
        zero_cc_reloc_overflow_note(c);
    }

    /* ---- place final REGISTER units ---- */
    /* ABI GPR slot -> x86-64 register (0=rdi,1=rsi,2=rdx,3=rcx,4=r8,5=r9). */
    static const int gpr_slot_to_reg[6] = {7, 6, 2, 1, 8, 9};
    for (int i = 0; i < n; i++) {
        ZeroCCTrampUnit *a = &u[i];
        if (a->fin_slot < 0) continue;
        if (a->is_fp) {
            /* wide16: 16B-aligned 16B source (ctx word 0 / xmm save);
             * otherwise an 8B source (movdqu). */
            bool wide16 = (a->src == 2) || (a->src == 0 && a->src_off % 16 == 0);
            tramp_mov_xmm_mem(c, a->fin_slot, a->src == 0 ? 10 : X86_RBP,
                              a->src_off, wide16);
        } else {
            x86_rm(c, 0x8B, gpr_slot_to_reg[a->fin_slot], a->src == 0 ? 10 : X86_RBP, a->src_off);
        }
    }

    /* ---- final STACK units: push in DESCENDING index order so unit 0
     * ends up at the lowest address (= [rsp] just before the call, which
     * is [rsp_call+8] after the call - the callee's first stack arg) ---- */
    for (int idx = fin_stack_n - 1; idx >= 0; idx--) {
        for (int i = 0; i < n; i++) {
            if (u[i].fin_stack != idx) continue;
            if (u[i].src == 0) tramp_push_mem(c, 10, u[i].src_off);
            else               tramp_push_mem(c, X86_RBP, u[i].src_off);
        }
    }
    /* 16-byte stack alignment at the call: handled by the pre-push pad
     * in the prologue (same model as zerocc's OP_CALL lowering: pad 8
     * when fin_stack_n is ODD, BEFORE the pushes - a post-push pad would
     * shift the stack units away from [entry_sp]). */

    /* call the real nested function (same image: exact rel32, no reloc) */
    me_emit_u8(c, 0xE8);
    size_t call_pos = me_get_offset(c);
    int32_t rel = (int32_t)(nf->text_offset - (call_pos + 4));
    me_emit_u32(c, (uint32_t)rel);

    /* the target's `ret` lands here: return to the original caller */
    me_emit_u8(c, 0xC9);                 /* leave */
    me_emit_u8(c, 0xC3);                 /* ret */
}
/* ==========================================================================
 * GNU __int128 (Sprint 109): 128-bit integer ops on a (lo,hi) GPR pair.
 * Register numbers are the standard x86-64 encoding (0=rax, 1=rcx, 2=rdx,
 * 3=rbx, 6=rsi, 7=rdi, 8-15=r8-r15). 128-bit temps never live in rax/rdx
 * (the GPR128 pool excludes them), so OP_MUL128 may use rax/rdx as scratch.
 * ========================================================================= */
static void x86_emit_add128(ZeroCCCompiler *c, int dlo, int dhi, int a_lo, int a_hi, int b_lo, int b_hi) {
    x86_mov_rr(c, dlo, a_lo);
    x86_alu(c, 0x01, dlo, b_lo);   /* add dlo, b_lo  (CF) */
    x86_mov_rr(c, dhi, a_hi);
    x86_alu(c, 0x11, dhi, b_hi);   /* adc dhi, b_hi */
}
static void x86_emit_sub128(ZeroCCCompiler *c, int dlo, int dhi, int a_lo, int a_hi, int b_lo, int b_hi) {
    x86_mov_rr(c, dlo, a_lo);
    x86_alu(c, 0x29, dlo, b_lo);   /* sub dlo, b_lo  (CF = borrow) */
    x86_mov_rr(c, dhi, a_hi);
    x86_alu(c, 0x19, dhi, b_hi);   /* sbb dhi, b_hi */
}
/* 128x128 -> low 128 bits. result.lo = (a0*b0) mod 2^64; result.hi =
 * (r0.hi + (a0*b1) mod 2^64 + (a1*b0) mod 2^64) mod 2^64, where
 * r0 = a0*b0. Uses rax/rdx (reserved for OP_MUL128) as scratch. */
static void x86_emit_mul128(ZeroCCCompiler *c, int dlo, int dhi, int a_lo, int a_hi, int b_lo, int b_hi) {
    int rax = 0, rdx = 2;
    x86_mov_rr(c, rax, a_lo);
    x86_rex(c, 1, 0, 0, (b_lo >> 3) & 1); me_emit_u8(c, 0xF7); x86_modrm(c, 3, 4, b_lo); /* mul b_lo -> rdx:rax */
    x86_mov_rr(c, dlo, rax);   /* dlo = (a_lo*b_lo) low  */
    x86_mov_rr(c, dhi, rdx);   /* dhi = (a_lo*b_lo) high */
    x86_mov_rr(c, rax, a_lo);
    x86_rex(c, 1, 0, 0, (b_hi >> 3) & 1); me_emit_u8(c, 0xF7); x86_modrm(c, 3, 4, b_hi); /* mul b_hi -> rdx:rax */
    x86_alu(c, 0x01, dhi, rax); /* dhi += (a_lo*b_hi) low */
    x86_mov_rr(c, rax, a_hi);
    x86_rex(c, 1, 0, 0, (b_lo >> 3) & 1); me_emit_u8(c, 0xF7); x86_modrm(c, 3, 4, b_lo); /* mul b_lo -> rdx:rax */
    x86_alu(c, 0x01, dhi, rax); /* dhi += (a_hi*b_lo) low */
}
static void x86_emit_and128(ZeroCCCompiler *c, int dlo, int dhi, int a_lo, int a_hi, int b_lo, int b_hi) {
    x86_mov_rr(c, dlo, a_lo); x86_alu(c, 0x21, dlo, b_lo);
    x86_mov_rr(c, dhi, a_hi); x86_alu(c, 0x21, dhi, b_hi);
}
static void x86_emit_or128(ZeroCCCompiler *c, int dlo, int dhi, int a_lo, int a_hi, int b_lo, int b_hi) {
    x86_mov_rr(c, dlo, a_lo); x86_alu(c, 0x09, dlo, b_lo);
    x86_mov_rr(c, dhi, a_hi); x86_alu(c, 0x09, dhi, b_hi);
}
static void x86_emit_xor128(ZeroCCCompiler *c, int dlo, int dhi, int a_lo, int a_hi, int b_lo, int b_hi) {
    x86_mov_rr(c, dlo, a_lo); x86_alu(c, 0x31, dlo, b_lo);
    x86_mov_rr(c, dhi, a_hi); x86_alu(c, 0x31, dhi, b_hi);
}
static void x86_emit_not128(ZeroCCCompiler *c, int dlo, int dhi, int s_lo, int s_hi) {
    x86_emit_not(c, dlo, s_lo);
    x86_emit_not(c, dhi, s_hi);
}
static void x86_emit_neg128(ZeroCCCompiler *c, int dlo, int dhi, int s_lo, int s_hi) {
    /* -x = ~x + 1. low = ~s_lo+1 (CF = carry-out = (s_lo==0)); high = ~s_hi + CF.
     * NOT (F7 /2) preserves CF; ADD r64,imm8 (0x83 /0) sets CF; ADC r/m64,imm8 is 0x83 /2. */
    x86_emit_not(c, dlo, s_lo);      /* dlo = ~s_lo (CF preserved) */
    x86_emit_add_imm(c, dlo, 1);     /* dlo = ~s_lo+1 = low; CF = (s_lo==0) */
    x86_emit_not(c, dhi, s_hi);      /* dhi = ~s_hi (CF preserved) */
    x86_rex(c, 1, 0, 0, (dhi >> 3) & 1); me_emit_u8(c, 0x83); x86_modrm(c, 3, 2, dhi); me_emit_u8(c, 0x00); /* adc dhi, 0 (0x83 /2): high = ~s_hi + CF */
}
static void x86_emit_mov128(ZeroCCCompiler *c, int dlo, int dhi, int s_lo, int s_hi) {
    x86_mov_rr(c, dlo, s_lo);
    x86_mov_rr(c, dhi, s_hi);
}
static void x86_emit_load128(ZeroCCCompiler *c, int dlo, int dhi, int base, int32_t offset) {
    /* Aliasing: when dlo == base (e.g. the base is a spilled pointer reloaded
     * into the same scratch register as the spilled 128-bit dst's lo half),
     * the lo load would clobber the base before the hi load. Load the hi half
     * first; the self-referential lo load (d == base) is safe - the base is
     * read before it is written. */
    if (dlo == base) {
        x86_rm(c, 0x8B, dhi, base, offset + 8);  /* mov dhi, [base+offset+8] */
        x86_rm(c, 0x8B, dlo, base, offset);      /* mov dlo, [base+offset]   */
    } else {
        x86_rm(c, 0x8B, dlo, base, offset);      /* mov dlo, [base+offset]   */
        x86_rm(c, 0x8B, dhi, base, offset + 8);  /* mov dhi, [base+offset+8] */
    }
}
static void x86_emit_store128(ZeroCCCompiler *c, int s_lo, int s_hi, int base, int32_t offset) {
    x86_rm(c, 0x89, s_lo, base, offset);      /* mov [base+offset], s_lo   */
    x86_rm(c, 0x89, s_hi, base, offset + 8);  /* mov [base+offset+8], s_hi */
}
static void x86_emit_zext64_128(ZeroCCCompiler *c, int dlo, int dhi, int src) {
    x86_mov_rr(c, dlo, src);
    x86_emit_mov_imm(c, dhi, 0);
}
static void x86_emit_sext64_128(ZeroCCCompiler *c, int dlo, int dhi, int src) {
    x86_mov_rr(c, dlo, src);
    /* Sign-extend the 64-bit source to the 128-bit high half: copy src then
     * `sar dhi, 63` -> 0 (src >= 0) or -1 (src < 0).  (movsxd alone is wrong:
     * it only sign-extends the low 32 bits, so a positive 64-bit value such as
     * 0x1234 would wrongly yield hi = 0x1234 instead of 0. */
    x86_mov_rr(c, dhi, src);
    x86_rex(c, 1, 0, 0, (dhi >> 3) & 1);
    me_emit_u8(c, 0xC1);
    x86_modrm(c, 3, 7, dhi & 7);  /* SAR r64, imm8 (reg field 7 = arithmetic; 5 is logical SHR) */
    me_emit_u8(c, 63);
}
static void x86_emit_trunc128_64(ZeroCCCompiler *c, int dst, int s_lo) {
    x86_mov_rr(c, dst, s_lo);
}
static void x86_emit_mov128_imm(ZeroCCCompiler *c, int dlo, int dhi, int64_t imm) {
    x86_emit_mov_imm(c, dlo, imm);
    x86_emit_mov_imm(c, dhi, 0);
}

#include "zero_cc/isa/x86_64_inline_asm.c"  /* Phase 2: integrated inline assembler */

ZeroCCISABackend x86_64_backend = {  /* writable: ASM emit pointers are filled in lazily */
    .isa = ISA_X86_64,
    .regs = &x86_64_regs,
    .conv = &x86_64_conv,
    .fp_reg = X86_RBP,  /* hardware encoding 5 */
    .sp_reg = X86_RSP,  /* hardware encoding 4 */
    .scratch_reg1 = 10,                        /* r10 (non-argument/return register) */
    .scratch_reg2 = 11,                        /* r11 (non-argument/return register) */
    .fp_scratch_reg1 = REGALLOC_FP_SCRATCH1,  /* xmm15 - outside XMM pool 0-7 */
    .fp_scratch_reg2 = REGALLOC_FP_SCRATCH2,   /* xmm14 - outside XMM pool 0-7 */
    /* 128-bit spill scratch (reserved; excluded from the 64-bit and GPR128 pools):
     * pair0 = (r10, r11). Only ONE pair is safe on x86-64 -- the only GPRs outside both
     * the 64-bit and GPR128 value pools are r10/r11; freeing more (e.g. r8/r9) from the
     * 64-bit pool regresses TLS register allocation at -O1+ (test/jit/tls.c). */
    .g128_scratch_lo = {10, 8},
    .g128_scratch_hi = {11, 9},
    .g128_scratch_pair_count = 1,
    .callee_save_frame_reserve = 48,           /* 5 callee-saved regs: rbx, r12-r15 */

    .emit_prologue = x86_emit_prologue,
    .emit_leave = x86_emit_leave,
    .emit_epilogue = x86_emit_epilogue,
    .get_code_offset = x86_get_code_offset,
    .emit_inline_asm_mc = x86_emit_inline_asm_mc,

    .emit_prologue_asm = x86_emit_prologue_asm,
    .emit_epilogue_asm = x86_emit_epilogue_asm,
    .get_hw_reg_name = x86_get_hw_reg_name,
    .get_vec_reg_name = x86_get_vec_reg_name,

    .emit_mov_imm = x86_emit_mov_imm,
    .emit_add = x86_emit_add,
    .emit_sub = x86_emit_sub,
    .emit_mul = x86_emit_mul,
    .emit_div = x86_emit_div,
    .emit_mod = x86_emit_mod,
    .emit_and = x86_emit_and,
    .emit_or = x86_emit_or,
    .emit_xor = x86_emit_xor,
    .emit_shl = x86_emit_shl,
    .emit_cmp = x86_emit_cmp,
    .emit_cond_set = x86_emit_cond_set,
    .emit_neg = x86_emit_neg,
    .emit_extend = x86_emit_extend,
    .emit_test = x86_emit_test,
    .emit_cqo = x86_emit_cqo,
    .emit_idiv = x86_emit_idiv,
    .emit_lea = x86_emit_lea,

    /* GNU __int128 */
    .emit_add128 = x86_emit_add128,
    .emit_sub128 = x86_emit_sub128,
    .emit_mul128 = x86_emit_mul128,
    .emit_mul128_spilled = x86_emit_mul128_spilled,
    .emit_mul128_mixed = x86_emit_mul128_mixed,
    .emit_bin128_sp = x86_emit_bin128_sp,
    .emit_and128 = x86_emit_and128,
    .emit_or128 = x86_emit_or128,
    .emit_xor128 = x86_emit_xor128,
    .emit_not128 = x86_emit_not128,
    .emit_neg128 = x86_emit_neg128,
    .emit_mov128 = x86_emit_mov128,
    .emit_load128 = x86_emit_load128,
    .emit_store128 = x86_emit_store128,
    .emit_zext64_128 = x86_emit_zext64_128,
    .emit_sext64_128 = x86_emit_sext64_128,
    .emit_trunc128_64 = x86_emit_trunc128_64,
    .emit_mov128_imm = x86_emit_mov128_imm,

    /* Vector operations for JIT binary emission */
    .emit_vload = x86_emit_vload,
    .emit_vstore = x86_emit_vstore,
    .emit_vloadss = x86_emit_vloadss,
    .emit_vloadsd = x86_emit_vloadsd,
    .emit_vstoress = x86_emit_vstoress,
    .emit_vstoredsd = x86_emit_vstoredsd,
    .emit_vaddps = x86_emit_vaddps,
    .emit_vsubps = x86_emit_vsubps,
    .emit_vmulps = x86_emit_vmulps,
    .emit_vdivps = x86_emit_vdivps,
    .emit_vaddpd = x86_emit_vaddpd,
    .emit_vsubpd = x86_emit_vsubpd,
    .emit_vmulpd = x86_emit_vmulpd,
    .emit_vdivpd = x86_emit_vdivpd,
    .emit_vpaddd = x86_emit_vpaddd,
    .emit_vpsubd = x86_emit_vpsubd,
    .emit_vpmulld = x86_emit_vpmulld,
    .emit_vmovaps = x86_emit_vmovaps,
    .emit_cvtss2sd = x86_emit_fp_cvtss2sd,
    .emit_cvtsi2ss = x86_emit_fp_cvtsi2ss,
    .emit_cvtsd2ss = x86_emit_fp_cvtsd2ss,
    .patch_jmp = x86_patch_jmp,
    .resolve_labels = x86_resolve_labels,

    /* VREG mapping */
    .get_int_reg = x86_get_int_reg,
    .get_fp_reg = x86_get_fp_reg,

    .emit_load = x86_emit_load,
    .emit_store = x86_emit_store,

    .emit_jmp = x86_emit_jmp,
    .emit_jeq = x86_emit_jeq,
    .emit_jne = x86_emit_jne,
    .emit_jlt = x86_emit_jlt,
    .emit_jle = x86_emit_jle,
    .emit_jgt = x86_emit_jgt,
    .emit_jge = x86_emit_jge,
    .emit_jcond = x86_emit_jcond,
    .emit_call = x86_emit_call,
    .emit_call_reg = x86_emit_call_reg,
    .emit_jmp_reg = x86_emit_jmp_reg,
    .emit_lea_label = x86_emit_lea_label,
    .emit_ret = x86_emit_ret,

    .emit_load_fp = x86_emit_load_fp,
    .emit_store_fp = x86_emit_store_fp,
    .emit_load_fp_rodata = x86_emit_load_fp_rodata,
    .emit_fp_add = x86_emit_fp_add,
    .emit_fp_sub = x86_emit_fp_sub,
    .emit_fp_mul = x86_emit_fp_mul,
    .emit_fp_div = x86_emit_fp_div,
    .emit_fp_neg = x86_emit_fp_neg,
    .emit_fp_cmp = x86_emit_fp_cmp,
    .emit_fp_cvtsi = x86_emit_fp_cvtsi,
    .emit_fp_cvtsf = x86_emit_fp_cvtsf,
    .emit_fp_mov = x86_emit_fp_mov,

    .emit_const = x86_emit_const,
    .emit_stack_adjust = x86_emit_stack_adjust,
    .emit_stack_align = x86_emit_stack_align,
    .emit_push_reg = x86_emit_push_reg,
    .emit_pop_reg = x86_emit_pop_reg,

    /* Additional operations for IR-based codegen */
    .emit_mov_reg = x86_mov_rr,
    .emit_add_imm = x86_emit_add_imm,
    .emit_sub_imm = x86_emit_sub_imm,
    .emit_inc = x86_emit_inc,
    .emit_dec = x86_emit_dec,
    .emit_sar = x86_emit_sar,
    .emit_shr = x86_emit_shr,
    .emit_setl = x86_emit_setl,
    .emit_setle = x86_emit_setle,
    .emit_setg = x86_emit_setg,
    .emit_setge = x86_emit_setge,
    .emit_sete = x86_emit_sete,
    .emit_setne = x86_emit_setne,
    .emit_deref = x86_emit_deref,
    .emit_load_byte = x86_emit_load_byte,
    .emit_load_16 = x86_emit_load_16,
    .emit_load_32 = x86_emit_load_32,
    .emit_store_byte = x86_emit_store_byte,
    .emit_store_16 = x86_emit_store_16,
    .emit_store_32 = x86_emit_store_32,
    .emit_store_64 = x86_emit_store_64,
    .emit_vla_alloc = x86_emit_vla_alloc,
    .emit_atomic_xadd = x86_emit_atomic_xadd,
    .emit_atomic_or = x86_emit_atomic_or,
    .emit_atomic_and = x86_emit_atomic_and,
    .emit_atomic_xor = x86_emit_atomic_xor,
    .emit_atomic_cmpxchg = x86_emit_atomic_cmpxchg,
    .emit_atomic_cmpxchg_bool = x86_emit_atomic_cmpxchg_bool,
    .emit_atomic_xchg = x86_emit_atomic_xchg,
    .emit_mfence = x86_emit_mfence,
    .emit_trap = x86_emit_trap,
    .emit_trap_asm = x86_emit_trap_asm,
    .emit_syscall = x86_emit_syscall,
    .emit_cmp_zero = x86_emit_cmp_zero,
    .emit_not = x86_emit_not,
    .emit_lnot = x86_emit_lnot,
    .emit_label = x86_emit_label,

    .emit_param_save_int = x86_emit_param_save_int,
    .emit_param_save_fp = x86_emit_param_save_fp,
    .emit_vloadss_raw = x86_emit_vloadss_raw,
    .emit_vstoress_raw = x86_emit_vstoress_raw,
    .emit_cvttsd2ss = x86_emit_cvttsd2ss,
    .emit_variadic_spill = x86_emit_variadic_spill,
    .emit_variadic_al_setup = x86_emit_variadic_al_setup,
    .emit_lea_rip = x86_emit_lea_rip,

    /* Assembly emission for IR instructions (Option A) */
    .emit_mov_imm_asm = x86_emit_mov_imm_asm,
    .emit_mov_reg_asm = x86_emit_mov_reg_asm,
    .emit_load_asm = x86_emit_load_asm,
    .emit_store_asm = x86_emit_store_asm,
    .emit_add_asm = x86_emit_add_asm,
    .emit_sub_asm = x86_emit_sub_asm,
    .emit_mul_asm = x86_emit_mul_asm,
    .emit_div_asm = x86_emit_div_asm,
    .emit_mod_asm = x86_emit_mod_asm,
    .emit_and_asm = x86_emit_and_asm,
    .emit_or_asm = x86_emit_or_asm,
    .emit_xor_asm = x86_emit_xor_asm,
    .emit_shl_asm = x86_emit_shl_asm,
    .emit_shr_asm = x86_emit_shr_asm,
    .emit_sar_asm = x86_emit_sar_asm,
    .emit_extend_asm = x86_emit_extend_asm,
    .emit_cmp_asm = x86_emit_cmp_asm,
    .emit_cond_set_asm = x86_emit_cond_set_asm,
    .emit_neg_asm = x86_emit_neg_asm,
    .emit_lea_asm = x86_emit_lea_asm,
    .emit_deref_asm = x86_emit_deref_asm,
    .emit_call_asm = x86_emit_call_asm,
    .emit_ret_asm = x86_emit_ret_asm,
    .emit_label_asm = x86_emit_label_asm,
    .emit_jmp_asm = x86_emit_jmp_asm,
    .emit_jeq_asm = x86_emit_jeq_asm,
    .emit_jne_asm = x86_emit_jne_asm,
    .emit_jlt_asm = x86_emit_jlt_asm,
    .emit_jle_asm = x86_emit_jle_asm,
    .emit_jgt_asm = x86_emit_jgt_asm,
    .emit_jge_asm = x86_emit_jge_asm,
    .emit_load_fp_asm = x86_emit_load_fp_asm,
    .emit_store_fp_asm = x86_emit_store_fp_asm,
    .emit_fp_add_asm = x86_emit_fp_add_asm,
    .emit_fp_sub_asm = x86_emit_fp_sub_asm,
    .emit_fp_mul_asm = x86_emit_fp_mul_asm,
    .emit_fp_div_asm = x86_emit_fp_div_asm,
    .emit_fp_mov_asm = x86_emit_fp_mov_asm,
    .emit_vla_alloc_asm = x86_emit_vla_alloc_asm,

    /* Vector operations for ASM emission */
    .emit_vload_asm = x86_emit_vload_asm,
    .emit_vstore_asm = x86_emit_vstore_asm,
    .emit_vaddss_asm = x86_emit_vaddss_asm,
    .emit_vsubss_asm = x86_emit_vsubss_asm,
    .emit_vmulss_asm = x86_emit_vmulss_asm,
    .emit_vdivss_asm = x86_emit_vdivss_asm,
    .emit_vaddps_asm = x86_emit_vaddps_asm,
    .emit_vsubps_asm = x86_emit_vsubps_asm,
    .emit_vmulps_asm = x86_emit_vmulps_asm,
    .emit_vdivps_asm = x86_emit_vdivps_asm,
    .emit_vaddsd_asm = x86_emit_vaddsd_asm,
    .emit_vsubsd_asm = x86_emit_vsubsd_asm,
    .emit_vmulsd_asm = x86_emit_vmulsd_asm,
    .emit_vdivsd_asm = x86_emit_vdivsd_asm,
    .emit_vaddpd_asm = x86_emit_vaddpd_asm,
    .emit_vsubpd_asm = x86_emit_vsubpd_asm,
    .emit_vmulpd_asm = x86_emit_vmulpd_asm,
    .emit_vdivpd_asm = x86_emit_vdivpd_asm,
    .emit_vpaddd_asm = x86_emit_vpaddd_asm,
    .emit_vpsubd_asm = x86_emit_vpsubd_asm,
    .emit_vpmulld_asm = x86_emit_vpmulld_asm,
    .emit_vmovss_asm = x86_emit_vmovss_asm,
    .emit_vmovsd_asm = x86_emit_vmovsd_asm,
    .emit_vmovaps_asm = x86_emit_vmovaps_asm,
    .emit_vmovapd_asm = x86_emit_vmovapd_asm,
    .emit_vandps_asm = x86_emit_vandps_asm,
    .emit_vorps_asm = x86_emit_vorps_asm,
    .emit_vxorps_asm = x86_emit_vxorps_asm,
    .emit_vshufps_asm = x86_emit_vshufps_asm,
    .emit_vcvt_si2ss_asm = x86_emit_vcvt_si2ss_asm,
    .emit_vcvt_ss2si_asm = x86_emit_vcvt_ss2si_asm,
    .emit_vcmpss_asm = x86_emit_vcmpss_asm,
    .emit_vcmpsd_asm = x86_emit_vcmpsd_asm,
    };

/* NOTE (Sprint 114): the global ISA backend registry (zero_cc_get_backend,
 * zero_cc_get_native_backend, zero_cc_init_backends_once) now lives in
 * backend_registry.c so no single ISA emitter owns the lookup table. */
/* =========================================================================
 * Virtual Register (VREG) Mapping for x86-64
 * =========================================================================
 *
 * The lowering code uses virtual registers defined in ../types/vregs.h.
 * Each VREG number is interpreted as the x86-64 hardware encoding:
 *
 *   VREG_0  = 0 = rax   (result / return value)
 *   VREG_1  = 1 = rcx   (temp / 4th arg)
 *   VREG_2  = 2 = rdx   (temp / 3rd arg)
 *   VREG_3  = 3 = rbx   (temp / callee-saved)
 *   VREG_4  = 4 = rsp   (stack pointer - use BE(c)->sp_reg instead)
 *   VREG_5  = 5 = rbp   (frame pointer - use BE(c)->fp_reg instead)
 *   VREG_6  = 6 = rsi   (temp / 2nd arg)
 *   VREG_7  = 7 = rdi   (temp / 1st arg)
 *   VREG_8  = 8 = r8    (temp / 5th arg)
 *   VREG_9  = 9 = r9    (temp / 6th arg)
 *   VREG_10 = 10 = r10  (temp)
 *   VREG_11 = 11 = r11  (temp)
 *   VREG_12 = 12 = r12  (temp / callee-saved)
 *   VREG_13 = 13 = r13  (temp / callee-saved)
 *   VREG_14 = 14 = r14  (temp / callee-saved)
 *   VREG_15 = 15 = r15  (temp / callee-saved)
 *
 * Float registers (VREG_F0, VREG_F1, ...):
 *   VREG_F0 = 0 = xmm0  (float result / return value)
 *   VREG_F1 = 1 = xmm1  (float temp / 2nd float arg)
 *   VREG_F2 = 2 = xmm2  (float temp / 3rd float arg)
 *   ...
 *
 * Special registers:
 *   BE(c)->sp_reg = 4 (rsp) - use this instead of VREG_4
 *   BE(c)->fp_reg = 5 (rbp) - use this instead of VREG_5
 *
 * Note: The lowering code should use VREG_0-VREG_15 for general purposes,
 * and BE(c)->sp_reg / BE(c)->fp_reg for stack/frame pointers.
 */
