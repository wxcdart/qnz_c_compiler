/*
 * x86-64 Assembly Emitter
 * ISA-specific assembly emission for GNU Assembler (GAS) syntax
 */

#include "zero_cc/isa/isa_backend.h"
#include "zero_cc/ir/ir.h"
#include "zero_cc/ir/regalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Hardware register names for x86-64 (indexed by hardware number) */
static const char *x86_hw_reg_names[] = {
    "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
    "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"
};

/* Get register name by hardware number */
static const char *x86_get_hw_reg_name(int phys_reg) {
    if (phys_reg >= 0 && phys_reg < 16) {
        return x86_hw_reg_names[phys_reg];
    }
    return "rax";  /* Default */
}

/* Get vector register name by index */
static const char *x86_get_vec_reg_name(int vec_reg_idx) {
    static const char *const x86_vec_names[] = {
        "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7",
        "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15"
    };
    if (vec_reg_idx < 0 || vec_reg_idx >= 16) {
        return "xmm0";  /* Default to xmm0 for invalid */
    }
    return x86_vec_names[vec_reg_idx];
}

/* Prologue: push rbp, mov rbp, rsp, sub rsp, save callee-saved */
static void x86_emit_prologue_asm(ZeroCCCompiler *c, size_t stack_size) {
    FILE *out = c->asm_out;

    fprintf(out, "\tpushq\t%%rbp\n");
    fprintf(out, "\tmovq\t%%rsp, %%rbp\n");
    /* Stack probing for frames > 4KB (Linux guard page): touch every page,
     * deepest first, before lowering RSP. RAX is scratch (caller-saved). */
    if (stack_size > 4096) {
        size_t top_page = (stack_size - 1) / 4096;
        for (size_t k = top_page; k >= 1; k--) {
            fprintf(out, "\tmovq\t%%rsp, %%rax\n");
            fprintf(out, "\tsubq\t$%zu, %%rax\n", k * 4096);
            fprintf(out, "\tcmpb\t$0, (%%rax)\n");
        }
    }
    fprintf(out, "\tsubq\t$%zu, %%rsp\n", stack_size);

    /* Save callee-saved registers (only those allocated by register allocator) */
    const int *callee_saved = BE(c)->conv->callee_saved_gpr;
    int count = BE(c)->conv->callee_saved_gpr_count;
    uint32_t mask = c->regalloc.used_callee_saved_gpr_mask;

    for (int i = 0; i < count; i++) {
        int reg = callee_saved[i];
        if (!((mask >> reg) & 1u)) continue;
        const char *reg_name = BE(c)->get_hw_reg_name(reg);
        int offset = BE(c)->conv->callee_save_slot_offsets[i];
        fprintf(out, "\tmovq\t%%%s, -%d(%%rbp)\n", reg_name, offset);
    }
}

/* Epilogue: restore callee-saved, leave, ret */
static void x86_emit_epilogue_asm(ZeroCCCompiler *c) {
    FILE *out = c->asm_out;

    /* Restore callee-saved registers (only those allocated by register allocator) */
    const int *callee_saved = BE(c)->conv->callee_saved_gpr;
    int count = BE(c)->conv->callee_saved_gpr_count;
    uint32_t mask = c->regalloc.used_callee_saved_gpr_mask;

    for (int i = 0; i < count; i++) {
        int reg = callee_saved[i];
        if (!((mask >> reg) & 1u)) continue;
        const char *reg_name = BE(c)->get_hw_reg_name(reg);
        int offset = BE(c)->conv->callee_save_slot_offsets[i];
        fprintf(out, "\tmovq\t-%d(%%rbp), %%%s\n", offset, reg_name);
    }

    fprintf(out, "\tleave\n");
    fprintf(out, "\tret\n");
}

/* MOV_IMM: mov $imm, dst */
static void x86_emit_mov_imm_asm(ZeroCCCompiler *c, int dst, int64_t imm, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    fprintf(out, "\tmovq\t$%ld,%%%s\n", (long)imm, dst_name);
}

/* MOV_REG: mov src, dst */
static void x86_emit_mov_reg_asm(ZeroCCCompiler *c, int dst, int src, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *src_name = BE(c)->get_hw_reg_name(src);
    fprintf(out, "\tmovq\t%%%s,%%%s\n", src_name, dst_name);
}

/* OS syscall (GAS text form). See x86_emit_syscall in x86_64.c for the
 * SysV register mapping (eax=number, rdi/rsi/rdx/r10/r8/r9=args 1-6) and
 * the parallel-move rationale (an argument temp may share a register with
 * another argument's syscall slot). Spill reloads use r11, same idiom as
 * x86_emit_call_asm. */
static void x86_emit_syscall_asm(ZeroCCCompiler *c, int arg_count, const int *arg_temps, FILE *out) {
    static const int sysv_syscall_regs[7] = {0, 7, 6, 2, 10, 8, 9};
    static const char *sysv_syscall_reg_names[7] = {"rax", "rdi", "rsi", "rdx", "r10", "r8", "r9"};
    int32_t spill_base_offset = c->regalloc.spill_base_offset;
    int n = (arg_count < 7) ? arg_count : 7;
    /* Pass 1: resolve + push sources needing a move. */
    for (int a = 0; a < n; a++) {
        int temp = arg_temps[a];
        const char *src_name;
        int sp = regalloc_get_spill_slot_for_temp(&c->regalloc, temp);
        if (sp >= 0) {
            int32_t off = spill_base_offset + (sp + 1) * 8;
            fprintf(out, "\tmovq\t%d(%%rbp),%%r11\n", -off);
            src_name = "r11";
        } else {
            src_name = BE(c)->get_hw_reg_name(regalloc_get_reg_for_temp(&c->regalloc, temp));
        }
        if (strcmp(src_name, sysv_syscall_reg_names[a]) != 0) {
            fprintf(out, "\tpushq\t%%%s\n", src_name);
        }
    }
    /* Pass 2: pop into the syscall registers (reverse order - LIFO). */
    for (int a = n - 1; a >= 0; a--) {
        int temp = arg_temps[a];
        int sp = regalloc_get_spill_slot_for_temp(&c->regalloc, temp);
        const char *src_name;
        if (sp >= 0) {
            src_name = "r11";
        } else {
            src_name = BE(c)->get_hw_reg_name(regalloc_get_reg_for_temp(&c->regalloc, temp));
        }
        if (strcmp(src_name, sysv_syscall_reg_names[a]) != 0) {
            fprintf(out, "\tpopq\t%%%s\n", sysv_syscall_reg_names[a]);
        }
    }
    fprintf(out, "\tsyscall\n");
}

