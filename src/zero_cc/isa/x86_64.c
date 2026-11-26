#include <string.h>
#include "zero_cc/isa/isa_backend.h"
#include "zero_cc/ir/machine_emitter.h"
#include "zero_cc/isa/x86_64_common.h"
#include "zero_cc/isa/x86_64_asm.c"

/* x86-64 register info */
static ISARegInfo x86_64_regs = {
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

void x86_emit_mov_imm(ZeroCCCompiler *c, int dst_reg, int64_t imm) {
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

static void x86_emit_vpaddb(ZeroCCCompiler *c, int dst, int src1, int src2) {
    /* paddb xmm_dest, xmm_src - packed 8-bit int add (66 0F FC /r) */
    if (dst == src2 && dst != src1) {
        int tmp = src1; src1 = src2; src2 = tmp;
    }
    if (dst != src1) {
        x86_emit_vmovaps(c, dst, src1);
    }
    me_emit_u8(c, 0x66);
    x86_rex(c, 0, (dst >> 3) & 1, 0, (src2 >> 3) & 1);
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0xFC);  /* paddb */
    x86_modrm(c, 3, dst & 7, src2 & 7);
}

static void x86_emit_vpsubb(ZeroCCCompiler *c, int dst, int src1, int src2) {
    /* psubb xmm_dest, xmm_src - packed 8-bit int subtract (66 0F F8 /r) */
    if (dst == src2 && dst != src1) {
        x86_emit_vmovaps(c, 15, src2);
        x86_emit_vmovaps(c, dst, src1);
        src2 = 15;
    } else if (dst != src1) {
        x86_emit_vmovaps(c, dst, src1);
    }
    me_emit_u8(c, 0x66);
    x86_rex(c, 0, (dst >> 3) & 1, 0, (src2 >> 3) & 1);
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0xF8);  /* psubb */
    x86_modrm(c, 3, dst & 7, src2 & 7);
}

static void x86_emit_vpaddw(ZeroCCCompiler *c, int dst, int src1, int src2) {
    /* paddw xmm_dest, xmm_src - packed 16-bit int add (66 0F FD /r) */
    if (dst == src2 && dst != src1) {
        int tmp = src1; src1 = src2; src2 = tmp;
    }
    if (dst != src1) {
        x86_emit_vmovaps(c, dst, src1);
    }
    me_emit_u8(c, 0x66);
    x86_rex(c, 0, (dst >> 3) & 1, 0, (src2 >> 3) & 1);
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0xFD);  /* paddw */
    x86_modrm(c, 3, dst & 7, src2 & 7);
}

static void x86_emit_vpsubw(ZeroCCCompiler *c, int dst, int src1, int src2) {
    /* psubw xmm_dest, xmm_src - packed 16-bit int subtract (66 0F F9 /r) */
    if (dst == src2 && dst != src1) {
        x86_emit_vmovaps(c, 15, src2);
        x86_emit_vmovaps(c, dst, src1);
        src2 = 15;
    } else if (dst != src1) {
        x86_emit_vmovaps(c, dst, src1);
    }
    me_emit_u8(c, 0x66);
    x86_rex(c, 0, (dst >> 3) & 1, 0, (src2 >> 3) & 1);
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0xF9);  /* psubw */
    x86_modrm(c, 3, dst & 7, src2 & 7);
}

static void x86_emit_vpmullw(ZeroCCCompiler *c, int dst, int src1, int src2) {
    /* pmullw xmm_dest, xmm_src - packed 16-bit int multiply (66 0F D5 /r) */
    if (dst == src2 && dst != src1) {
        int tmp = src1; src1 = src2; src2 = tmp;
    }
    if (dst != src1) {
        x86_emit_vmovaps(c, dst, src1);
    }
    me_emit_u8(c, 0x66);
    x86_rex(c, 0, (dst >> 3) & 1, 0, (src2 >> 3) & 1);
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0xD5);  /* pmullw */
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

static void x86_emit_vandps(ZeroCCCompiler *c, int dst, int src1, int src2) {
    /* andps xmm_dest, xmm_src - bitwise AND (0F 54 /r) */
    if (dst == src2 && dst != src1) {
        int tmp = src1; src1 = src2; src2 = tmp;
    }
    if (dst != src1) {
        x86_emit_vmovaps(c, dst, src1);
    }
    x86_rex(c, 0, (dst >> 3) & 1, 0, (src2 >> 3) & 1);
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x54);  /* andps */
    x86_modrm(c, 3, dst & 7, src2 & 7);
}

static void x86_emit_vorps(ZeroCCCompiler *c, int dst, int src1, int src2) {
    /* orps xmm_dest, xmm_src - bitwise OR (0F 56 /r) */
    if (dst == src2 && dst != src1) {
        int tmp = src1; src1 = src2; src2 = tmp;
    }
    if (dst != src1) {
        x86_emit_vmovaps(c, dst, src1);
    }
    x86_rex(c, 0, (dst >> 3) & 1, 0, (src2 >> 3) & 1);
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x56);  /* orps */
    x86_modrm(c, 3, dst & 7, src2 & 7);
}

static void x86_emit_vxorps(ZeroCCCompiler *c, int dst, int src1, int src2) {
    /* xorps xmm_dest, xmm_src - bitwise XOR (0F 57 /r) */
    if (dst == src2 && dst != src1) {
        int tmp = src1; src1 = src2; src2 = tmp;
    }
    if (dst != src1) {
        x86_emit_vmovaps(c, dst, src1);
    }
    x86_rex(c, 0, (dst >> 3) & 1, 0, (src2 >> 3) & 1);
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x57);  /* xorps */
    x86_modrm(c, 3, dst & 7, src2 & 7);
}

/* 256-bit AVX Vector Emitters (VEX prefix encoded) */
static void x86_emit_v256_load(ZeroCCCompiler *c, int dst, int base, int32_t offset) {
    c->used_avx = true;
    x86_vex(c, 1, 0, 0, dst, base, 0, 1);
    me_emit_u8(c, 0x10);  /* vmovups ymm_dst, [base + offset] */
    int mod;
    if (offset == 0 && (base & 7) != X86_RBP) mod = 0;
    else if (offset >= -128 && offset <= 127) mod = 1;
    else mod = 2;
    x86_modrm(c, mod, dst & 7, base & 7);
    if ((base & 7) == X86_RSP) me_emit_u8(c, 0x24);
    if (mod == 1) me_emit_u8(c, (uint8_t)(int8_t)offset);
    else if (mod == 2) me_emit_u32(c, (uint32_t)offset);
}

static void x86_emit_v256_store(ZeroCCCompiler *c, int src, int base, int32_t offset) {
    c->used_avx = true;
    x86_vex(c, 1, 0, 0, src, base, 0, 1);
    me_emit_u8(c, 0x11);  /* vmovups [base + offset], ymm_src */
    int mod;
    if (offset == 0 && (base & 7) != X86_RBP) mod = 0;
    else if (offset >= -128 && offset <= 127) mod = 1;
    else mod = 2;
    x86_modrm(c, mod, src & 7, base & 7);
    if ((base & 7) == X86_RSP) me_emit_u8(c, 0x24);
    if (mod == 1) me_emit_u8(c, (uint8_t)(int8_t)offset);
    else if (mod == 2) me_emit_u32(c, (uint32_t)offset);
}

static void x86_emit_v256_addps(ZeroCCCompiler *c, int dst, int src1, int src2) {
    c->used_avx = true;
    x86_vex(c, 1, 0, 0, dst, src2, src1, 1);
    me_emit_u8(c, 0x58);
    x86_modrm(c, 3, dst & 7, src2 & 7);
}

static void x86_emit_v256_subps(ZeroCCCompiler *c, int dst, int src1, int src2) {
    c->used_avx = true;
    x86_vex(c, 1, 0, 0, dst, src2, src1, 1);
    me_emit_u8(c, 0x5C);
    x86_modrm(c, 3, dst & 7, src2 & 7);
}

static void x86_emit_v256_mulps(ZeroCCCompiler *c, int dst, int src1, int src2) {
    c->used_avx = true;
    x86_vex(c, 1, 0, 0, dst, src2, src1, 1);
    me_emit_u8(c, 0x59);
    x86_modrm(c, 3, dst & 7, src2 & 7);
}

static void x86_emit_v256_divps(ZeroCCCompiler *c, int dst, int src1, int src2) {
    c->used_avx = true;
    x86_vex(c, 1, 0, 0, dst, src2, src1, 1);
    me_emit_u8(c, 0x5E);
    x86_modrm(c, 3, dst & 7, src2 & 7);
}

static void x86_emit_v256_addpd(ZeroCCCompiler *c, int dst, int src1, int src2) {
    c->used_avx = true;
    x86_vex(c, 1, 1, 0, dst, src2, src1, 1);
    me_emit_u8(c, 0x58);
    x86_modrm(c, 3, dst & 7, src2 & 7);
}

static void x86_emit_v256_subpd(ZeroCCCompiler *c, int dst, int src1, int src2) {
    c->used_avx = true;
    x86_vex(c, 1, 1, 0, dst, src2, src1, 1);
    me_emit_u8(c, 0x5C);
    x86_modrm(c, 3, dst & 7, src2 & 7);
}