/* LOAD: mov offset(base), dst */
static void x86_emit_load_asm(ZeroCCCompiler *c, int dst, int base, int32_t offset, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *base_name = BE(c)->get_hw_reg_name(base);
    fprintf(out, "\tmovq\t%d(%%%s),%%%s\n", offset, base_name, dst_name);
}

/* STORE: mov src, offset(base) */
static void x86_emit_store_asm(ZeroCCCompiler *c, int src, int base, int32_t offset, FILE *out) {
    const char *src_name = BE(c)->get_hw_reg_name(src);
    const char *base_name = BE(c)->get_hw_reg_name(base);
    fprintf(out, "\tmovq\t%%%s,%d(%%%s)\n", src_name, offset, base_name);
}

/* ADD: Handle src2 == dst or src1 == dst */
static void x86_emit_add_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *src1_name = BE(c)->get_hw_reg_name(src1);
    const char *src2_name = BE(c)->get_hw_reg_name(src2);

    if (dst == src2) {
        fprintf(out, "\taddq\t%%%s,%%%s\n", src1_name, dst_name);
    } else {
        if (dst != src1) {
            fprintf(out, "\tmovq\t%%%s,%%%s\n", src1_name, dst_name);
        }
        fprintf(out, "\taddq\t%%%s,%%%s\n", src2_name, dst_name);
    }
}

/* SUB: Handle src2 == dst or src1 == dst */
static void x86_emit_sub_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *src1_name = BE(c)->get_hw_reg_name(src1);
    const char *src2_name = BE(c)->get_hw_reg_name(src2);

    if (dst == src2) {
        fprintf(out, "\tsubq\t%%%s,%%%s\n", src1_name, dst_name);
    } else {
        if (dst != src1) {
            fprintf(out, "\tmovq\t%%%s,%%%s\n", src1_name, dst_name);
        }
        fprintf(out, "\tsubq\t%%%s,%%%s\n", src2_name, dst_name);
    }
}

/* MUL: two-operand imulq src, dst (avoids clobbering rax/rdx) */
static void x86_emit_mul_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *src1_name = BE(c)->get_hw_reg_name(src1);
    const char *src2_name = BE(c)->get_hw_reg_name(src2);

    if (dst == src2) {
        fprintf(out, "\timulq\t%%%s,%%%s\n", src1_name, dst_name);
    } else {
        if (dst != src1) {
            fprintf(out, "\tmovq\t%%%s,%%%s\n", src1_name, dst_name);
        }
        fprintf(out, "\timulq\t%%%s,%%%s\n", src2_name, dst_name);
    }
}

/* DIV: x86-64 uses rax:rdx for division */
static void x86_emit_div_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *src1_name = BE(c)->get_hw_reg_name(src1);
    const char *src2_name = BE(c)->get_hw_reg_name(src2);

    if (src1 != 0) {  /* RAX */
        fprintf(out, "\tmovq\t%%%s,%%rax\n", src1_name);
    }
    fprintf(out, "\tcqo\n");
    fprintf(out, "\tidivq\t%%%s\n", src2_name);
    if (dst != 0) {
        fprintf(out, "\tmovq\t%%rax,%%%s\n", dst_name);
    }
}

/* MOD: x86-64 uses rax:rdx for division, remainder is in rdx */
static void x86_emit_mod_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *src1_name = BE(c)->get_hw_reg_name(src1);
    const char *src2_name = BE(c)->get_hw_reg_name(src2);

    if (src1 != 0) {  /* RAX */
        fprintf(out, "\tmovq\t%%%s,%%rax\n", src1_name);
    }
    fprintf(out, "\tcqo\n");
    fprintf(out, "\tidivq\t%%%s\n", src2_name);
    if (dst != 2) {  /* RDX */
        fprintf(out, "\tmovq\t%%rdx,%%%s\n", dst_name);
    }
}

/* AND */
static void x86_emit_and_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *src1_name = BE(c)->get_hw_reg_name(src1);
    const char *src2_name = BE(c)->get_hw_reg_name(src2);

    if (dst != src1) {
        fprintf(out, "\tmovq\t%%%s,%%%s\n", src1_name, dst_name);
    }
    fprintf(out, "\tandq\t%%%s,%%%s\n", src2_name, dst_name);
}

/* OR */
static void x86_emit_or_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *src1_name = BE(c)->get_hw_reg_name(src1);
    const char *src2_name = BE(c)->get_hw_reg_name(src2);

    if (dst != src1) {
        fprintf(out, "\tmovq\t%%%s,%%%s\n", src1_name, dst_name);
    }
    fprintf(out, "\torq\t%%%s,%%%s\n", src2_name, dst_name);
}

/* XOR */
static void x86_emit_xor_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *src1_name = BE(c)->get_hw_reg_name(src1);
    const char *src2_name = BE(c)->get_hw_reg_name(src2);

    if (dst != src1) {
        fprintf(out, "\tmovq\t%%%s,%%%s\n", src1_name, dst_name);
    }
    fprintf(out, "\txorq\t%%%s,%%%s\n", src2_name, dst_name);
}

/* SHL */
static void x86_emit_shl_asm(ZeroCCCompiler *c, int dst, int src, int shift, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *src_name = BE(c)->get_hw_reg_name(src);
    const char *shift_name = BE(c)->get_hw_reg_name(shift);

    /* Handle RCX conflict (shift uses CL) */
    if (dst == 4) {  /* RCX */
        fprintf(out, "\tmovq\t%%%s,%%r10\n", src_name);
        fprintf(out, "\tmovq\t%%%s,%%rcx\n", shift_name);
        fprintf(out, "\tshlq\t%%cl,%%r10\n");
        fprintf(out, "\tmovq\t%%r10,%%%s\n", dst_name);
    } else {
        if (dst != src) {
            fprintf(out, "\tmovq\t%%%s,%%%s\n", src_name, dst_name);
        }
        fprintf(out, "\tmovq\t%%%s,%%rcx\n", shift_name);
        fprintf(out, "\tshlq\t%%cl,%%%s\n", dst_name);
    }
}

/* SHR */
static void x86_emit_shr_asm(ZeroCCCompiler *c, int dst, int src, int shift, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *src_name = BE(c)->get_hw_reg_name(src);
    const char *shift_name = BE(c)->get_hw_reg_name(shift);

    if (dst == 4) {  /* RCX */
        fprintf(out, "\tmovq\t%%%s,%%r10\n", src_name);
        fprintf(out, "\tmovq\t%%%s,%%rcx\n", shift_name);
        fprintf(out, "\tshrq\t%%cl,%%r10\n");
        fprintf(out, "\tmovq\t%%r10,%%%s\n", dst_name);
    } else {
        if (dst != src) {
            fprintf(out, "\tmovq\t%%%s,%%%s\n", src_name, dst_name);
        }
        fprintf(out, "\tmovq\t%%%s,%%rcx\n", shift_name);
        fprintf(out, "\tshrq\t%%cl,%%%s\n", dst_name);
    }
}

/* SAR */
static void x86_emit_sar_asm(ZeroCCCompiler *c, int dst, int src, int shift, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *src_name = BE(c)->get_hw_reg_name(src);
    const char *shift_name = BE(c)->get_hw_reg_name(shift);

    if (dst == 4) {  /* RCX */
        fprintf(out, "\tmovq\t%%%s,%%r10\n", src_name);
        fprintf(out, "\tmovq\t%%%s,%%rcx\n", shift_name);
        fprintf(out, "\tsarq\t%%cl,%%r10\n");
        fprintf(out, "\tmovq\t%%r10,%%%s\n", dst_name);
    } else {
        if (dst != src) {
            fprintf(out, "\tmovq\t%%%s,%%%s\n", src_name, dst_name);
        }
        fprintf(out, "\tmovq\t%%%s,%%rcx\n", shift_name);
        fprintf(out, "\tsarq\t%%cl,%%%s\n", dst_name);
    }
}

/* SEXT/ZEXT (in-place width extension, dst == src physical register).
 * Mirrors the binary emitter's encodings (see x86_emit_extend). */
static const char *x86_reg8_name(int r) {
    static const char *names[] = {"al","cl","dl","bl","spl","bpl","sil","dil",
                                  "r8b","r9b","r10b","r11b","r12b","r13b","r14b","r15b"};
    return (r >= 0 && r < 16) ? names[r] : "al";
}
static const char *x86_reg16_name(int r) {
    static const char *names[] = {"ax","cx","dx","bx","sp","bp","si","di",
                                  "r8w","r9w","r10w","r11w","r12w","r13w","r14w","r15w"};
    return (r >= 0 && r < 16) ? names[r] : "ax";
}
static const char *x86_reg32_name(int r) {
    static const char *names[] = {"eax","ecx","edx","ebx","esp","ebp","esi","edi",
                                  "r8d","r9d","r10d","r11d","r12d","r13d","r14d","r15d"};
    return (r >= 0 && r < 16) ? names[r] : "eax";
}

/* Width extension. src is the value register, dst the destination;
 * they coincide only for in-place extensions. */
static void x86_emit_extend_asm(ZeroCCCompiler *c, int dst, int src, int width, int sign, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    if (width == 4) {
        if (sign) fprintf(out, "\tmovslq\t%%%s,%%%s\n", x86_reg32_name(src), dst_name);
        else fprintf(out, "\tmovl\t%%%s,%%%s\n", x86_reg32_name(src), x86_reg32_name(dst));
        return;
    }
    if (width == 1) {
        if (sign) {
            fprintf(out, "\tmovsbl\t%%%s,%%%s\n", x86_reg8_name(src), x86_reg32_name(dst));
            fprintf(out, "\tmovslq\t%%%s,%%%s\n", x86_reg32_name(dst), dst_name);
        } else {
            /* No 64-bit-destination form of movzbl is accepted by GAS; use a
             * 32-bit destination (which zeroes the upper half). */
            fprintf(out, "\tmovzbl\t%%%s,%%%s\n", x86_reg8_name(src), x86_reg32_name(dst));
            fprintf(out, "\tmovl\t%%%s,%%%s\n", x86_reg32_name(dst), x86_reg32_name(dst));
        }
        return;
    }
    /* width 2 */
    if (!sign) {
        /* No 64-bit-destination form exists for movzwl; use two 32-bit ops
         * (the 32-bit write clears the upper half). */
        fprintf(out, "\tmovzwl\t%%%s,%%%s\n", x86_reg16_name(src), x86_reg32_name(dst));
        fprintf(out, "\tmovl\t%%%s,%%%s\n", x86_reg32_name(dst), x86_reg32_name(dst));
        return;
    }
    fprintf(out, "\tmovswl\t%%%s,%%%s\n", x86_reg16_name(src), x86_reg32_name(dst));
    fprintf(out, "\tmovslq\t%%%s,%%%s\n", x86_reg32_name(dst), dst_name);
}

/* CMP */
static void x86_emit_cmp_asm(ZeroCCCompiler *c, int src1, int src2, FILE *out) {
    const char *src1_name = BE(c)->get_hw_reg_name(src1);
    const char *src2_name = BE(c)->get_hw_reg_name(src2);
    fprintf(out, "\tcmpq\t%%%s,%%%s\n", src2_name, src1_name);
}

/* COND_SET: setcc into the low byte of the destination register itself,
 * then zero-extend. Using a fixed scratch (e.g. %al) corrupts any live
 * value held in the scratch register (e.g. a LICM-hoisted operand in RAX). */
static void x86_emit_cond_set_asm(ZeroCCCompiler *c, int dst, int cond, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *dst8_name = x86_reg8_name(dst);
    const char *cc[] = {"l", "le", "g", "ge", "e", "ne", "b", "be", "a", "ae"};
    if (cond < 0 || cond > 9) return;
    fprintf(out, "\tset%s\t%%%s\n", cc[cond], dst8_name);
    fprintf(out, "\tmovzbq\t%%%s,%%%s\n", dst8_name, dst_name);
}

/* NEG */
static void x86_emit_neg_asm(ZeroCCCompiler *c, int dst, int src, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *src_name = BE(c)->get_hw_reg_name(src);
    fprintf(out, "\tmovq\t%%%s,%%%s\n", src_name, dst_name);
    fprintf(out, "\tnegq\t%%%s\n", dst_name);
}

/* ADD_IMM: addq $imm, dst (Sprint 40: was missing from -S) */
static void x86_emit_add_imm_asm(ZeroCCCompiler *c, int dst, int64_t imm, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    fprintf(out, "\taddq\t$%ld,%%%s\n", (long)imm, dst_name);
}

/* SUB_IMM: subq $imm, dst */
static void x86_emit_sub_imm_asm(ZeroCCCompiler *c, int dst, int64_t imm, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    fprintf(out, "\tsubq\t$%ld,%%%s\n", (long)imm, dst_name);
}

/* INC: incq dst */
static void x86_emit_inc_asm(ZeroCCCompiler *c, int dst, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    fprintf(out, "\tincq\t%%%s\n", dst_name);
}

/* DEC: decq dst */
static void x86_emit_dec_asm(ZeroCCCompiler *c, int dst, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    fprintf(out, "\tdecq\t%%%s\n", dst_name);
}

/* NOT: not dst (in-place bitwise NOT; copy src first when distinct,
 * mirroring the MC emitter) */