static void x86_emit_v256_mulpd(ZeroCCCompiler *c, int dst, int src1, int src2) {
    c->used_avx = true;
    x86_vex(c, 1, 1, 0, dst, src2, src1, 1);
    me_emit_u8(c, 0x59);
    x86_modrm(c, 3, dst & 7, src2 & 7);
}

static void x86_emit_v256_divpd(ZeroCCCompiler *c, int dst, int src1, int src2) {
    c->used_avx = true;
    x86_vex(c, 1, 1, 0, dst, src2, src1, 1);
    me_emit_u8(c, 0x5E);
    x86_modrm(c, 3, dst & 7, src2 & 7);
}

static void x86_emit_v256_andps(ZeroCCCompiler *c, int dst, int src1, int src2) {
    c->used_avx = true;
    x86_vex(c, 1, 0, 0, dst, src2, src1, 1);
    me_emit_u8(c, 0x54);
    x86_modrm(c, 3, dst & 7, src2 & 7);
}

static void x86_emit_v256_orps(ZeroCCCompiler *c, int dst, int src1, int src2) {
    c->used_avx = true;
    x86_vex(c, 1, 0, 0, dst, src2, src1, 1);
    me_emit_u8(c, 0x56);
    x86_modrm(c, 3, dst & 7, src2 & 7);
}

static void x86_emit_v256_vxorps(ZeroCCCompiler *c, int dst, int src1, int src2) {
    c->used_avx = true;
    x86_vex(c, 1, 0, 0, dst, src2, src1, 1);
    me_emit_u8(c, 0x57);
    x86_modrm(c, 3, dst & 7, src2 & 7);
}

static void x86_emit_v256_zeroupper(ZeroCCCompiler *c) {
    (void)c;
    me_emit_u8(c, 0xC5);
    me_emit_u8(c, 0xF8);
    me_emit_u8(c, 0x77);
}

/* =========================================================================
 * AVX-512 Raw Binary Emitters (512-bit ZMM, EVEX prefix)
 * ========================================================================= */
static void x86_emit_v512_load(ZeroCCCompiler *c, int dst, int base, int32_t offset) {
    c->used_avx512 = true;
    c->used_avx = true;
    x86_evex(c, 1, 0, 0, dst, base, 0, 2, 0, 0);
    me_emit_u8(c, 0x10);  /* vmovups zmm_dst, [base + offset] */
    int mod;
    if (offset == 0 && (base & 7) != X86_RBP) {
        mod = 0;
    } else if ((offset % 64 == 0) && (offset / 64 >= -128) && (offset / 64 <= 127)) {
        mod = 1;
    } else {
        mod = 2;
    }
    x86_modrm(c, mod, dst & 7, base & 7);
    if ((base & 7) == X86_RSP) me_emit_u8(c, 0x24);
    if (mod == 1) me_emit_u8(c, (uint8_t)(int8_t)(offset / 64));
    else if (mod == 2) me_emit_u32(c, (uint32_t)offset);
}

static void x86_emit_v512_store(ZeroCCCompiler *c, int src, int base, int32_t offset) {
    c->used_avx512 = true;
    c->used_avx = true;
    x86_evex(c, 1, 0, 0, src, base, 0, 2, 0, 0);
    me_emit_u8(c, 0x11);  /* vmovups [base + offset], zmm_src */
    int mod;
    if (offset == 0 && (base & 7) != X86_RBP) {
        mod = 0;
    } else if ((offset % 64 == 0) && (offset / 64 >= -128) && (offset / 64 <= 127)) {
        mod = 1;
    } else {
        mod = 2;
    }
    x86_modrm(c, mod, src & 7, base & 7);
    if ((base & 7) == X86_RSP) me_emit_u8(c, 0x24);
    if (mod == 1) me_emit_u8(c, (uint8_t)(int8_t)(offset / 64));
    else if (mod == 2) me_emit_u32(c, (uint32_t)offset);
}

static void x86_emit_v512_addps(ZeroCCCompiler *c, int dst, int src1, int src2) {
    c->used_avx512 = true;
    c->used_avx = true;
    x86_evex(c, 1, 0, 0, dst, src2, src1, 2, 0, 0);
    me_emit_u8(c, 0x58);
    x86_modrm(c, 3, dst & 7, src2 & 7);
}

static void x86_emit_v512_subps(ZeroCCCompiler *c, int dst, int src1, int src2) {
    c->used_avx512 = true;
    c->used_avx = true;
    x86_evex(c, 1, 0, 0, dst, src2, src1, 2, 0, 0);
    me_emit_u8(c, 0x5C);
    x86_modrm(c, 3, dst & 7, src2 & 7);
}

static void x86_emit_v512_mulps(ZeroCCCompiler *c, int dst, int src1, int src2) {
    c->used_avx512 = true;
    c->used_avx = true;
    x86_evex(c, 1, 0, 0, dst, src2, src1, 2, 0, 0);
    me_emit_u8(c, 0x59);
    x86_modrm(c, 3, dst & 7, src2 & 7);
}

static void x86_emit_v512_divps(ZeroCCCompiler *c, int dst, int src1, int src2) {
    c->used_avx512 = true;
    c->used_avx = true;
    x86_evex(c, 1, 0, 0, dst, src2, src1, 2, 0, 0);
    me_emit_u8(c, 0x5E);
    x86_modrm(c, 3, dst & 7, src2 & 7);
}

static void x86_emit_v512_addpd(ZeroCCCompiler *c, int dst, int src1, int src2) {
    c->used_avx512 = true;
    c->used_avx = true;
    x86_evex(c, 1, 1, 1, dst, src2, src1, 2, 0, 0);
    me_emit_u8(c, 0x58);
    x86_modrm(c, 3, dst & 7, src2 & 7);
}

static void x86_emit_v512_subpd(ZeroCCCompiler *c, int dst, int src1, int src2) {
    c->used_avx512 = true;
    c->used_avx = true;
    x86_evex(c, 1, 1, 1, dst, src2, src1, 2, 0, 0);
    me_emit_u8(c, 0x5C);
    x86_modrm(c, 3, dst & 7, src2 & 7);
}

static void x86_emit_v512_mulpd(ZeroCCCompiler *c, int dst, int src1, int src2) {
    c->used_avx512 = true;
    c->used_avx = true;
    x86_evex(c, 1, 1, 1, dst, src2, src1, 2, 0, 0);
    me_emit_u8(c, 0x59);
    x86_modrm(c, 3, dst & 7, src2 & 7);
}

static void x86_emit_v512_divpd(ZeroCCCompiler *c, int dst, int src1, int src2) {
    c->used_avx512 = true;
    c->used_avx = true;
    x86_evex(c, 1, 1, 1, dst, src2, src1, 2, 0, 0);
    me_emit_u8(c, 0x5E);
    x86_modrm(c, 3, dst & 7, src2 & 7);
}

static void x86_emit_v512_andps(ZeroCCCompiler *c, int dst, int src1, int src2) {
    c->used_avx512 = true;
    c->used_avx = true;
    x86_evex(c, 1, 0, 0, dst, src2, src1, 2, 0, 0);
    me_emit_u8(c, 0x54);
    x86_modrm(c, 3, dst & 7, src2 & 7);
}

static void x86_emit_v512_orps(ZeroCCCompiler *c, int dst, int src1, int src2) {
    c->used_avx512 = true;
    c->used_avx = true;
    x86_evex(c, 1, 0, 0, dst, src2, src1, 2, 0, 0);
    me_emit_u8(c, 0x56);
    x86_modrm(c, 3, dst & 7, src2 & 7);
}

static void x86_emit_v512_vxorps(ZeroCCCompiler *c, int dst, int src1, int src2) {
    c->used_avx512 = true;
    c->used_avx = true;
    x86_evex(c, 1, 0, 0, dst, src2, src1, 2, 0, 0);
    me_emit_u8(c, 0x57);
    x86_modrm(c, 3, dst & 7, src2 & 7);
}

/* AVX-512 Opmask / Predication (Sprint 201 & Sprint 205) */
static void x86_emit_kmov_to_k1(ZeroCCCompiler *c, int src_gpr) {
    c->used_avx512 = true;
    c->used_avx = true;
    x86_vex(c, 1, 0, 0, 1, src_gpr, 0, 0);
    me_emit_u8(c, 0x92);
    x86_modrm(c, 3, 1, src_gpr & 7);
}

static void x86_emit_kmov_from_k1(ZeroCCCompiler *c, int dst_gpr) {
    c->used_avx512 = true;
    c->used_avx = true;
    x86_vex(c, 1, 0, 0, dst_gpr, 1, 0, 0);
    me_emit_u8(c, 0x93);
    x86_modrm(c, 3, dst_gpr & 7, 1);
}

static void x86_emit_kmov_to_k(ZeroCCCompiler *c, int k_dst, int src_gpr) {
    c->used_avx512 = true;
    c->used_avx = true;
    x86_vex(c, 1, 0, 0, k_dst & 7, src_gpr, 0, 0);
    me_emit_u8(c, 0x92);
    x86_modrm(c, 3, k_dst & 7, src_gpr & 7);
}