static void x86_emit_not_asm(ZeroCCCompiler *c, int dst, int src, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    if (dst != src) {
        const char *src_name = BE(c)->get_hw_reg_name(src);
        fprintf(out, "\tmovq\t%%%s,%%%s\n", src_name, dst_name);
    }
    fprintf(out, "\tnotq\t%%%s\n", dst_name);
}

/* LEA */
static void x86_emit_lea_asm(ZeroCCCompiler *c, int dst, int base, int32_t offset, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *base_name = BE(c)->get_hw_reg_name(base);
    fprintf(out, "\tleaq\t%d(%%%s),%%%s\n", offset, base_name, dst_name);
}

/* DEREF */
static void x86_emit_deref_asm(ZeroCCCompiler *c, int dst, int src, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *src_name = BE(c)->get_hw_reg_name(src);
    fprintf(out, "\tmovq\t(%%%s),%%%s\n", src_name, dst_name);
}

/* CALL - Full x86-64 ABI compliance */
static void x86_emit_call_asm(ZeroCCCompiler *c, const char *func_name, int arg_count, int *arg_temps, int *is_fp, FILE *out) {
    /* x86-64 System V ABI argument registers */
    static const char *int_arg_regs[] = {"rdi", "rsi", "rdx", "rcx", "r8", "r9"};
    static const char *fp_arg_regs[] = {"xmm0", "xmm1", "xmm2", "xmm3",
                                         "xmm4", "xmm5", "xmm6", "xmm7"};

    int int_arg_idx = 0;
    int fp_arg_idx = 0;
    int32_t spill_base_offset = c->regalloc.spill_base_offset;

    /* Check if this is an indirect call (func_name == NULL) */
    const char *indirect_reg_name = NULL;
    for (int i = 0; i < arg_count; i++) {
        if (is_fp && is_fp[i] == 2) {
            int temp = arg_temps[i];
            int sp = regalloc_get_spill_slot_for_temp(&c->regalloc, temp);
            if (sp >= 0) {
                int32_t off = spill_base_offset + (sp + 1) * 8;
                fprintf(out, "\tmovq\t%d(%%rbp),%%r11\n", -off);
                indirect_reg_name = "r11";
            } else {
                int phys_reg = regalloc_get_reg_for_temp(&c->regalloc, temp);
                indirect_reg_name = BE(c)->get_hw_reg_name(phys_reg);
                /* Save to r11 so argument register moves don't clobber the callee pointer */
                fprintf(out, "\tmovq\t%%%s,%%r11\n", indirect_reg_name);
            }
        }
    }

    /* Handle FP args first */
    for (int i = 0; i < arg_count; i++) {
        int temp = arg_temps[i];
        int phys_reg = regalloc_get_reg_for_temp(&c->regalloc, temp);
        const char *reg_name = (phys_reg >= 0 && phys_reg < BE(c)->regs->num_fp_regs)
                                   ? BE(c)->regs->fp_regs[phys_reg]
                                   : BE(c)->get_vec_reg_name(phys_reg);
        int arg_is_fp = is_fp ? is_fp[i] : 0;
        if (arg_is_fp == 1 && fp_arg_idx < 8) {
            fprintf(out, "\tmovsd\t%%%s, %%%s\n", reg_name, fp_arg_regs[fp_arg_idx]);
            fp_arg_idx++;
        }
    }

    /* Collect integer argument temps and emit safe parallel move via push/pop */
    int n_int_args = 0;
    int int_arg_temps[IR_CALL_MAX_ARGS];
    int total_int_count = 0;
    (void)int_arg_idx;

    for (int i = 0; i < arg_count; i++) {
        int arg_is_fp = is_fp ? is_fp[i] : 0;
        if (arg_is_fp == 0) {
            int temp = arg_temps[i];
            int_arg_temps[total_int_count++] = temp;
            if (n_int_args < 6) {
                n_int_args++;
            }
        }
    }

    /* Arguments beyond 6 are passed on the stack: push in reverse order, maintain 16-byte alignment */
    int n_stack = (total_int_count > 6) ? total_int_count - 6 : 0;
    int stack_pad = (n_stack & 1) ? 8 : 0;
    if (n_stack > 0) {
        if (stack_pad) fprintf(out, "\tsubq\t$8, %%rsp\n");
        for (int a = total_int_count - 1; a >= 6; a--) {
            int sp = regalloc_get_spill_slot_for_temp(&c->regalloc, int_arg_temps[a]);
            if (sp >= 0) {
                int32_t off = spill_base_offset + (sp + 1) * 8;
                fprintf(out, "\tmovq\t%d(%%rbp),%%r10\n", -off);
                fprintf(out, "\tpushq\t%%r10\n");
            } else {
                int phys_reg = regalloc_get_reg_for_temp(&c->regalloc, int_arg_temps[a]);
                fprintf(out, "\tpushq\t%%%s\n", BE(c)->get_hw_reg_name(phys_reg));
            }
        }
    }

    /* Push all src integer registers to stack (reloading spilled temps via %r10) */
    for (int i = 0; i < n_int_args; i++) {
        int sp = regalloc_get_spill_slot_for_temp(&c->regalloc, int_arg_temps[i]);
        if (sp >= 0) {
            int32_t off = spill_base_offset + (sp + 1) * 8;
            fprintf(out, "\tmovq\t%d(%%rbp),%%r10\n", -off);
            fprintf(out, "\tpushq\t%%r10\n");
        } else {
            int phys_reg = regalloc_get_reg_for_temp(&c->regalloc, int_arg_temps[i]);
            fprintf(out, "\tpushq\t%%%s\n", BE(c)->get_hw_reg_name(phys_reg));
        }
    }
    /* Pop in reverse into destination ABI registers */
    for (int i = n_int_args - 1; i >= 0; i--) {
        fprintf(out, "\tpopq\t%%%s\n", int_arg_regs[i]);
    }

    if (indirect_reg_name) {
        fprintf(out, "\tcall\t*%%r11\n");
    } else {
        fprintf(out, "\tcall\t%s\n", func_name);
    }

    /* Clean up stack arguments after call */
    if (n_stack > 0) {
        int total_cleanup = n_stack * 8 + stack_pad;
        fprintf(out, "\taddq\t$%d, %%rsp\n", total_cleanup);
    }
}

/* RET */
static void x86_emit_ret_asm(ZeroCCCompiler *, FILE *out) {
    fprintf(out, "\tret\n");
}

/* LABEL */
static void x86_emit_label_asm(ZeroCCCompiler *, const char *label, FILE *out) {
    fprintf(out, "%s:\n", label);
}

/* JMP */
static void x86_emit_jmp_asm(ZeroCCCompiler *, const char *label, FILE *out) {
    fprintf(out, "\tjmp\t%s\n", label);
}

/* JEQ */
static void x86_emit_jeq_asm(ZeroCCCompiler *, const char *label, FILE *out) {
    fprintf(out, "\tje\t%s\n", label);
}

/* JNE */
static void x86_emit_jne_asm(ZeroCCCompiler *, const char *label, FILE *out) {
    fprintf(out, "\tjne\t%s\n", label);
}

/* JLT */
static void x86_emit_jlt_asm(ZeroCCCompiler *, const char *label, FILE *out) {
    fprintf(out, "\tjl\t%s\n", label);
}

/* JLE */
static void x86_emit_jle_asm(ZeroCCCompiler *, const char *label, FILE *out) {
    fprintf(out, "\tjle\t%s\n", label);
}

/* JGT */
static void x86_emit_jgt_asm(ZeroCCCompiler *, const char *label, FILE *out) {
    fprintf(out, "\tjg\t%s\n", label);
}

/* JGE */
static void x86_emit_jge_asm(ZeroCCCompiler *, const char *label, FILE *out) {
    fprintf(out, "\tjge\t%s\n", label);
}

/* FP LOAD: movsd offset(%base), %xmm */
static void x86_emit_load_fp_asm(ZeroCCCompiler *c, int dst, int base, int32_t offset, FILE *out) {
    const char *dst_name = BE(c)->regs->fp_regs[dst];
    const char *base_name = BE(c)->get_hw_reg_name(base);
    fprintf(out, "\tmovsd\t%d(%%%s),%%%s\n", offset, base_name, dst_name);
}

/* FP STORE: movsd %xmm, offset(%base) */
static void x86_emit_store_fp_asm(ZeroCCCompiler *c, int src, int base, int32_t offset, FILE *out) {
    const char *src_name = BE(c)->regs->fp_regs[src];
    const char *base_name = BE(c)->get_hw_reg_name(base);
    fprintf(out, "\tmovsd\t%%%s,%d(%%%s)\n", src_name, offset, base_name);
}

/* FP ADD - x86's addsd is 2-operand (dst <- dst + src2); materialize src1
 * into dst first, with alias handling (dst may equal src1 or src2). */
static void x86_emit_fp_add_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->regs->fp_regs[dst];
    const char *src1_name = BE(c)->regs->fp_regs[src1];
    const char *src2_name = BE(c)->regs->fp_regs[src2];
    if (dst == src1) {
        fprintf(out, "\taddsd\t%%%s,%%%s\n", src2_name, dst_name);
    } else if (dst == src2) {
        /* commutative: dst = src2 + src1 in place */
        fprintf(out, "\taddsd\t%%%s,%%%s\n", src1_name, dst_name);
    } else {
        fprintf(out, "\tmovsd\t%%%s,%%%s\n", src1_name, dst_name);
        fprintf(out, "\taddsd\t%%%s,%%%s\n", src2_name, dst_name);
    }
}

/* FP SUB - non-commutative; when dst aliases src2, stage src1 through the
 * FP scratch (xmm15, outside the allocatable pool). */
static void x86_emit_fp_sub_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->regs->fp_regs[dst];
    const char *src1_name = BE(c)->regs->fp_regs[src1];
    const char *src2_name = BE(c)->regs->fp_regs[src2];
    const char *scratch = BE(c)->regs->fp_regs[15];
    if (dst == src1) {
        fprintf(out, "\tsubsd\t%%%s,%%%s\n", src2_name, dst_name);
    } else if (dst == src2) {
        fprintf(out, "\tmovsd\t%%%s,%%%s\n", src1_name, scratch);
        fprintf(out, "\tsubsd\t%%%s,%%%s\n", dst_name, scratch);
        fprintf(out, "\tmovsd\t%%%s,%%%s\n", scratch, dst_name);
    } else {
        fprintf(out, "\tmovsd\t%%%s,%%%s\n", src1_name, dst_name);
        fprintf(out, "\tsubsd\t%%%s,%%%s\n", src2_name, dst_name);
    }
}

/* FP MUL (commutative, same pattern as ADD) */
static void x86_emit_fp_mul_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->regs->fp_regs[dst];
    const char *src1_name = BE(c)->regs->fp_regs[src1];
    const char *src2_name = BE(c)->regs->fp_regs[src2];
    if (dst == src1) {
        fprintf(out, "\tmulsd\t%%%s,%%%s\n", src2_name, dst_name);
    } else if (dst == src2) {
        fprintf(out, "\tmulsd\t%%%s,%%%s\n", src1_name, dst_name);
    } else {
        fprintf(out, "\tmovsd\t%%%s,%%%s\n", src1_name, dst_name);
        fprintf(out, "\tmulsd\t%%%s,%%%s\n", src2_name, dst_name);
    }
}

/* FP DIV (non-commutative, same pattern as SUB) */
static void x86_emit_fp_div_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    const char *dst_name = BE(c)->regs->fp_regs[dst];
    const char *src1_name = BE(c)->regs->fp_regs[src1];
    const char *src2_name = BE(c)->regs->fp_regs[src2];
    const char *scratch = BE(c)->regs->fp_regs[15];
    if (dst == src1) {
        fprintf(out, "\tdivsd\t%%%s,%%%s\n", src2_name, dst_name);
    } else if (dst == src2) {
        fprintf(out, "\tmovsd\t%%%s,%%%s\n", src1_name, scratch);
        fprintf(out, "\tdivsd\t%%%s,%%%s\n", dst_name, scratch);
        fprintf(out, "\tmovsd\t%%%s,%%%s\n", scratch, dst_name);
    } else {
        fprintf(out, "\tmovsd\t%%%s,%%%s\n", src1_name, dst_name);
        fprintf(out, "\tdivsd\t%%%s,%%%s\n", src2_name, dst_name);
    }
}

/* FP compare: ucomisd src1, src2 (reg=src1, rm=src2 - matches the MC
 * emitter's operand order). The following OP_COND_SET reuses the standard
 * setcc path; ucomisd sets the same flags as cmp for ordered values. */
static void x86_emit_fp_cmp_asm(ZeroCCCompiler *c, int src1, int src2, FILE *out) {
    fprintf(out, "\tucomisd\t%%%s,%%%s\n",
            BE(c)->regs->fp_regs[src2], BE(c)->regs->fp_regs[src1]);
}

/* FP MOV */
static void x86_emit_fp_mov_asm(ZeroCCCompiler *c, int dst, int src, FILE *out) {
    const char *dst_name = BE(c)->regs->fp_regs[dst];
    const char *src_name = BE(c)->regs->fp_regs[src];
    fprintf(out, "\tmovsd\t%%%s,%%%s\n", src_name, dst_name);
}