static void x86_emit_kmov_from_k(ZeroCCCompiler *c, int dst_gpr, int k_src) {
    c->used_avx512 = true;
    c->used_avx = true;
    x86_vex(c, 1, 0, 0, dst_gpr, k_src & 7, 0, 0);
    me_emit_u8(c, 0x93);
    x86_modrm(c, 3, dst_gpr & 7, k_src & 7);
}

static void x86_emit_kmov_kk(ZeroCCCompiler *c, int k_dst, int k_src) {
    c->used_avx512 = true;
    c->used_avx = true;
    x86_vex(c, 1, 0, 0, k_dst & 7, k_src & 7, 0, 0);
    me_emit_u8(c, 0x90);
    x86_modrm(c, 3, k_dst & 7, k_src & 7);
}

static void x86_emit_kand(ZeroCCCompiler *c, int k_dst, int k_src1, int k_src2) {
    c->used_avx512 = true;
    c->used_avx = true;
    x86_vex(c, 1, 0, 0, k_dst & 7, k_src2 & 7, k_src1 & 7, 1);
    me_emit_u8(c, 0x41);
    x86_modrm(c, 3, k_dst & 7, k_src2 & 7);
}

static void x86_emit_kor(ZeroCCCompiler *c, int k_dst, int k_src1, int k_src2) {
    c->used_avx512 = true;
    c->used_avx = true;
    x86_vex(c, 1, 0, 0, k_dst & 7, k_src2 & 7, k_src1 & 7, 1);
    me_emit_u8(c, 0x45);
    x86_modrm(c, 3, k_dst & 7, k_src2 & 7);
}

static void x86_emit_kxor(ZeroCCCompiler *c, int k_dst, int k_src1, int k_src2) {
    c->used_avx512 = true;
    c->used_avx = true;
    x86_vex(c, 1, 0, 0, k_dst & 7, k_src2 & 7, k_src1 & 7, 1);
    me_emit_u8(c, 0x47);
    x86_modrm(c, 3, k_dst & 7, k_src2 & 7);
}

static void x86_emit_knot(ZeroCCCompiler *c, int k_dst, int k_src) {
    c->used_avx512 = true;
    c->used_avx = true;
    x86_vex(c, 1, 0, 0, k_dst & 7, k_src & 7, 0, 0);
    me_emit_u8(c, 0x44);
    x86_modrm(c, 3, k_dst & 7, k_src & 7);
}

static void x86_emit_cond_set(ZeroCCCompiler *c, int dst, int cond);

static void x86_emit_kortest(ZeroCCCompiler *c, int dst_gpr, int k_src1, int k_src2, int test_type) {
    c->used_avx512 = true;
    c->used_avx = true;
    x86_vex(c, 1, 0, 0, k_src1 & 7, k_src2 & 7, 0, 0);
    me_emit_u8(c, 0x98);
    x86_modrm(c, 3, k_src1 & 7, k_src2 & 7);
    x86_emit_cond_set(c, dst_gpr, (test_type == 0) ? 4 /* CC_EQ */ : 6 /* CC_ULT */);
}

static void x86_emit_v512_blendps(ZeroCCCompiler *c, int dst, int src1, int src2, int k_mask) {
    c->used_avx512 = true;
    c->used_avx = true;
    x86_evex(c, 2, 1, 0, dst, src2, src1, 2, k_mask & 7, 0);
    me_emit_u8(c, 0x65);
    x86_modrm(c, 3, dst & 7, src2 & 7);
}

static void x86_emit_v512_blendpd(ZeroCCCompiler *c, int dst, int src1, int src2, int k_mask) {
    c->used_avx512 = true;
    c->used_avx = true;
    x86_evex(c, 2, 1, 1, dst, src2, src1, 2, k_mask & 7, 0);
    me_emit_u8(c, 0x65);
    x86_modrm(c, 3, dst & 7, src2 & 7);
}

static void x86_emit_v512_mask_load(ZeroCCCompiler *c, int dst, int base, int32_t offset, int z) {
    c->used_avx512 = true;
    c->used_avx = true;
    x86_evex(c, 1, 0, 0, dst, base, 0, 2, 1, z ? 1 : 0);
    me_emit_u8(c, 0x10);
    int mod;
    if (offset == 0 && (base & 7) != X86_RBP) {
        mod = 0;
    } else if ((offset % 64 == 0) && (offset / 64 >= -128) && (offset / 64 <= 127)) {
        mod = 1;
    } else {
        mod = 2;
    }
    x86_modrm(c, mod, dst & 7, base & 7);
    if ((base & 7) == X86_RSP) me_emit_u8(c, 0x24);
    if (mod == 1) me_emit_u8(c, (uint8_t)(int8_t)(offset / 64));
    else if (mod == 2) me_emit_u32(c, (uint32_t)offset);
}

static void x86_emit_v512_mask_store(ZeroCCCompiler *c, int src, int base, int32_t offset) {
    c->used_avx512 = true;
    c->used_avx = true;
    x86_evex(c, 1, 0, 0, src, base, 0, 2, 1, 0);
    me_emit_u8(c, 0x11);
    int mod;
    if (offset == 0 && (base & 7) != X86_RBP) {
        mod = 0;
    } else if ((offset % 64 == 0) && (offset / 64 >= -128) && (offset / 64 <= 127)) {
        mod = 1;
    } else {
        mod = 2;
    }
    x86_modrm(c, mod, src & 7, base & 7);
    if ((base & 7) == X86_RSP) me_emit_u8(c, 0x24);
    if (mod == 1) me_emit_u8(c, (uint8_t)(int8_t)(offset / 64));
    else if (mod == 2) me_emit_u32(c, (uint32_t)offset);
}

static void x86_emit_v512_mask_addps(ZeroCCCompiler *c, int dst, int src1, int src2, int z) {
    c->used_avx512 = true;
    c->used_avx = true;
    x86_evex(c, 1, 0, 0, dst, src2, src1, 2, 1, z ? 1 : 0);
    me_emit_u8(c, 0x58);
    x86_modrm(c, 3, dst & 7, src2 & 7);
}

static void x86_emit_v512_mask_subps(ZeroCCCompiler *c, int dst, int src1, int src2, int z) {
    c->used_avx512 = true;
    c->used_avx = true;
    x86_evex(c, 1, 0, 0, dst, src2, src1, 2, 1, z ? 1 : 0);
    me_emit_u8(c, 0x5C);
    x86_modrm(c, 3, dst & 7, src2 & 7);
}

static void x86_emit_v512_mask_mulps(ZeroCCCompiler *c, int dst, int src1, int src2, int z) {
    c->used_avx512 = true;
    c->used_avx = true;
    x86_evex(c, 1, 0, 0, dst, src2, src1, 2, 1, z ? 1 : 0);
    me_emit_u8(c, 0x59);
    x86_modrm(c, 3, dst & 7, src2 & 7);
}

static void x86_emit_v512_mask_divps(ZeroCCCompiler *c, int dst, int src1, int src2, int z) {
    c->used_avx512 = true;
    c->used_avx = true;
    x86_evex(c, 1, 0, 0, dst, src2, src1, 2, 1, z ? 1 : 0);
    me_emit_u8(c, 0x5E);
    x86_modrm(c, 3, dst & 7, src2 & 7);
}

static void x86_emit_v512_mask_addpd(ZeroCCCompiler *c, int dst, int src1, int src2, int z) {
    c->used_avx512 = true;
    c->used_avx = true;
    x86_evex(c, 1, 1, 1, dst, src2, src1, 2, 1, z ? 1 : 0);
    me_emit_u8(c, 0x58);
    x86_modrm(c, 3, dst & 7, src2 & 7);
}

static void x86_emit_v512_mask_subpd(ZeroCCCompiler *c, int dst, int src1, int src2, int z) {
    c->used_avx512 = true;
    c->used_avx = true;
    x86_evex(c, 1, 1, 1, dst, src2, src1, 2, 1, z ? 1 : 0);
    me_emit_u8(c, 0x5C);
    x86_modrm(c, 3, dst & 7, src2 & 7);
}

static void x86_emit_v512_mask_mulpd(ZeroCCCompiler *c, int dst, int src1, int src2, int z) {
    c->used_avx512 = true;
    c->used_avx = true;
    x86_evex(c, 1, 1, 1, dst, src2, src1, 2, 1, z ? 1 : 0);
    me_emit_u8(c, 0x59);
    x86_modrm(c, 3, dst & 7, src2 & 7);
}

static void x86_emit_v512_mask_divpd(ZeroCCCompiler *c, int dst, int src1, int src2, int z) {
    c->used_avx512 = true;
    c->used_avx = true;
    x86_evex(c, 1, 1, 1, dst, src2, src1, 2, 1, z ? 1 : 0);
    me_emit_u8(c, 0x5E);
    x86_modrm(c, 3, dst & 7, src2 & 7);
}