/* FP CVTSF: cvtsi2sd %reg, %xmm (int -> double) */
static void x86_emit_fp_cvtsf_asm(ZeroCCCompiler *c, int fp_dst, int int_src, FILE *out) {
    const char *dst_name = BE(c)->regs->fp_regs[fp_dst];
    const char *src_name = BE(c)->get_hw_reg_name(int_src);
    fprintf(out, "\tcvtsi2sd\t%%%s,%%%s\n", src_name, dst_name);
}

/* FP CVTSI: cvttsd2si %xmm, %reg (double -> int, truncate) */
static void x86_emit_fp_cvtsi_asm(ZeroCCCompiler *c, int int_dst, int fp_src, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(int_dst);
    const char *src_name = BE(c)->regs->fp_regs[fp_src];
    fprintf(out, "\tcvttsd2si\t%%%s,%%%s\n", src_name, dst_name);
}

/* VLA_ALLOC */
static void x86_emit_vla_alloc_asm(ZeroCCCompiler *c, int dst, int size, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *size_name = BE(c)->get_hw_reg_name(size);
    fprintf(out, "\tmovq\t%%%s,%%rax\n", size_name);
    fprintf(out, "\tandq\t$-16,%%rax\n");
    fprintf(out, "\tsubq\t%%rax,%%rsp\n");
    fprintf(out, "\tmovq\t%%rsp,%%%s\n", dst_name);
}

/* PARAM_SAVE_INT: mov [rbp - offset], reg */
static void x86_emit_param_save_int_asm(ZeroCCCompiler *, int reg_idx, int32_t offset, FILE *out) {
    static const char *arg_regs[6] = {"rdi", "rsi", "rdx", "rcx", "r8", "r9"};
    if (reg_idx >= 0 && reg_idx < 6) {
        fprintf(out, "\tmovq\t%%%s,%d(%%rbp)\n", arg_regs[reg_idx], -offset);
    }
}

/* PARAM_SAVE_FP: movsd [rbp - offset], xmmN */
static void x86_emit_param_save_fp_asm(ZeroCCCompiler *, int reg_idx, int32_t offset, FILE *out) {
    if (reg_idx >= 0 && reg_idx < 8) {
        fprintf(out, "\tmovsd\t%%xmm%d,%d(%%rbp)\n", reg_idx, -offset);
    }
}

/* VARIADIC_SPILL: spill remaining integer registers for va_start */
static void x86_emit_variadic_spill_asm(ZeroCCCompiler *, int param_count, int32_t base_offset, FILE *out) {
    static const char *arg_regs[6] = {"rdi", "rsi", "rdx", "rcx", "r8", "r9"};
    for (int i = 0; i < param_count && i < 6; i++) {
        fprintf(out, "\tmovq\t%%%s,%d(%%rbp)\n", arg_regs[i], base_offset - i * 8);
    }
}

/* =========================================================================
 * Vector Instruction Emission (SSE2)
 * ========================================================================= */

/* Vector load (128-bit unaligned) - movups */
static void x86_emit_vload_asm(ZeroCCCompiler *c, int dst, int base, int32_t offset, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    fprintf(out, "\tmovups\t%d(%%%s),%%%s\n", offset, BE(c)->get_hw_reg_name(base), dst_name);
}

/* Vector store (128-bit unaligned) - movups */
static void x86_emit_vstore_asm(ZeroCCCompiler *c, int src, int base, int32_t offset, FILE *out) {
    const char *src_name = BE(c)->get_vec_reg_name(src);
    fprintf(out, "\tmovups\t%%%s,%d(%%%s)\n", src_name, offset, BE(c)->get_hw_reg_name(base));
}

/* Vector scalar add - addss */
static void x86_emit_vaddss_asm(ZeroCCCompiler *c, int dst, int, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src2_name = BE(c)->get_vec_reg_name(src2);
    fprintf(out, "\taddss\t%%%s,%%%s\n", src2_name, dst_name);
}

/* Vector scalar sub - subss */
static void x86_emit_vsubss_asm(ZeroCCCompiler *c, int dst, int, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src2_name = BE(c)->get_vec_reg_name(src2);
    fprintf(out, "\tsubss\t%%%s,%%%s\n", src2_name, dst_name);
}

/* Vector scalar mul - mulss */
static void x86_emit_vmulss_asm(ZeroCCCompiler *c, int dst, int, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src2_name = BE(c)->get_vec_reg_name(src2);
    fprintf(out, "\tmulss\t%%%s,%%%s\n", src2_name, dst_name);
}

/* Vector scalar div - divss */
static void x86_emit_vdivss_asm(ZeroCCCompiler *c, int dst, int, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src2_name = BE(c)->get_vec_reg_name(src2);
    fprintf(out, "\tdivss\t%%%s,%%%s\n", src2_name, dst_name);
}

/* Vector packed single add - addps */
static void x86_emit_vaddps_asm(ZeroCCCompiler *c, int dst, int, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src2_name = BE(c)->get_vec_reg_name(src2);
    fprintf(out, "\taddps\t%%%s,%%%s\n", src2_name, dst_name);
}

/* Vector packed single sub - subps */
static void x86_emit_vsubps_asm(ZeroCCCompiler *c, int dst, int, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src2_name = BE(c)->get_vec_reg_name(src2);
    fprintf(out, "\tsubps\t%%%s,%%%s\n", src2_name, dst_name);
}

/* Vector packed single mul - mulps */
static void x86_emit_vmulps_asm(ZeroCCCompiler *c, int dst, int, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src2_name = BE(c)->get_vec_reg_name(src2);
    fprintf(out, "\tmulps\t%%%s,%%%s\n", src2_name, dst_name);
}

/* Vector packed single div - divps */
static void x86_emit_vdivps_asm(ZeroCCCompiler *c, int dst, int, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src2_name = BE(c)->get_vec_reg_name(src2);
    fprintf(out, "\tdivps\t%%%s,%%%s\n", src2_name, dst_name);
}

/* Vector scalar double add - addsd */
static void x86_emit_vaddsd_asm(ZeroCCCompiler *c, int dst, int, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src2_name = BE(c)->get_vec_reg_name(src2);
    fprintf(out, "\taddsd\t%%%s,%%%s\n", src2_name, dst_name);
}

/* Vector scalar double sub - subsd */
static void x86_emit_vsubsd_asm(ZeroCCCompiler *c, int dst, int, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src2_name = BE(c)->get_vec_reg_name(src2);
    fprintf(out, "\tsubsd\t%%%s,%%%s\n", src2_name, dst_name);
}

/* Vector scalar double mul - mulsd */
static void x86_emit_vmulsd_asm(ZeroCCCompiler *c, int dst, int, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src2_name = BE(c)->get_vec_reg_name(src2);
    fprintf(out, "\tmulsd\t%%%s,%%%s\n", src2_name, dst_name);
}

/* Vector scalar double div - divsd */
static void x86_emit_vdivsd_asm(ZeroCCCompiler *c, int dst, int, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src2_name = BE(c)->get_vec_reg_name(src2);
    fprintf(out, "\tdivsd\t%%%s,%%%s\n", src2_name, dst_name);
}

/* Vector packed double add - addpd */
static void x86_emit_vaddpd_asm(ZeroCCCompiler *c, int dst, int, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src2_name = BE(c)->get_vec_reg_name(src2);
    fprintf(out, "\taddpd\t%%%s,%%%s\n", src2_name, dst_name);
}

/* Vector packed double sub - subpd */
static void x86_emit_vsubpd_asm(ZeroCCCompiler *c, int dst, int, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src2_name = BE(c)->get_vec_reg_name(src2);
    fprintf(out, "\tsubpd\t%%%s,%%%s\n", src2_name, dst_name);
}

/* Vector packed double mul - mulpd */
static void x86_emit_vmulpd_asm(ZeroCCCompiler *c, int dst, int, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src2_name = BE(c)->get_vec_reg_name(src2);
    fprintf(out, "\tmulpd\t%%%s,%%%s\n", src2_name, dst_name);
}

/* Vector packed double div - divpd */
static void x86_emit_vdivpd_asm(ZeroCCCompiler *c, int dst, int, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src2_name = BE(c)->get_vec_reg_name(src2);
    fprintf(out, "\tdivpd\t%%%s,%%%s\n", src2_name, dst_name);
}

/* Vector move scalar - movss */
static void x86_emit_vmovss_asm(ZeroCCCompiler *c, int dst, int src, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src_name = BE(c)->get_vec_reg_name(src);
    fprintf(out, "\tmovss\t%%%s,%%%s\n", src_name, dst_name);
}

/* Vector scalar double move - movsd */
static void x86_emit_vmovsd_asm(ZeroCCCompiler *c, int dst, int src, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src_name = BE(c)->get_vec_reg_name(src);
    fprintf(out, "\tmovsd\t%%%s,%%%s\n", src_name, dst_name);
}

/* Vector move aligned packed single - movaps */
static void x86_emit_vmovaps_asm(ZeroCCCompiler *c, int dst, int src, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src_name = BE(c)->get_vec_reg_name(src);
    fprintf(out, "\tmovaps\t%%%s,%%%s\n", src_name, dst_name);
}

/* Vector move aligned packed double - movapd */
static void x86_emit_vmovapd_asm(ZeroCCCompiler *c, int dst, int src, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src_name = BE(c)->get_vec_reg_name(src);
    fprintf(out, "\tmovapd\t%%%s,%%%s\n", src_name, dst_name);
}

/* Vector packed 32-bit int add - paddd */
static void x86_emit_vpaddd_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    if (dst == src2 && dst != src1) {
        int tmp = src1; src1 = src2; src2 = tmp;
    }
    if (dst != src1) {
        x86_emit_vmovaps_asm(c, dst, src1, out);
    }
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src2_name = BE(c)->get_vec_reg_name(src2);
    fprintf(out, "\tpaddd\t%%%s,%%%s\n", src2_name, dst_name);
}

/* Vector packed 32-bit int sub - psubd */
static void x86_emit_vpsubd_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    if (dst != src1) {
        x86_emit_vmovaps_asm(c, dst, src1, out);
    }
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src2_name = BE(c)->get_vec_reg_name(src2);
    fprintf(out, "\tpsubd\t%%%s,%%%s\n", src2_name, dst_name);
}

/* Vector packed 32-bit int mul - pmulld */
static void x86_emit_vpmulld_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    if (dst == src2 && dst != src1) {
        int tmp = src1; src1 = src2; src2 = tmp;
    }
    if (dst != src1) {
        x86_emit_vmovaps_asm(c, dst, src1, out);
    }
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src2_name = BE(c)->get_vec_reg_name(src2);
    fprintf(out, "\tpmulld\t%%%s,%%%s\n", src2_name, dst_name);
}

/* Vector bitwise and - andps */
static void x86_emit_vandps_asm(ZeroCCCompiler *c, int dst, int, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src2_name = BE(c)->get_vec_reg_name(src2);
    fprintf(out, "\tandps\t%%%s,%%%s\n", src2_name, dst_name);
}

/* Vector bitwise or - orps */
static void x86_emit_vorps_asm(ZeroCCCompiler *c, int dst, int, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src2_name = BE(c)->get_vec_reg_name(src2);
    fprintf(out, "\torps\t%%%s,%%%s\n", src2_name, dst_name);
}

/* Vector bitwise xor - xorps */
static void x86_emit_vxorps_asm(ZeroCCCompiler *c, int dst, int, int src2, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src2_name = BE(c)->get_vec_reg_name(src2);
    fprintf(out, "\txorps\t%%%s,%%%s\n", src2_name, dst_name);
}

/* Vector shuffle - shufps */
static void x86_emit_vshufps_asm(ZeroCCCompiler *c, int dst, int, int src2, int imm, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src2_name = BE(c)->get_vec_reg_name(src2);
    fprintf(out, "\tshufps\t$%d,%%%s,%%%s\n", imm, src2_name, dst_name);
}

/* Convert scalar int to scalar single - cvtsi2ss */
static void x86_emit_vcvt_si2ss_asm(ZeroCCCompiler *c, int dst, int src, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src_name = BE(c)->get_hw_reg_name(src);
    fprintf(out, "\tcvtsi2ss\t%%%s,%%%s\n", src_name, dst_name);
}

/* Convert scalar single to scalar int - cvtss2si */
static void x86_emit_vcvt_ss2si_asm(ZeroCCCompiler *c, int dst, int src, FILE *out) {
    const char *dst_name = BE(c)->get_hw_reg_name(dst);
    const char *src_name = BE(c)->get_vec_reg_name(src);
    fprintf(out, "\tcvtss2si\t%%%s,%%%s\n", src_name, dst_name);
}

/* Vector compare scalar single - cmpss */
static void x86_emit_vcmpss_asm(ZeroCCCompiler *c, int dst, int, int src2, int pred, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src2_name = BE(c)->get_vec_reg_name(src2);
    fprintf(out, "\tcmpss\t$%d,%%%s,%%%s\n", pred, src2_name, dst_name);
}