static void x86_emit_v512_cmp_mask(ZeroCCCompiler *c, int dst_gpr, int src1, int src2, int imm_predicate, int cmp_type) {
    c->used_avx512 = true;
    c->used_avx = true;
    int mm = 1, pp = 0, w = 0, is_64 = 0;
    uint8_t op = 0xC2;
    if (cmp_type == 1) { /* pd */
        pp = 1; w = 1; is_64 = 1; op = 0xC2;
    } else if (cmp_type == 2) { /* epi32 */
        mm = 3; pp = 1; w = 0; op = 0x1F;
    } else if (cmp_type == 3) { /* epu32 */
        mm = 3; pp = 1; w = 0; op = 0x1E;
    } else if (cmp_type == 4) { /* epi64 */
        mm = 3; pp = 1; w = 1; is_64 = 1; op = 0x1F;
    } else if (cmp_type == 5) { /* epu64 */
        mm = 3; pp = 1; w = 1; is_64 = 1; op = 0x1E;
    }
    x86_evex(c, mm, pp, w, 1, src2, src1, 2, 0, 0);
    me_emit_u8(c, op);
    x86_modrm(c, 3, 1, src2 & 7);
    me_emit_u8(c, (uint8_t)imm_predicate);
    x86_vex(c, 1, is_64 ? 1 : 0, 0, dst_gpr, 1, 0, 0);
    me_emit_u8(c, 0x93);
    x86_modrm(c, 3, dst_gpr & 7, 1);
}

/* AVX-512 Integer SIMD Raw Binary Emitters (Sprint 202) */
static void x86_emit_v512_int_binop(ZeroCCCompiler *c, int dst, int src1, int src2, int mask_mode, int mm, int pp, int w, uint8_t op) {
    c->used_avx512 = true;
    c->used_avx = true;
    int mask = (mask_mode > 0) ? 1 : 0;
    int z = (mask_mode == 2) ? 1 : 0;
    x86_evex(c, mm, pp, w, dst, src2, src1, 2, mask, z);
    me_emit_u8(c, op);
    x86_modrm(c, 3, dst & 7, src2 & 7);
}

static void x86_emit_v512_paddd(ZeroCCCompiler *c, int dst, int src1, int src2, int mask_mode) {
    x86_emit_v512_int_binop(c, dst, src1, src2, mask_mode, 1, 1, 0, 0xFE);
}

static void x86_emit_v512_psubd(ZeroCCCompiler *c, int dst, int src1, int src2, int mask_mode) {
    x86_emit_v512_int_binop(c, dst, src1, src2, mask_mode, 1, 1, 0, 0xFA);
}

static void x86_emit_v512_pmulld(ZeroCCCompiler *c, int dst, int src1, int src2, int mask_mode) {
    x86_emit_v512_int_binop(c, dst, src1, src2, mask_mode, 2, 1, 0, 0x40);
}

static void x86_emit_v512_paddq(ZeroCCCompiler *c, int dst, int src1, int src2, int mask_mode) {
    x86_emit_v512_int_binop(c, dst, src1, src2, mask_mode, 1, 1, 1, 0xD4);
}

static void x86_emit_v512_psubq(ZeroCCCompiler *c, int dst, int src1, int src2, int mask_mode) {
    x86_emit_v512_int_binop(c, dst, src1, src2, mask_mode, 1, 1, 1, 0xFB);
}

static void x86_emit_v512_pmullq(ZeroCCCompiler *c, int dst, int src1, int src2, int mask_mode) {
    x86_emit_v512_int_binop(c, dst, src1, src2, mask_mode, 2, 1, 1, 0x40);
}

static void x86_emit_v512_pandd(ZeroCCCompiler *c, int dst, int src1, int src2, int mask_mode) {
    x86_emit_v512_int_binop(c, dst, src1, src2, mask_mode, 1, 1, 0, 0xDB);
}

static void x86_emit_v512_pord(ZeroCCCompiler *c, int dst, int src1, int src2, int mask_mode) {
    x86_emit_v512_int_binop(c, dst, src1, src2, mask_mode, 1, 1, 0, 0xEB);
}

static void x86_emit_v512_pxord(ZeroCCCompiler *c, int dst, int src1, int src2, int mask_mode) {
    x86_emit_v512_int_binop(c, dst, src1, src2, mask_mode, 1, 1, 0, 0xEF);
}

static void x86_emit_v512_pandnd(ZeroCCCompiler *c, int dst, int src1, int src2, int mask_mode) {
    x86_emit_v512_int_binop(c, dst, src1, src2, mask_mode, 1, 1, 0, 0xDF);
}

static void x86_emit_v512_paddb(ZeroCCCompiler *c, int dst, int src1, int src2, int mask_mode) {
    x86_emit_v512_int_binop(c, dst, src1, src2, mask_mode, 1, 1, 0, 0xFC);
}

static void x86_emit_v512_psubb(ZeroCCCompiler *c, int dst, int src1, int src2, int mask_mode) {
    x86_emit_v512_int_binop(c, dst, src1, src2, mask_mode, 1, 1, 0, 0xF8);
}

static void x86_emit_v512_paddw(ZeroCCCompiler *c, int dst, int src1, int src2, int mask_mode) {
    x86_emit_v512_int_binop(c, dst, src1, src2, mask_mode, 1, 1, 0, 0xFD);
}

static void x86_emit_v512_psubw(ZeroCCCompiler *c, int dst, int src1, int src2, int mask_mode) {
    x86_emit_v512_int_binop(c, dst, src1, src2, mask_mode, 1, 1, 0, 0xF9);
}

static void x86_emit_v512_pmullw(ZeroCCCompiler *c, int dst, int src1, int src2, int mask_mode) {
    x86_emit_v512_int_binop(c, dst, src1, src2, mask_mode, 1, 1, 0, 0xD5);
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
    if (fn && !fn->is_prototype && (!fn->lto_defer || fn->lto_lowered)) {
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

static void x86_emit_tail_jmp(ZeroCCCompiler *c, const char *fn_name) {
    ZeroCCFunction *fn = zero_cc_find_function_by_symbol(c, fn_name);
    bool is_external = (!fn || fn->is_prototype);

    if (is_external && c->jit_mode) {
        me_emit_u8(c, 0x49);              /* REX.WR */
        me_emit_u8(c, 0xBB);              /* mov r11, imm64 */
        size_t patch_pos = me_get_offset(c);
        me_emit_u64(c, 0);                /* placeholder target address (8 bytes) */
        me_emit_u8(c, 0x41);              /* REX.B */
        me_emit_u8(c, 0xFF);              /* jmp *r/m64 */
        me_emit_u8(c, 0xE3);              /* r11 (scratch_reg2) */
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

    /* Relative jump (E9 rel32) */
    me_emit_u8(c, 0xE9);
    size_t patch_pos = me_get_offset(c);

    int32_t rel = 0;
    if (fn && !fn->is_prototype && (!fn->lto_defer || fn->lto_lowered)) {
        rel = (int32_t)(fn->text_offset - (patch_pos + 4));
    }
    me_emit_u32(c, (uint32_t)rel);

    if (c->reloc_count < ZERO_CC_MAX_RELOCS) {
        ZeroCCReloc *r = &c->relocs[c->reloc_count++];
        r->text_patch_offset = patch_pos;
        snprintf(r->target_symbol, sizeof(r->target_symbol), "%s", fn_name);
        r->is_call = true;
        r->is_tls = false;
    } else {
        zero_cc_reloc_overflow_note(c);
    }
}

static void x86_emit_stack_guard_init(ZeroCCCompiler *c, int32_t slot_off) {
    /* mov %fs:40, %r11: 64 4c 8b 1c 25 28 00 00 00 */
    const uint8_t fs_load_r11[] = {0x64, 0x4c, 0x8b, 0x1c, 0x25, 0x28, 0x00, 0x00, 0x00};
    me_emit_bytes(c, fs_load_r11, sizeof(fs_load_r11));
    int base_reg = zero_cc_frame_base_reg(c);
    int32_t disp = zero_cc_frame_disp(c, -slot_off);
    x86_emit_store(c, X86_R11, base_reg, disp);
    /* xor %r11, %r11: 4d 31 db */
    const uint8_t xor_r11[] = {0x4d, 0x31, 0xdb};
    me_emit_bytes(c, xor_r11, sizeof(xor_r11));
}

static void x86_emit_stack_guard_check(ZeroCCCompiler *c, int32_t slot_off) {
    int base_reg = zero_cc_frame_base_reg(c);
    int32_t disp = zero_cc_frame_disp(c, -slot_off);
    x86_emit_load(c, X86_R11, base_reg, disp);
    /* sub %fs:40, %r11: 64 4c 2b 1c 25 28 00 00 00 */
    const uint8_t fs_sub_r11[] = {0x64, 0x4c, 0x2b, 0x1c, 0x25, 0x28, 0x00, 0x00, 0x00};
    me_emit_bytes(c, fs_sub_r11, sizeof(fs_sub_r11));
    /* je .Lok (over the call to __stack_chk_fail) */
    me_emit_u8(c, 0x74); /* je rel8 */
    size_t patch_jump = me_get_offset(c);
    me_emit_u8(c, 0); /* placeholder */
    x86_emit_call(c, "__stack_chk_fail");
    size_t end_pos = me_get_offset(c);
    int8_t disp8 = (int8_t)(end_pos - (patch_jump + 1));
    c->text_section.data[patch_jump] = (uint8_t)disp8;
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

/* Horizontal SIMD Vector Reductions (Sprint 210) */
static void x86_emit_haddps_rr(ZeroCCCompiler *c, int dst, int src) {
    me_emit_u8(c, 0xF2);
    x86_rex(c, 0, (dst >> 3) & 1, 0, (src >> 3) & 1);
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x7C);
    x86_modrm(c, 3, dst & 7, src & 7);
}

static void x86_emit_haddpd_rr(ZeroCCCompiler *c, int dst, int src) {
    me_emit_u8(c, 0x66);
    x86_rex(c, 0, (dst >> 3) & 1, 0, (src >> 3) & 1);
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x7C);
    x86_modrm(c, 3, dst & 7, src & 7);
}

static void x86_emit_phaddd_rr(ZeroCCCompiler *c, int dst, int src) {
    me_emit_u8(c, 0x66);
    x86_rex(c, 0, (dst >> 3) & 1, 0, (src >> 3) & 1);
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x38);
    me_emit_u8(c, 0x02);
    x86_modrm(c, 3, dst & 7, src & 7);
}

static void x86_emit_movd_xmm_to_gpr(ZeroCCCompiler *c, int dst_gpr, int src_xmm) {
    me_emit_u8(c, 0x66);
    x86_rex(c, 0, (src_xmm >> 3) & 1, 0, (dst_gpr >> 3) & 1);
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x7E);
    x86_modrm(c, 3, src_xmm & 7, dst_gpr & 7);
}

static void x86_emit_movhlps_rr(ZeroCCCompiler *c, int dst, int src) {
    x86_rex(c, 0, (dst >> 3) & 1, 0, (src >> 3) & 1);
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x12);
    x86_modrm(c, 3, dst & 7, src & 7);
}

static void x86_emit_minps_rr(ZeroCCCompiler *c, int dst, int src) {
    x86_rex(c, 0, (dst >> 3) & 1, 0, (src >> 3) & 1);
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x5D);
    x86_modrm(c, 3, dst & 7, src & 7);
}

static void x86_emit_maxps_rr(ZeroCCCompiler *c, int dst, int src) {
    x86_rex(c, 0, (dst >> 3) & 1, 0, (src >> 3) & 1);
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x5F);
    x86_modrm(c, 3, dst & 7, src & 7);
}

static void x86_emit_minss_rr(ZeroCCCompiler *c, int dst, int src) {
    me_emit_u8(c, 0xF3);
    x86_rex(c, 0, (dst >> 3) & 1, 0, (src >> 3) & 1);
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x5D);
    x86_modrm(c, 3, dst & 7, src & 7);
}

static void x86_emit_maxss_rr(ZeroCCCompiler *c, int dst, int src) {
    me_emit_u8(c, 0xF3);
    x86_rex(c, 0, (dst >> 3) & 1, 0, (src >> 3) & 1);
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x5F);
    x86_modrm(c, 3, dst & 7, src & 7);
}

static void x86_emit_shufps_rr(ZeroCCCompiler *c, int dst, int src, uint8_t imm) {
    x86_rex(c, 0, (dst >> 3) & 1, 0, (src >> 3) & 1);
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0xC6);
    x86_modrm(c, 3, dst & 7, src & 7);
    me_emit_u8(c, imm);
}

static void x86_emit_pshufd_rr(ZeroCCCompiler *c, int dst, int src, uint8_t imm) {
    me_emit_u8(c, 0x66);
    x86_rex(c, 0, (dst >> 3) & 1, 0, (src >> 3) & 1);
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x70);
    x86_modrm(c, 3, dst & 7, src & 7);
    me_emit_u8(c, imm);
}

static void x86_emit_pminsd_rr(ZeroCCCompiler *c, int dst, int src) {
    me_emit_u8(c, 0x66);
    x86_rex(c, 0, (dst >> 3) & 1, 0, (src >> 3) & 1);
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x38);
    me_emit_u8(c, 0x39);
    x86_modrm(c, 3, dst & 7, src & 7);
}

static void x86_emit_pmaxsd_rr(ZeroCCCompiler *c, int dst, int src) {
    me_emit_u8(c, 0x66);
    x86_rex(c, 0, (dst >> 3) & 1, 0, (src >> 3) & 1);
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x38);
    me_emit_u8(c, 0x3D);
    x86_modrm(c, 3, dst & 7, src & 7);
}

static void x86_emit_v128_hadd(ZeroCCCompiler *c, int dst, int src, int esz, int is_fp) {
    if (is_fp) {
        if (esz == 3) {
            if (dst != src) x86_emit_vmovaps(c, dst, src);
            x86_emit_haddpd_rr(c, dst, dst);
        } else {
            if (dst != src) x86_emit_vmovaps(c, dst, src);
            x86_emit_haddps_rr(c, dst, dst);
            x86_emit_haddps_rr(c, dst, dst);
            x86_emit_fp_cvtss2sd(c, dst, dst);
        }
    } else {
        int scratch = 15; /* xmm15 */
        x86_emit_vmovaps(c, scratch, src);
        x86_emit_phaddd_rr(c, scratch, scratch);
        x86_emit_phaddd_rr(c, scratch, scratch);
        x86_emit_movd_xmm_to_gpr(c, dst, scratch);
    }
}

static void x86_emit_v128_hmin(ZeroCCCompiler *c, int dst, int src, int esz, int is_fp) {
    if (is_fp) {
        if (dst != src) x86_emit_vmovaps(c, dst, src);
        x86_emit_movhlps_rr(c, 15, dst);
        if (esz == 3) {
            me_emit_u8(c, 0xF2);
            x86_rex(c, 0, (dst >> 3) & 1, 0, 1);
            me_emit_u8(c, 0x0F);
            me_emit_u8(c, 0x5D);
            x86_modrm(c, 3, dst & 7, 15 & 7);
        } else {
            x86_emit_minps_rr(c, dst, 15);
            x86_emit_shufps_rr(c, 15, dst, 1);
            x86_emit_minss_rr(c, dst, 15);
            x86_emit_fp_cvtss2sd(c, dst, dst);
        }
    } else {
        x86_emit_vmovaps(c, 15, src);
        x86_emit_pshufd_rr(c, 14, 15, 0xEE);
        x86_emit_pminsd_rr(c, 15, 14);
        x86_emit_pshufd_rr(c, 14, 15, 0x55);
        x86_emit_pminsd_rr(c, 15, 14);
        x86_emit_movd_xmm_to_gpr(c, dst, 15);
    }
}

static void x86_emit_v128_hmax(ZeroCCCompiler *c, int dst, int src, int esz, int is_fp) {
    if (is_fp) {
        if (dst != src) x86_emit_vmovaps(c, dst, src);
        x86_emit_movhlps_rr(c, 15, dst);
        if (esz == 3) {
            me_emit_u8(c, 0xF2);
            x86_rex(c, 0, (dst >> 3) & 1, 0, 1);
            me_emit_u8(c, 0x0F);
            me_emit_u8(c, 0x5F);
            x86_modrm(c, 3, dst & 7, 15 & 7);
        } else {
            x86_emit_maxps_rr(c, dst, 15);
            x86_emit_shufps_rr(c, 15, dst, 1);
            x86_emit_maxss_rr(c, dst, 15);
            x86_emit_fp_cvtss2sd(c, dst, dst);
        }
    } else {
        x86_emit_vmovaps(c, 15, src);
        x86_emit_pshufd_rr(c, 14, 15, 0xEE);
        x86_emit_pmaxsd_rr(c, 15, 14);
        x86_emit_pshufd_rr(c, 14, 15, 0x55);
        x86_emit_pmaxsd_rr(c, 15, 14);
        x86_emit_movd_xmm_to_gpr(c, dst, 15);
    }
}

static inline void x86_pinsrd(ZeroCCCompiler *c, int xmm_dst, int base, int32_t disp, uint8_t lane) {
    me_emit_u8(c, 0x66);
    x86_rex(c, 0, (xmm_dst >> 3) & 1, 0, (base >> 3) & 1);
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x3A);
    me_emit_u8(c, 0x22);
    int mod;
    if (disp == 0 && (base & 7) != X86_RBP) mod = 0;
    else if (disp >= -128 && disp <= 127) mod = 1;
    else mod = 2;
    x86_modrm(c, mod, xmm_dst & 7, base & 7);
    if ((base & 7) == X86_RSP) me_emit_u8(c, 0x24);
    if (mod == 1) me_emit_u8(c, (uint8_t)(int8_t)disp);
    else if (mod == 2) me_emit_u32(c, (uint32_t)disp);
    me_emit_u8(c, lane);
}

static inline void x86_pinsrq(ZeroCCCompiler *c, int xmm_dst, int base, int32_t disp, uint8_t lane) {
    me_emit_u8(c, 0x66);
    x86_rex(c, 1, (xmm_dst >> 3) & 1, 0, (base >> 3) & 1);
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x3A);
    me_emit_u8(c, 0x22);
    int mod;
    if (disp == 0 && (base & 7) != X86_RBP) mod = 0;
    else if (disp >= -128 && disp <= 127) mod = 1;
    else mod = 2;
    x86_modrm(c, mod, xmm_dst & 7, base & 7);
    if ((base & 7) == X86_RSP) me_emit_u8(c, 0x24);
    if (mod == 1) me_emit_u8(c, (uint8_t)(int8_t)disp);
    else if (mod == 2) me_emit_u32(c, (uint32_t)disp);
    me_emit_u8(c, lane);
}