/* Vector scalar double comparison - cmpsd */
static void x86_emit_vcmpsd_asm(ZeroCCCompiler *c, int dst, int, int src2, int pred, FILE *out) {
    const char *dst_name = BE(c)->get_vec_reg_name(dst);
    const char *src2_name = BE(c)->get_vec_reg_name(src2);
    fprintf(out, "\tcmpsd\t$%d,%%%s,%%%s\n", pred, src2_name, dst_name);
}

/* Initialize x86-64 assembly emission functions in backend */
void x86_init_asm_emission(ZeroCCISABackend *be) {
    be->emit_prologue_asm = x86_emit_prologue_asm;
    be->emit_epilogue_asm = x86_emit_epilogue_asm;
    be->get_hw_reg_name = x86_get_hw_reg_name;
    be->get_vec_reg_name = x86_get_vec_reg_name;

    be->emit_mov_imm_asm = x86_emit_mov_imm_asm;
    be->emit_mov_reg_asm = x86_emit_mov_reg_asm;
    be->emit_syscall_asm = x86_emit_syscall_asm;
    be->emit_load_asm = x86_emit_load_asm;
    be->emit_store_asm = x86_emit_store_asm;
    be->emit_add_asm = x86_emit_add_asm;
    be->emit_sub_asm = x86_emit_sub_asm;
    be->emit_mul_asm = x86_emit_mul_asm;
    be->emit_div_asm = x86_emit_div_asm;
    be->emit_mod_asm = x86_emit_mod_asm;
    be->emit_and_asm = x86_emit_and_asm;
    be->emit_or_asm = x86_emit_or_asm;
    be->emit_xor_asm = x86_emit_xor_asm;
    be->emit_shl_asm = x86_emit_shl_asm;
    be->emit_shr_asm = x86_emit_shr_asm;
    be->emit_sar_asm = x86_emit_sar_asm;
    be->emit_extend_asm = x86_emit_extend_asm;
    be->emit_cmp_asm = x86_emit_cmp_asm;
    be->emit_cond_set_asm = x86_emit_cond_set_asm;
    be->emit_neg_asm = x86_emit_neg_asm;
    be->emit_add_imm_asm = x86_emit_add_imm_asm;
    be->emit_sub_imm_asm = x86_emit_sub_imm_asm;
    be->emit_inc_asm = x86_emit_inc_asm;
    be->emit_dec_asm = x86_emit_dec_asm;
    be->emit_not_asm = x86_emit_not_asm;
    be->emit_lea_asm = x86_emit_lea_asm;
    be->emit_deref_asm = x86_emit_deref_asm;
    be->emit_call_asm = x86_emit_call_asm;
    be->emit_ret_asm = x86_emit_ret_asm;
    be->emit_label_asm = x86_emit_label_asm;
    be->emit_jmp_asm = x86_emit_jmp_asm;
    be->emit_jeq_asm = x86_emit_jeq_asm;
    be->emit_jne_asm = x86_emit_jne_asm;
    be->emit_jlt_asm = x86_emit_jlt_asm;
    be->emit_jle_asm = x86_emit_jle_asm;
    be->emit_jgt_asm = x86_emit_jgt_asm;
    be->emit_jge_asm = x86_emit_jge_asm;
    be->emit_load_fp_asm = x86_emit_load_fp_asm;
    be->emit_store_fp_asm = x86_emit_store_fp_asm;
    be->emit_fp_add_asm = x86_emit_fp_add_asm;
    be->emit_fp_sub_asm = x86_emit_fp_sub_asm;
    be->emit_fp_mul_asm = x86_emit_fp_mul_asm;
    be->emit_fp_div_asm = x86_emit_fp_div_asm;
    be->emit_fp_cmp_asm = x86_emit_fp_cmp_asm;
    be->emit_fp_mov_asm = x86_emit_fp_mov_asm;
    be->emit_fp_cvtsf_asm = x86_emit_fp_cvtsf_asm;
    be->emit_fp_cvtsi_asm = x86_emit_fp_cvtsi_asm;
    be->emit_vla_alloc_asm = x86_emit_vla_alloc_asm;
    be->emit_param_save_int_asm = x86_emit_param_save_int_asm;
    be->emit_param_save_fp_asm = x86_emit_param_save_fp_asm;
    be->emit_variadic_spill_asm = x86_emit_variadic_spill_asm;

    /* Vector instruction emission (SSE2) */
    be->emit_vload_asm = x86_emit_vload_asm;
    be->emit_vstore_asm = x86_emit_vstore_asm;
    be->emit_vaddss_asm = x86_emit_vaddss_asm;
    be->emit_vsubss_asm = x86_emit_vsubss_asm;
    be->emit_vmulss_asm = x86_emit_vmulss_asm;
    be->emit_vdivss_asm = x86_emit_vdivss_asm;
    be->emit_vaddps_asm = x86_emit_vaddps_asm;
    be->emit_vsubps_asm = x86_emit_vsubps_asm;
    be->emit_vmulps_asm = x86_emit_vmulps_asm;
    be->emit_vdivps_asm = x86_emit_vdivps_asm;
    be->emit_vaddsd_asm = x86_emit_vaddsd_asm;
    be->emit_vsubsd_asm = x86_emit_vsubsd_asm;
    be->emit_vmulsd_asm = x86_emit_vmulsd_asm;
    be->emit_vdivsd_asm = x86_emit_vdivsd_asm;
    be->emit_vaddpd_asm = x86_emit_vaddpd_asm;
    be->emit_vsubpd_asm = x86_emit_vsubpd_asm;
    be->emit_vmulpd_asm = x86_emit_vmulpd_asm;
    be->emit_vdivpd_asm = x86_emit_vdivpd_asm;
    be->emit_vmovss_asm = x86_emit_vmovss_asm;
    be->emit_vmovsd_asm = x86_emit_vmovsd_asm;
    be->emit_vmovaps_asm = x86_emit_vmovaps_asm;
    be->emit_vmovapd_asm = x86_emit_vmovapd_asm;
    be->emit_vandps_asm = x86_emit_vandps_asm;
    be->emit_vorps_asm = x86_emit_vorps_asm;
    be->emit_vxorps_asm = x86_emit_vxorps_asm;
    be->emit_vshufps_asm = x86_emit_vshufps_asm;
    be->emit_vcvt_si2ss_asm = x86_emit_vcvt_si2ss_asm;
    be->emit_vcvt_ss2si_asm = x86_emit_vcvt_ss2si_asm;
    be->emit_vcmpss_asm = x86_emit_vcmpss_asm;
    be->emit_vcmpsd_asm = x86_emit_vcmpsd_asm;
    be->emit_vpaddd_asm = x86_emit_vpaddd_asm;
    be->emit_vpsubd_asm = x86_emit_vpsubd_asm;
    be->emit_vpmulld_asm = x86_emit_vpmulld_asm;
}