static inline void x86_pextrd(ZeroCCCompiler *c, int xmm_src, int base, int32_t disp, uint8_t lane) {
    me_emit_u8(c, 0x66);
    x86_rex(c, 0, (xmm_src >> 3) & 1, 0, (base >> 3) & 1);
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x3A);
    me_emit_u8(c, 0x16);
    int mod;
    if (disp == 0 && (base & 7) != X86_RBP) mod = 0;
    else if (disp >= -128 && disp <= 127) mod = 1;
    else mod = 2;
    x86_modrm(c, mod, xmm_src & 7, base & 7);
    if ((base & 7) == X86_RSP) me_emit_u8(c, 0x24);
    if (mod == 1) me_emit_u8(c, (uint8_t)(int8_t)disp);
    else if (mod == 2) me_emit_u32(c, (uint32_t)disp);
    me_emit_u8(c, lane);
}

static inline void x86_pextrq(ZeroCCCompiler *c, int xmm_src, int base, int32_t disp, uint8_t lane) {
    me_emit_u8(c, 0x66);
    x86_rex(c, 1, (xmm_src >> 3) & 1, 0, (base >> 3) & 1);
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x3A);
    me_emit_u8(c, 0x16);
    int mod;
    if (disp == 0 && (base & 7) != X86_RBP) mod = 0;
    else if (disp >= -128 && disp <= 127) mod = 1;
    else mod = 2;
    x86_modrm(c, mod, xmm_src & 7, base & 7);
    if ((base & 7) == X86_RSP) me_emit_u8(c, 0x24);
    if (mod == 1) me_emit_u8(c, (uint8_t)(int8_t)disp);
    else if (mod == 2) me_emit_u32(c, (uint32_t)disp);
    me_emit_u8(c, lane);
}

static void x86_emit_v128_load_strided(ZeroCCCompiler *c, int dst, int base, int stride, int esz) {
    if (esz == 8) {
        me_emit_u8(c, 0xF2);
        x86_rex(c, 0, (dst >> 3) & 1, 0, (base >> 3) & 1);
        me_emit_u8(c, 0x0F);
        me_emit_u8(c, 0x10);
        int mod = (base & 7) == X86_RBP ? 1 : 0;
        x86_modrm(c, mod, dst & 7, base & 7);
        if ((base & 7) == X86_RSP) me_emit_u8(c, 0x24);
        if (mod == 1) me_emit_u8(c, 0);

        x86_pinsrq(c, dst, base, stride, 1);
    } else {
        me_emit_u8(c, 0xF3);
        x86_rex(c, 0, (dst >> 3) & 1, 0, (base >> 3) & 1);
        me_emit_u8(c, 0x0F);
        me_emit_u8(c, 0x10);
        int mod = (base & 7) == X86_RBP ? 1 : 0;
        x86_modrm(c, mod, dst & 7, base & 7);
        if ((base & 7) == X86_RSP) me_emit_u8(c, 0x24);
        if (mod == 1) me_emit_u8(c, 0);

        for (int lane = 1; lane < 4; lane++) {
            x86_pinsrd(c, dst, base, lane * stride, (uint8_t)lane);
        }
    }
}

static void x86_emit_v128_store_strided(ZeroCCCompiler *c, int src, int base, int stride, int esz) {
    if (esz == 8) {
        me_emit_u8(c, 0xF2);
        x86_rex(c, 0, (src >> 3) & 1, 0, (base >> 3) & 1);
        me_emit_u8(c, 0x0F);
        me_emit_u8(c, 0x11);
        int mod = (base & 7) == X86_RBP ? 1 : 0;
        x86_modrm(c, mod, src & 7, base & 7);
        if ((base & 7) == X86_RSP) me_emit_u8(c, 0x24);
        if (mod == 1) me_emit_u8(c, 0);

        x86_pextrq(c, src, base, stride, 1);
    } else {
        me_emit_u8(c, 0xF3);
        x86_rex(c, 0, (src >> 3) & 1, 0, (base >> 3) & 1);
        me_emit_u8(c, 0x0F);
        me_emit_u8(c, 0x11);
        int mod = (base & 7) == X86_RBP ? 1 : 0;
        x86_modrm(c, mod, src & 7, base & 7);
        if ((base & 7) == X86_RSP) me_emit_u8(c, 0x24);
        if (mod == 1) me_emit_u8(c, 0);

        for (int lane = 1; lane < 4; lane++) {
            x86_pextrd(c, src, base, lane * stride, (uint8_t)lane);
        }
    }
}

static void x86_emit_v128_gather(ZeroCCCompiler *c, int dst, int base, int idx, int esz) {
    int lanes = (esz == 8) ? 2 : 4;
    for (int k = 0; k < lanes; k++) {
        if (esz == 8) {
            /* pextrq $k, %xmm_idx, %r10 */
            me_emit_u8(c, 0x66);
            x86_rex(c, 1, (idx >> 3) & 1, 0, 1);
            me_emit_u8(c, 0x0F);
            me_emit_u8(c, 0x3A);
            me_emit_u8(c, 0x16);
            me_emit_u8(c, (uint8_t)(0xC0 | ((idx & 7) << 3) | 2));
            me_emit_u8(c, (uint8_t)k);

            /* shl $3, %r10 */
            me_emit_u8(c, 0x49);
            me_emit_u8(c, 0xC1);
            me_emit_u8(c, 0xE2);
            me_emit_u8(c, 0x03);

            /* add %base, %r10 */
            x86_rex(c, 1, (base >> 3) & 1, 0, 1);
            me_emit_u8(c, 0x01);
            me_emit_u8(c, (uint8_t)(0xC0 | ((base & 7) << 3) | 2));

            /* pinsrq $k, (%r10), %xmm_dst */
            x86_pinsrq(c, dst, 10, 0, (uint8_t)k);
        } else {
            /* pextrd $k, %xmm_idx, %r10d */
            me_emit_u8(c, 0x66);
            x86_rex(c, 0, (idx >> 3) & 1, 0, 1);
            me_emit_u8(c, 0x0F);
            me_emit_u8(c, 0x3A);
            me_emit_u8(c, 0x16);
            me_emit_u8(c, (uint8_t)(0xC0 | ((idx & 7) << 3) | 2));
            me_emit_u8(c, (uint8_t)k);

            /* movslq %r10d, %r10 */
            me_emit_u8(c, 0x4D);
            me_emit_u8(c, 0x63);
            me_emit_u8(c, 0xD2);

            /* shl $2, %r10 */
            me_emit_u8(c, 0x49);
            me_emit_u8(c, 0xC1);
            me_emit_u8(c, 0xE2);
            me_emit_u8(c, 0x02);

            /* add %base, %r10 */
            x86_rex(c, 1, (base >> 3) & 1, 0, 1);
            me_emit_u8(c, 0x01);
            me_emit_u8(c, (uint8_t)(0xC0 | ((base & 7) << 3) | 2));

            /* pinsrd $k, (%r10), %xmm_dst */
            x86_pinsrd(c, dst, 10, 0, (uint8_t)k);
        }
    }
}

static void x86_emit_v128_scatter(ZeroCCCompiler *c, int src, int base, int idx, int esz) {
    int lanes = (esz == 8) ? 2 : 4;
    for (int k = 0; k < lanes; k++) {
        if (esz == 8) {
            /* pextrq $k, %xmm_idx, %r10 */
            me_emit_u8(c, 0x66);
            x86_rex(c, 1, (idx >> 3) & 1, 0, 1);
            me_emit_u8(c, 0x0F);
            me_emit_u8(c, 0x3A);
            me_emit_u8(c, 0x16);
            me_emit_u8(c, (uint8_t)(0xC0 | ((idx & 7) << 3) | 2));
            me_emit_u8(c, (uint8_t)k);

            /* shl $3, %r10 */
            me_emit_u8(c, 0x49);
            me_emit_u8(c, 0xC1);
            me_emit_u8(c, 0xE2);
            me_emit_u8(c, 0x03);

            /* add %base, %r10 */
            x86_rex(c, 1, (base >> 3) & 1, 0, 1);
            me_emit_u8(c, 0x01);
            me_emit_u8(c, (uint8_t)(0xC0 | ((base & 7) << 3) | 2));

            /* pextrq $k, %xmm_src, (%r10) */
            x86_pextrq(c, src, 10, 0, (uint8_t)k);
        } else {
            /* pextrd $k, %xmm_idx, %r10d */
            me_emit_u8(c, 0x66);
            x86_rex(c, 0, (idx >> 3) & 1, 0, 1);
            me_emit_u8(c, 0x0F);
            me_emit_u8(c, 0x3A);
            me_emit_u8(c, 0x16);
            me_emit_u8(c, (uint8_t)(0xC0 | ((idx & 7) << 3) | 2));
            me_emit_u8(c, (uint8_t)k);

            /* movslq %r10d, %r10 */
            me_emit_u8(c, 0x4D);
            me_emit_u8(c, 0x63);
            me_emit_u8(c, 0xD2);

            /* shl $2, %r10 */
            me_emit_u8(c, 0x49);
            me_emit_u8(c, 0xC1);
            me_emit_u8(c, 0xE2);
            me_emit_u8(c, 0x02);

            /* add %base, %r10 */
            x86_rex(c, 1, (base >> 3) & 1, 0, 1);
            me_emit_u8(c, 0x01);
            me_emit_u8(c, (uint8_t)(0xC0 | ((base & 7) << 3) | 2));

            /* pextrd $k, %xmm_src, (%r10) */
            x86_pextrd(c, src, 10, 0, (uint8_t)k);
        }
    }
}

static void x86_emit_andnps_rr(ZeroCCCompiler *c, int dst, int src) {
    x86_rex(c, 0, (dst >> 3) & 1, 0, (src >> 3) & 1);
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x55);
    x86_modrm(c, 3, dst & 7, src & 7);
}

static void x86_emit_pcmpeqd_rr(ZeroCCCompiler *c, int dst, int src) {
    me_emit_u8(c, 0x66);
    x86_rex(c, 0, (dst >> 3) & 1, 0, (src >> 3) & 1);
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x76);
    x86_modrm(c, 3, dst & 7, src & 7);
}

static void x86_emit_pcmpgtd_rr(ZeroCCCompiler *c, int dst, int src) {
    me_emit_u8(c, 0x66);
    x86_rex(c, 0, (dst >> 3) & 1, 0, (src >> 3) & 1);
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x66);
    x86_modrm(c, 3, dst & 7, src & 7);
}

static void x86_emit_cmpps_rr(ZeroCCCompiler *c, int dst, int src, uint8_t pred) {
    x86_rex(c, 0, (dst >> 3) & 1, 0, (src >> 3) & 1);
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0xC2);
    x86_modrm(c, 3, dst & 7, src & 7);
    me_emit_u8(c, pred);
}

static void x86_emit_cmppd_rr(ZeroCCCompiler *c, int dst, int src, uint8_t pred) {
    me_emit_u8(c, 0x66);
    x86_rex(c, 0, (dst >> 3) & 1, 0, (src >> 3) & 1);
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0xC2);
    x86_modrm(c, 3, dst & 7, src & 7);
    me_emit_u8(c, pred);
}

static void x86_emit_v128_select(ZeroCCCompiler *c, int dst, int src_true, int src_false, int mask, int esz) {
    (void)esz;
    int scratch = 15; /* xmm15 */
    x86_emit_vmovaps(c, scratch, mask);
    x86_emit_andnps_rr(c, scratch, src_false);
    if (dst != mask) x86_emit_vmovaps(c, dst, mask);
    x86_emit_vandps(c, dst, dst, src_true);
    x86_emit_vorps(c, dst, dst, scratch);
}

static void x86_emit_v128_cmp(ZeroCCCompiler *c, int dst, int src1, int src2, int cond, int esz, int is_fp) {
    if (is_fp) {
        uint8_t pred = 0;
        int s1 = src1, s2 = src2;
        switch (cond) {
            case CC_EQ: pred = 0; break;
            case CC_LT: case CC_FLT_LT: pred = 1; break;
            case CC_LE: case CC_FLT_LE: pred = 2; break;
            case CC_NE: pred = 4; break;
            case CC_GE: case CC_FLT_GE: pred = 2; s1 = src2; s2 = src1; break;
            case CC_GT: case CC_FLT_GT: pred = 1; s1 = src2; s2 = src1; break;
            default: pred = 0; break;
        }
        if (dst != s1) x86_emit_vmovaps(c, dst, s1);
        if (esz == 8 || esz == 3) {
            x86_emit_cmppd_rr(c, dst, s2, pred);
        } else {
            x86_emit_cmpps_rr(c, dst, s2, pred);
        }
    } else {
        int scratch = 15;
        switch (cond) {
            case CC_EQ:
                if (dst != src1) x86_emit_vmovaps(c, dst, src1);
                x86_emit_pcmpeqd_rr(c, dst, src2);
                break;
            case CC_NE:
                if (dst != src1) x86_emit_vmovaps(c, dst, src1);
                x86_emit_pcmpeqd_rr(c, dst, src2);
                x86_emit_pcmpeqd_rr(c, scratch, scratch);
                x86_emit_vxorps(c, dst, dst, scratch);
                break;
            case CC_GT:
                if (dst != src1) x86_emit_vmovaps(c, dst, src1);
                x86_emit_pcmpgtd_rr(c, dst, src2);
                break;
            case CC_LT:
                if (dst != src2) x86_emit_vmovaps(c, dst, src2);
                x86_emit_pcmpgtd_rr(c, dst, src1);
                break;
            case CC_GE:
                if (dst != src2) x86_emit_vmovaps(c, dst, src2);
                x86_emit_pcmpgtd_rr(c, dst, src1);
                x86_emit_pcmpeqd_rr(c, scratch, scratch);
                x86_emit_vxorps(c, dst, dst, scratch);
                break;
            case CC_LE:
                if (dst != src1) x86_emit_vmovaps(c, dst, src1);
                x86_emit_pcmpgtd_rr(c, dst, src2);
                x86_emit_pcmpeqd_rr(c, scratch, scratch);
                x86_emit_vxorps(c, dst, dst, scratch);
                break;
        }
    }
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
void x86_emit_add_imm(ZeroCCCompiler *c, int dst_reg, int16_t imm) {
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

/* cmp src_reg, 0 */
static void x86_emit_cmp_zero(ZeroCCCompiler *c, int src_reg) {
    x86_alu(c, 0x85, src_reg, src_reg);  /* test src_reg, src_reg */
}

/* not dst_reg */
void x86_emit_not(ZeroCCCompiler *c, int dst_reg, int src_reg) {
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

/* x86-64 VREG mapping: strip VREG_TAG_BIT if passed a VREG_* token */
static int x86_get_int_reg(int vreg) {
    return zc_temp_is_vreg((uint32_t)vreg) ? zc_vreg_hw_num((uint32_t)vreg) : vreg;
}
static int x86_get_fp_reg(int vreg) {
    return zc_temp_is_vreg((uint32_t)vreg) ? zc_vreg_hw_num((uint32_t)vreg) : vreg;
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
    .fp_scratch_reg1 = 15,                     /* xmm15 - outside XMM pool 0-7 */
    .fp_scratch_reg2 = 14,                     /* xmm14 - outside XMM pool 0-7 */
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
    .emit_stack_guard_init = x86_emit_stack_guard_init,
    .emit_stack_guard_check = x86_emit_stack_guard_check,
    .emit_inline_asm_mc = x86_emit_inline_asm_mc,
    .emit_filescope_asm_mc = x86_emit_filescope_asm_mc,

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
    .emit_vpaddb = x86_emit_vpaddb,
    .emit_vpsubb = x86_emit_vpsubb,
    .emit_vpaddw = x86_emit_vpaddw,
    .emit_vpsubw = x86_emit_vpsubw,
    .emit_vpmullw = x86_emit_vpmullw,
    .emit_vpaddd = x86_emit_vpaddd,
    .emit_vpsubd = x86_emit_vpsubd,
    .emit_vpmulld = x86_emit_vpmulld,
    .emit_vmovaps = x86_emit_vmovaps,
    .emit_vandps = x86_emit_vandps,
    .emit_vorps = x86_emit_vorps,
    .emit_vxorps = x86_emit_vxorps,
    .emit_v256_load = x86_emit_v256_load,
    .emit_v256_store = x86_emit_v256_store,
    .emit_v256_addps = x86_emit_v256_addps,
    .emit_v256_subps = x86_emit_v256_subps,
    .emit_v256_mulps = x86_emit_v256_mulps,
    .emit_v256_divps = x86_emit_v256_divps,
    .emit_v256_addpd = x86_emit_v256_addpd,
    .emit_v256_subpd = x86_emit_v256_subpd,
    .emit_v256_mulpd = x86_emit_v256_mulpd,
    .emit_v256_divpd = x86_emit_v256_divpd,
    .emit_v256_andps = x86_emit_v256_andps,
    .emit_v256_vorps = x86_emit_v256_orps,
    .emit_v256_vxorps = x86_emit_v256_vxorps,
    .emit_v256_zeroupper = x86_emit_v256_zeroupper,
    .emit_v512_load = x86_emit_v512_load,
    .emit_v512_store = x86_emit_v512_store,
    .emit_v512_addps = x86_emit_v512_addps,
    .emit_v512_subps = x86_emit_v512_subps,
    .emit_v512_mulps = x86_emit_v512_mulps,
    .emit_v512_divps = x86_emit_v512_divps,
    .emit_v512_addpd = x86_emit_v512_addpd,
    .emit_v512_subpd = x86_emit_v512_subpd,
    .emit_v512_mulpd = x86_emit_v512_mulpd,
    .emit_v512_divpd = x86_emit_v512_divpd,
    .emit_v512_andps = x86_emit_v512_andps,
    .emit_v512_vorps = x86_emit_v512_orps,
    .emit_v512_vxorps = x86_emit_v512_vxorps,
    .emit_kmov_to_k1 = x86_emit_kmov_to_k1,
    .emit_kmov_from_k1 = x86_emit_kmov_from_k1,
    .emit_kmov_to_k = x86_emit_kmov_to_k,
    .emit_kmov_from_k = x86_emit_kmov_from_k,
    .emit_kmov_kk = x86_emit_kmov_kk,
    .emit_kand = x86_emit_kand,
    .emit_kor = x86_emit_kor,
    .emit_kxor = x86_emit_kxor,
    .emit_knot = x86_emit_knot,
    .emit_kortest = x86_emit_kortest,
    .emit_v512_blendps = x86_emit_v512_blendps,
    .emit_v512_blendpd = x86_emit_v512_blendpd,
    .emit_v512_mask_load = x86_emit_v512_mask_load,
    .emit_v512_mask_store = x86_emit_v512_mask_store,
    .emit_v512_mask_addps = x86_emit_v512_mask_addps,
    .emit_v512_mask_subps = x86_emit_v512_mask_subps,
    .emit_v512_mask_mulps = x86_emit_v512_mask_mulps,
    .emit_v512_mask_divps = x86_emit_v512_mask_divps,
    .emit_v512_mask_addpd = x86_emit_v512_mask_addpd,
    .emit_v512_mask_subpd = x86_emit_v512_mask_subpd,
    .emit_v512_mask_mulpd = x86_emit_v512_mask_mulpd,
    .emit_v512_mask_divpd = x86_emit_v512_mask_divpd,
    .emit_v512_cmp_mask = x86_emit_v512_cmp_mask,
    .emit_v512_paddd = x86_emit_v512_paddd,
    .emit_v512_psubd = x86_emit_v512_psubd,
    .emit_v512_pmulld = x86_emit_v512_pmulld,
    .emit_v512_paddq = x86_emit_v512_paddq,
    .emit_v512_psubq = x86_emit_v512_psubq,
    .emit_v512_pmullq = x86_emit_v512_pmullq,
    .emit_v512_pandd = x86_emit_v512_pandd,
    .emit_v512_pord = x86_emit_v512_pord,
    .emit_v512_pxord = x86_emit_v512_pxord,
    .emit_v512_pandnd = x86_emit_v512_pandnd,
    .emit_v512_paddb = x86_emit_v512_paddb,
    .emit_v512_psubb = x86_emit_v512_psubb,
    .emit_v512_paddw = x86_emit_v512_paddw,
    .emit_v512_psubw = x86_emit_v512_psubw,
    .emit_v512_pmullw = x86_emit_v512_pmullw,
    .emit_cvtss2sd = x86_emit_fp_cvtss2sd,
    .emit_cvtsi2ss = x86_emit_fp_cvtsi2ss,
    .emit_cvtsd2ss = x86_emit_fp_cvtsd2ss,
    .emit_v128_hadd = x86_emit_v128_hadd,
    .emit_v128_hmin = x86_emit_v128_hmin,
    .emit_v128_hmax = x86_emit_v128_hmax,
    .emit_v128_load_strided = x86_emit_v128_load_strided,
    .emit_v128_store_strided = x86_emit_v128_store_strided,
    .emit_v128_gather = x86_emit_v128_gather,
    .emit_v128_scatter = x86_emit_v128_scatter,
    .emit_v128_cmp = x86_emit_v128_cmp,
    .emit_v128_select = x86_emit_v128_select,
    .emit_v128_hadd_asm = x86_emit_v128_hadd_asm,
    .emit_v128_hmin_asm = x86_emit_v128_hmin_asm,
    .emit_v128_hmax_asm = x86_emit_v128_hmax_asm,
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
    .emit_tail_jmp = x86_emit_tail_jmp,
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
    .emit_clone = x86_emit_clone,
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
    .emit_v256_load_asm = x86_emit_v256_load_asm,
    .emit_v256_store_asm = x86_emit_v256_store_asm,
    .emit_v256_addps_asm = x86_emit_v256_addps_asm,
    .emit_v256_subps_asm = x86_emit_v256_subps_asm,
    .emit_v256_mulps_asm = x86_emit_v256_mulps_asm,
    .emit_v256_divps_asm = x86_emit_v256_divps_asm,
    .emit_v256_addpd_asm = x86_emit_v256_addpd_asm,
    .emit_v256_subpd_asm = x86_emit_v256_subpd_asm,
    .emit_v256_mulpd_asm = x86_emit_v256_mulpd_asm,
    .emit_v256_divpd_asm = x86_emit_v256_divpd_asm,
    .emit_v256_andps_asm = x86_emit_v256_andps_asm,
    .emit_v256_vorps_asm = x86_emit_v256_orps_asm,
    .emit_v256_vxorps_asm = x86_emit_v256_vxorps_asm,
    .emit_v256_zeroupper_asm = x86_emit_v256_zeroupper_asm,
    .emit_v512_load_asm = x86_emit_v512_load_asm,
    .emit_v512_store_asm = x86_emit_v512_store_asm,
    .emit_v512_addps_asm = x86_emit_v512_addps_asm,
    .emit_v512_subps_asm = x86_emit_v512_subps_asm,
    .emit_v512_mulps_asm = x86_emit_v512_mulps_asm,
    .emit_v512_divps_asm = x86_emit_v512_divps_asm,
    .emit_v512_addpd_asm = x86_emit_v512_addpd_asm,
    .emit_v512_subpd_asm = x86_emit_v512_subpd_asm,
    .emit_v512_mulpd_asm = x86_emit_v512_mulpd_asm,
    .emit_v512_divpd_asm = x86_emit_v512_divpd_asm,
    .emit_v512_andps_asm = x86_emit_v512_andps_asm,
    .emit_v512_vorps_asm = x86_emit_v512_orps_asm,
    .emit_v512_vxorps_asm = x86_emit_v512_vxorps_asm,
    .emit_kmov_to_k1_asm = x86_emit_kmov_to_k1_asm,
    .emit_kmov_from_k1_asm = x86_emit_kmov_from_k1_asm,
    .emit_kmov_to_k_asm = x86_emit_kmov_to_k_asm,
    .emit_kmov_from_k_asm = x86_emit_kmov_from_k_asm,
    .emit_kmov_kk_asm = x86_emit_kmov_kk_asm,
    .emit_kand_asm = x86_emit_kand_asm,
    .emit_kor_asm = x86_emit_kor_asm,
    .emit_kxor_asm = x86_emit_kxor_asm,
    .emit_knot_asm = x86_emit_knot_asm,
    .emit_kortest_asm = x86_emit_kortest_asm,
    .emit_v512_blendps_asm = x86_emit_v512_blendps_asm,
    .emit_v512_blendpd_asm = x86_emit_v512_blendpd_asm,
    .emit_v512_mask_load_asm = x86_emit_v512_mask_load_asm,
    .emit_v512_mask_store_asm = x86_emit_v512_mask_store_asm,
    .emit_v512_mask_addps_asm = x86_emit_v512_mask_addps_asm,
    .emit_v512_mask_subps_asm = x86_emit_v512_mask_subps_asm,
    .emit_v512_mask_mulps_asm = x86_emit_v512_mask_mulps_asm,
    .emit_v512_mask_divps_asm = x86_emit_v512_mask_divps_asm,
    .emit_v512_mask_addpd_asm = x86_emit_v512_mask_addpd_asm,
    .emit_v512_mask_subpd_asm = x86_emit_v512_mask_subpd_asm,
    .emit_v512_mask_mulpd_asm = x86_emit_v512_mask_mulpd_asm,
    .emit_v512_mask_divpd_asm = x86_emit_v512_mask_divpd_asm,
    .emit_v512_cmp_mask_asm = x86_emit_v512_cmp_mask_asm,
    .emit_v512_paddd_asm = x86_emit_v512_paddd_asm,
    .emit_v512_psubd_asm = x86_emit_v512_psubd_asm,
    .emit_v512_pmulld_asm = x86_emit_v512_pmulld_asm,
    .emit_v512_paddq_asm = x86_emit_v512_paddq_asm,
    .emit_v512_psubq_asm = x86_emit_v512_psubq_asm,
    .emit_v512_pmullq_asm = x86_emit_v512_pmullq_asm,
    .emit_v512_pandd_asm = x86_emit_v512_pandd_asm,
    .emit_v512_pord_asm = x86_emit_v512_pord_asm,
    .emit_v512_pxord_asm = x86_emit_v512_pxord_asm,
    .emit_v512_pandnd_asm = x86_emit_v512_pandnd_asm,
    .emit_v512_paddb_asm = x86_emit_v512_paddb_asm,
    .emit_v512_psubb_asm = x86_emit_v512_psubb_asm,
    .emit_v512_paddw_asm = x86_emit_v512_paddw_asm,
    .emit_v512_psubw_asm = x86_emit_v512_psubw_asm,
    .emit_v512_pmullw_asm = x86_emit_v512_pmullw_asm,
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
