/* Assembly Emitter - IR to Assembly Translation
 *
 * This module implements ISA-agnostic assembly emission from IR.
 * It delegates to backend-specific functions for ISA-specific assembly generation.
 *
 * Option A: Full Backend-Driven Architecture
 * All assembly emission is delegated to BE(c)->emit_*_asm() functions.
 */

#include "zero_cc/ir/asm_emitter.h"
#include <stdio.h>
#include <string.h>
#include "zero_cc/ir/ir.h"
#include "zero_cc/ir/regalloc.h"
#include "zero_cc/isa/isa_backend.h"

/* Get physical register for a temporary */
static inline int asm_get_phys_reg(RegAllocator *ra, int temp) {
    return regalloc_get_reg_for_temp(ra, temp);
}

/* zero_san -fsanitize-trap (Sprint 167): the report block on the x86-64
 * trap path - the checked value (in `reg`), the op code and the check
 * width go into the ABI arg registers (%rdi/%rsi/%rdx), then
 * `call __zerocsan_report`. The caller emits the ud2 (and the skip label)
 * after it: the label spans the block automatically, and the per-ISA trap
 * signal is kept. The value move comes first: `reg` may be %rdi/%rsi/%rdx
 * itself, which the immediate moves below write. The block sits past the
 * skip branch, so the fast path is byte-identical to the no-report
 * codegen. */
static void x86asm_emit_san_report_block(ZeroCCCompiler *c, const char *reg,
                                         int width, int op, FILE *out) {
    (void)c;
    if (strcmp(reg, "rdx") != 0) {
        fprintf(out, "\tmovq\t%%%s,%%rdx\n", reg);
    }
    /* 32-bit immediate moves zero-extend into the 64-bit register - a
     * byte move (`mov $n,%dil`) would leave bits 8-31 stale and the
     * report's %u/%d reads would see garbage. */
    fprintf(out, "\tmov\t$%d,%%edi\n", op);
    fprintf(out, "\tmov\t$%d,%%esi\n", width);
    fprintf(out, "\tcall\t__zerocsan_report\n");
}

/* Emit function label (.globl and func:) */
void emit_function_label_asm(ZeroCCCompiler *c) {
    FILE *out = c->asm_out ? c->asm_out : stdout;
    size_t func_idx = c->lowering.current_func_idx;

    /* Emit .globl and label (skip .globl for static functions) */
    if (func_idx < c->function_count && c->functions[func_idx].name[0]) {
        if (!c->functions[func_idx].is_static) {
            fprintf(out, "\t.globl %s\n", c->functions[func_idx].name);
        }
        fprintf(out, "%s:\n", c->functions[func_idx].name);
        fflush(out);
    }
}

/* Emit function prologue directly (without IR instruction) */
void emit_prologue_asm(ZeroCCCompiler *c, size_t frame_size) {
    if (BE(c)->emit_prologue_asm) {
        BE(c)->emit_prologue_asm(c, frame_size);
    }
    if (c->lowering.has_stack_protector && BE(c)->emit_stack_guard_init_asm) {
        FILE *out = c->asm_out ? c->asm_out : stdout;
        BE(c)->emit_stack_guard_init_asm(c, c->lowering.canary_stack_off, out);
    }
}



/* =========================================================================
 * Core Emission Functions - Delegate to Backend
 * ========================================================================= */

static void asm_emit_instruction_core(ZeroCCCompiler *c, const Instr *instr, size_t idx, size_t func_idx,
                                      int phys_dst, int phys_src1, int phys_src2,
                                      int g128_dhi, int g128_s1hi, int g128_s2hi,
                                      int g64_dhi, int g64_s1hi, int g64_s2hi, FILE *out) {
    switch (instr->op) {
        /* Trap (__builtin_trap / __builtin_unreachable) */
        case OP_TRAP:
            if (BE(c)->emit_trap_asm) {
                BE(c)->emit_trap_asm(c, out);
            }
            return;

        /* Data Movement */
        case OP_MOV_IMM:
            if (BE(c)->emit_mov_imm_asm) {
                BE(c)->emit_mov_imm_asm(c, phys_dst, instr->imm64, out);
            }
            return;

        case OP_MOV_REG:
            /* Peephole: eliminate redundant register self-moves (mov reg, reg) */
            if (phys_dst == phys_src1) {
                return;
            }
            if (BE(c)->emit_mov_reg_asm) {
                BE(c)->emit_mov_reg_asm(c, phys_dst, phys_src1, out);
            }
            return;

        case OP_MOV:  /* Load from stack */
            if (BE(c)->emit_load_asm) {
                BE(c)->emit_load_asm(c, phys_dst, zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, (int32_t)instr->imm64), out);
            }
            return;

        case OP_MOV_MEM:  /* Store to stack */
            if (BE(c)->emit_store_asm) {
                BE(c)->emit_store_asm(c, phys_src1, zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, (int32_t)instr->imm64), out);
            }
            return;

        case OP_LEA:
            if (BE(c)->emit_lea_asm) {
                BE(c)->emit_lea_asm(c, phys_dst, zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, (int32_t)instr->imm64), out);
            }
            return;

        case OP_DEREF:
            if (BE(c)->emit_deref_asm) {
                BE(c)->emit_deref_asm(c, phys_dst, phys_src1, out);
            }
            return;

        case OP_LOAD_32: {
            const char *dst_name = BE(c)->get_hw_reg_name(phys_dst);
            const char *addr_name = BE(c)->get_hw_reg_name(phys_src1);
            if (c->isa == ISA_AARCH64) {
                fprintf(out, "\tldr\tw%s, [%s]\n", dst_name + 1, addr_name);
            } else if (c->isa == ISA_RISCV64 || c->isa == ISA_RISCV32) {
                fprintf(out, "\tlw\t%s, 0(%s)\n", dst_name, addr_name);
            } else {
                static const char *r32_names[] = {
                    "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi",
                    "r8d", "r9d", "r10d", "r11d", "r12d", "r13d", "r14d", "r15d"
                };
                const char *reg_name = (phys_dst >= 0 && phys_dst < 16) ? r32_names[phys_dst] : dst_name;
                fprintf(out, "\tmovl\t(%%%s),%%%s\n", addr_name, reg_name);
            }
            return;
        }

        case OP_LOAD_16: {
            const char *dst_name = BE(c)->get_hw_reg_name(phys_dst);
            const char *addr_name = BE(c)->get_hw_reg_name(phys_src1);
            if (c->isa == ISA_AARCH64) {
                fprintf(out, "\tldrh\tw%s, [%s]\n", dst_name + 1, addr_name);
            } else if (c->isa == ISA_RISCV64 || c->isa == ISA_RISCV32) {
                fprintf(out, "\tlhu\t%s, 0(%s)\n", dst_name, addr_name);
            } else {
                fprintf(out, "\tmovzwq\t(%%%s),%%%s\n", addr_name, dst_name);
            }
            return;
        }

        case OP_LOAD_BYTE: {
            const char *dst_name = BE(c)->get_hw_reg_name(phys_dst);
            const char *addr_name = BE(c)->get_hw_reg_name(phys_src1);
            if (c->isa == ISA_AARCH64) {
                fprintf(out, "\tldrb\tw%s, [%s]\n", dst_name + 1, addr_name);
            } else if (c->isa == ISA_RISCV64 || c->isa == ISA_RISCV32) {
                fprintf(out, "\tlbu\t%s, 0(%s)\n", dst_name, addr_name);
            } else {
                fprintf(out, "\tmovzbq\t(%%%s),%%%s\n", addr_name, dst_name);
            }
            return;
        }

        case OP_STORE_64: {
            const char *val_name = BE(c)->get_hw_reg_name(phys_src1);
            const char *addr_name = BE(c)->get_hw_reg_name(phys_src2);
            if (c->isa == ISA_AARCH64) {
                fprintf(out, "\tstr\t%s, [%s]\n", val_name, addr_name);
            } else if (c->isa == ISA_RISCV32) {
                fprintf(out, "\tsw\t%s, 0(%s)\n", val_name, addr_name);
            } else if (c->isa == ISA_RISCV64) {
                fprintf(out, "\tsd\t%s, 0(%s)\n", val_name, addr_name);
            } else {
                fprintf(out, "\tmovq\t%%%s,(%%%s)\n", val_name, addr_name);
            }
            return;
        }

        case OP_STORE_32: {
            const char *val_name = BE(c)->get_hw_reg_name(phys_src1);
            const char *addr_name = BE(c)->get_hw_reg_name(phys_src2);
            if (c->isa == ISA_AARCH64) {
                fprintf(out, "\tstr\tw%s, [%s]\n", val_name + 1, addr_name);
            } else if (c->isa == ISA_RISCV64 || c->isa == ISA_RISCV32) {
                fprintf(out, "\tsw\t%s, 0(%s)\n", val_name, addr_name);
            } else {
                static const char *r32_names[] = {
                    "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi",
                    "r8d", "r9d", "r10d", "r11d", "r12d", "r13d", "r14d", "r15d"
                };
                const char *reg_name = (phys_src1 >= 0 && phys_src1 < 16) ? r32_names[phys_src1] : val_name;
                fprintf(out, "\tmovl\t%%%s,(%%%s)\n", reg_name, addr_name);
            }
            return;
        }

        case OP_STORE_16: {
            const char *val_name = BE(c)->get_hw_reg_name(phys_src1);
            const char *addr_name = BE(c)->get_hw_reg_name(phys_src2);
            if (c->isa == ISA_AARCH64) {
                fprintf(out, "\tstrh\tw%s, [%s]\n", val_name + 1, addr_name);
            } else if (c->isa == ISA_RISCV64 || c->isa == ISA_RISCV32) {
                fprintf(out, "\tsh\t%s, 0(%s)\n", val_name, addr_name);
            } else {
                static const char *r16_names[] = {
                    "ax", "cx", "dx", "bx", "sp", "bp", "si", "di",
                    "r8w", "r9w", "r10w", "r11w", "r12w", "r13w", "r14w", "r15w"
                };
                const char *reg_name = (phys_src1 >= 0 && phys_src1 < 16) ? r16_names[phys_src1] : val_name;
                fprintf(out, "\tmovw\t%%%s,(%%%s)\n", reg_name, addr_name);
            }
            return;
        }

        case OP_STORE_BYTE: {
            const char *val_name = BE(c)->get_hw_reg_name(phys_src1);
            const char *addr_name = BE(c)->get_hw_reg_name(phys_src2);
            if (c->isa == ISA_AARCH64) {
                fprintf(out, "\tstrb\tw%s, [%s]\n", val_name + 1, addr_name);
            } else if (c->isa == ISA_RISCV64 || c->isa == ISA_RISCV32) {
                fprintf(out, "\tsb\t%s, 0(%s)\n", val_name, addr_name);
            } else {
                static const char *r8_names[] = {
                    "al", "cl", "dl", "bl", "spl", "bpl", "sil", "dil",
                    "r8b", "r9b", "r10b", "r11b", "r12b", "r13b", "r14b", "r15b"
                };
                const char *reg_name = (phys_src1 >= 0 && phys_src1 < 16) ? r8_names[phys_src1] : val_name;
                fprintf(out, "\tmovb\t%%%s,(%%%s)\n", reg_name, addr_name);
            }
            return;
        }

        /* Arithmetic */
        case OP_ADD:
            if (BE(c)->emit_add_asm) {
                BE(c)->emit_add_asm(c, phys_dst, phys_src1, phys_src2, out);
            }
            return;

        case OP_SUB:
            if (BE(c)->emit_sub_asm) {
                BE(c)->emit_sub_asm(c, phys_dst, phys_src1, phys_src2, out);
            }
            return;

        /* zero_san v1 (x86-64): signed-integer-overflow trap after signed
         * add/sub/mul. The preceding op set the CPU flags; we check whether
         * the raw 64-bit result fits in `width` signed bits and emit ud2
         * if not. All inline, self-contained labels. */
        case OP_OVF_TRAP: {
            if (c->isa != ISA_X86_64) {
                /* Sprint 163: compare-based check on aarch64/riscv. Operands
                 * (a, b) + op are carried in temp_src1/temp_src2 + imm (already
                 * resolved to phys_src1/phys_src2 by the caller). */
                if (BE(c)->emit_ovf_trap_asm) {
                    int width = zc_ovf_trap_width(c, instr);
                    int op = (int)instr->imm;
                    BE(c)->emit_ovf_trap_asm(c, phys_dst, phys_src1, phys_src2, width, op, out);
                }
                return;
            }
            int width = zc_ovf_trap_width(c, instr);
            int spill_dst_local = regalloc_get_spill_slot_for_temp(&c->regalloc, instr->temp_dest);
            bool is_spilled = (spill_dst_local >= 0);
            const char *reg = BE(c)->get_hw_reg_name(phys_dst);
            int scratch = is_spilled ? BE(c)->scratch_reg2 : BE(c)->scratch_reg1;
            const char *scratch_name = BE(c)->get_hw_reg_name(scratch);

            /* Unique label for the trap target (no IR OP_LABEL needed). */
            uint16_t trap_lbl = c->ir_next_label++;
            char lbl[32];
            snprintf(lbl, sizeof(lbl), ".Lovf_%u", (unsigned)trap_lbl);

            /* Follow-on (Sprint 166): div-by-zero / shift-count checks.
             * These test a register directly (no OF flag). temp_dest holds
             * the divisor/count temp (set at lowering), so phys_dst is the
             * register to test. */
            int op = (int)instr->imm;
            if (op == ZC_OVF_OP_DIV_ZERO || op == ZC_OVF_OP_REM_ZERO ||
                op == ZC_OVF_OP_SHIFT_COUNT) {
                if (op == ZC_OVF_OP_DIV_ZERO || op == ZC_OVF_OP_REM_ZERO) {
                    /* test %reg,%reg; jnz .Lskip  (trap if divisor == 0) */
                    fprintf(out, "\ttest\t%%%s,%%%s\n", reg, reg);
                    fprintf(out, "\tjnz\t%s\n", lbl);
                    if (c->sanitize_report)
                        x86asm_emit_san_report_block(c, reg, width, op, out);
                    fprintf(out, "\tud2\n");            /* trap: divisor == 0 */
                    fprintf(out, "%s:\n", lbl);
                    return;
                } else { /* ZC_OVF_OP_SHIFT_COUNT */
                    /* Two independent skip-triplets (count bound is the
                     * operand bit width: width*8):
                     *   cmp $0;  jge .L1; <report> ud2; .L1:    (count < 0 -> trap)
                     *   cmp $W;  jl  .L2; <report> ud2; .L2:    (count >= W -> trap) */
                    uint16_t lbl2 = c->ir_next_label++;
                    char lbl2s[32];
                    snprintf(lbl2s, sizeof(lbl2s), ".Lovf_%u", (unsigned)lbl2);
                    fprintf(out, "\tcmp\t$0,%%%s\n", reg);
                    fprintf(out, "\tjge\t%s\n", lbl);
                    if (c->sanitize_report)
                        x86asm_emit_san_report_block(c, reg, width, op, out);
                    fprintf(out, "\tud2\n");            /* trap: count < 0 */
                    fprintf(out, "%s:\n", lbl);
                    fprintf(out, "\tcmp\t$%d,%%%s\n", width * 8, reg);
                    fprintf(out, "\tjl\t%s\n", lbl2s);
                    if (c->sanitize_report)
                        x86asm_emit_san_report_block(c, reg, width, op, out);
                    fprintf(out, "\tud2\n");            /* trap: count >= bits */
                    fprintf(out, "%s:\n", lbl2s);
                }
                return;
            }

            if (width >= 8) {
                /* 64-bit: jno skips the trap when no overflow (OF=0).
                 * Overflow (OF=1) falls through to the report block
                 * (when -fsanitize-trap) + ud2 -> SIGILL. */
                fprintf(out, "\tjno\t%s\n", lbl);
                if (c->sanitize_report)
                    x86asm_emit_san_report_block(c, reg, width, op, out);
                fprintf(out, "\tud2\n");
                fprintf(out, "%s:\n", lbl);
            } else {
                /* Narrow: sign-extension round-trip check.
                 * scratch = (reg << (64-N)) >> (64-N) [arithmetic].
                 * If scratch == reg, value fits in N bits -> skip trap.
                 * Otherwise -> overflow -> trap. */
                int shift = 64 - width * 8;
                fprintf(out, "\tmovq\t%%%s,%%%s\n", reg, scratch_name);
                fprintf(out, "\tshl\t$%d,%%%s\n", shift, scratch_name);
                fprintf(out, "\tsar\t$%d,%%%s\n", shift, scratch_name);
                fprintf(out, "\tcmp\t%%%s,%%%s\n", scratch_name, reg);
                fprintf(out, "\tje\t%s\n", lbl);   /* equal (fits): skip trap */
                if (c->sanitize_report)
                    x86asm_emit_san_report_block(c, reg, width, op, out);
                fprintf(out, "\tud2\n");            /* trap on overflow */
                fprintf(out, "%s:\n", lbl);
            }
            return;
        }

        case OP_MUL:
            if (BE(c)->emit_mul_asm) {
                BE(c)->emit_mul_asm(c, phys_dst, phys_src1, phys_src2, out);
            }
            return;

        case OP_DIV:
            if (BE(c)->emit_div_asm) {
                c->lowering.div_is_unsigned = zc_div_is_unsigned(c, instr);
                BE(c)->emit_div_asm(c, phys_dst, phys_src1, phys_src2, out);
                c->lowering.div_is_unsigned = 0;
            }
            return;

        case OP_MOD:
            if (BE(c)->emit_mod_asm) {
                c->lowering.div_is_unsigned = zc_div_is_unsigned(c, instr);
                BE(c)->emit_mod_asm(c, phys_dst, phys_src1, phys_src2, out);
                c->lowering.div_is_unsigned = 0;
            }
            return;

        case OP_AND:
            if (BE(c)->emit_and_asm) {
                BE(c)->emit_and_asm(c, phys_dst, phys_src1, phys_src2, out);
            }
            return;

        case OP_OR:
            if (BE(c)->emit_or_asm) {
                BE(c)->emit_or_asm(c, phys_dst, phys_src1, phys_src2, out);
            }
            return;

        case OP_XOR:
            if (BE(c)->emit_xor_asm) {
                BE(c)->emit_xor_asm(c, phys_dst, phys_src1, phys_src2, out);
            }
            return;

        case OP_SHL:
            if (BE(c)->emit_shl_asm) {
                BE(c)->emit_shl_asm(c, phys_dst, phys_src1, phys_src2, out);
            }
            return;

        case OP_SHR:
            if (BE(c)->emit_shr_asm) {
                BE(c)->emit_shr_asm(c, phys_dst, phys_src1, phys_src2, out);
            }
            return;

        case OP_SAR:
            if (BE(c)->emit_sar_asm) {
                BE(c)->emit_sar_asm(c, phys_dst, phys_src1, phys_src2, out);
            }
            return;

        case OP_NEG:
            if (BE(c)->emit_neg_asm) {
                BE(c)->emit_neg_asm(c, phys_dst, phys_src1, out);
            }
            return;

        /* In-place ALU ops (Sprint 40: previously fell through to the
         * `# UNKNOWN` default, making -S output silently wrong). */
        case OP_ADD_IMM:
            if (BE(c)->emit_add_imm_asm) {
                BE(c)->emit_add_imm_asm(c, phys_dst, instr->imm, out);
            }
            return;

        case OP_SUB_IMM:
            if (BE(c)->emit_sub_imm_asm) {
                BE(c)->emit_sub_imm_asm(c, phys_dst, instr->imm, out);
            }
            return;

        case OP_INC:
            if (BE(c)->emit_inc_asm) {
                BE(c)->emit_inc_asm(c, phys_dst, out);
            }
            return;

        case OP_DEC:
            if (BE(c)->emit_dec_asm) {
                BE(c)->emit_dec_asm(c, phys_dst, out);
            }
            return;

        case OP_NOT:
            if (BE(c)->emit_not_asm) {
                BE(c)->emit_not_asm(c, phys_dst, phys_src1, out);
            }
            return;

        /* Logical NOT: test src,src + setcc EQ + movzbq - mirrors the MC
         * emitter (OP_LNOT in ast_lowering_mc.c). */
        case OP_LNOT: {
            const char *r = BE(c)->get_hw_reg_name(phys_src1);
            fprintf(out, "\ttestq\t%%%s,%%%s\n", r, r);
            if (BE(c)->emit_cond_set_asm) {
                BE(c)->emit_cond_set_asm(c, phys_dst, CC_EQ, out);
            }
            return;
        }

        /* In-place width extension: dst is the value register, src is its
         * (expected identical) physical register. */
        case OP_SEXT8:
        case OP_SEXT16:
        case OP_SEXT32:
        case OP_ZEXT8:
        case OP_ZEXT16:
        case OP_ZEXT32: {
            /* Width from the opcode (same mapping as the MC path in
             * ast_lowering_mc.c - the opcodes are consecutive, not
             * interleaved, so no /2). */
            int width = (instr->op == OP_SEXT16 || instr->op == OP_ZEXT16) ? 2
                         : (instr->op == OP_SEXT32 || instr->op == OP_ZEXT32) ? 4 : 1;
            int sign = (instr->op >= OP_SEXT8 && instr->op <= OP_SEXT32) ? 1 : 0;
            if (BE(c)->emit_extend_asm) {
                BE(c)->emit_extend_asm(c, phys_dst, phys_src1, width, sign, out);
            }
            return;
        }

        /* Comparison */
        case OP_CMP: {
            /* Float compare (ucomisd / fcmp) when either operand is an
             * FP-class temp - mirrors the MC emitter's dispatch. */
            RegClass rc1 = regalloc_get_class_for_temp(&c->regalloc, instr->temp_src1);
            RegClass rc2 = regalloc_get_class_for_temp(&c->regalloc, instr->temp_src2);
            int cmp_fp = (rc1 == REG_CLASS_XMM || rc1 == REG_CLASS_VEC ||
                          rc2 == REG_CLASS_XMM || rc2 == REG_CLASS_VEC);
            c->lowering.cmp_is_fp = cmp_fp;
            c->lowering.cmp_is_64 = 0;  /* a 32-bit compare (Sprint 170) */
            c->lowering.cmp_src1 = phys_src1;
            c->lowering.cmp_src2 = phys_src2;
            if (cmp_fp && BE(c)->emit_fp_cmp_asm) {
                BE(c)->emit_fp_cmp_asm(c, phys_src1, phys_src2, out);
            } else if (BE(c)->emit_cmp_asm) {
                BE(c)->emit_cmp_asm(c, phys_src1, phys_src2, out);
            }
            return;
        }

        case OP_COND_SET:
            if (BE(c)->emit_cond_set_asm) {
                BE(c)->emit_cond_set_asm(c, phys_dst, instr->cond, out);
            }
            return;

        /* CMP_ZERO: compare reg against 0 - sets the condition for the
         * following OP_COND_SET. Used by !expr / expr==0 / expr!=0 / && / ||.
         * ISA-specific: x86 `testq`, aarch64 `cmp r,#0` (flags for cset),
         * riscv flags materialization via c->lowering.cmp_src1/2. */
        case OP_CMP_ZERO: {
            RegClass rc0 = regalloc_get_class_for_temp(&c->regalloc, instr->temp_src1);
            c->lowering.cmp_src1 = phys_src1;
            c->lowering.cmp_src2 = 0;  /* zero register */
            c->lowering.cmp_is_fp = (rc0 == REG_CLASS_XMM || rc0 == REG_CLASS_VEC);
            c->lowering.cmp_is_64 = 0;  /* a 32-bit zero test (Sprint 170) */
            const char *r = BE(c)->get_hw_reg_name(phys_src1);
            if (c->isa == ISA_AARCH64) {
                fprintf(out, "\tcmp\t%s, #0\n", r);
            } else if (c->isa == ISA_RISCV64 || c->isa == ISA_RISCV32) {
                /* RISC-V flags materialization: state recorded in c->lowering */
            } else {
                fprintf(out, "\ttestq\t%%%s,%%%s\n", r, r);
            }
            return;
        }

        /* Control Flow */
        case OP_LABEL:
            /* Sprint 99: a nonlocal-goto landing point in a frame-pointer-
             * omitted function re-establishes the entry-time FP register;
             * the label must point AT the restore (record first, emit
             * second). MC counterpart in ast_lowering_mc.c. */
            bool nl_landing = false;
            if (c->lowering.omit_frame_pointer && c->lowering.fp_snap_off > 0 &&
                BE(c)->emit_load_asm) {
                const ZeroCCFunction *lf = &c->functions[func_idx];
                for (int li = 0; li < lf->nl_label_count; li++) {
                    if (lf->nl_label_ir[li] == (uint32_t)instr->label_idx) {
                        nl_landing = true;
                        break;
                    }
                }
            }
            if (nl_landing && BE(c)->emit_label_asm) {
                char lbl_buf[64];
                snprintf(lbl_buf, sizeof(lbl_buf), ".L%zu_%u", func_idx, (unsigned)instr->label_idx);
                BE(c)->emit_label_asm(c, lbl_buf, out);
                BE(c)->emit_load_asm(c, BE(c)->fp_reg, zero_cc_frame_base_reg(c),
                                     zero_cc_frame_disp(c, -c->lowering.fp_snap_off), out);
            } else if (BE(c)->emit_label_asm) {
                char lbl_buf[64];
                snprintf(lbl_buf, sizeof(lbl_buf), ".L%zu_%u", func_idx, (unsigned)instr->label_idx);
                BE(c)->emit_label_asm(c, lbl_buf, out);
            }
            return;

        case OP_JMP: {
            /* Fallthrough optimization: skip jump if next active instruction is target label */
            size_t count = ir_get_count(c);
            const Instr *buf = ir_get_buffer(c);
            size_t next_idx = idx + 1;
            while (next_idx < count && (buf[next_idx].op == OP_DEAD || buf[next_idx].op == OP_NOP)) {
                next_idx++;
            }
            if (next_idx < count && buf[next_idx].op == OP_LABEL && buf[next_idx].label_idx == instr->label_idx) {
                return;
            }
            /* Option 2: Inline epilogue on early returns when no static-local mirror or CSR restore is required */
            if (instr->label_idx == c->lowering.epilogue_label &&
                !c->lowering.has_stack_protector &&
                c->lowering.static_local_rt_off == 0 &&
                c->regalloc.used_callee_saved_gpr_mask == 0 &&
                BE(c)->emit_epilogue_asm) {
                BE(c)->emit_epilogue_asm(c);
                return;
            }
            if (BE(c)->emit_jmp_asm) {
                char lbl_buf[64];
                snprintf(lbl_buf, sizeof(lbl_buf), ".L%zu_%u", func_idx, (unsigned)instr->label_idx);
                BE(c)->emit_jmp_asm(c, lbl_buf, out);
            }
            return;
        }

        case OP_JE:
            if (BE(c)->emit_jeq_asm) {
                char lbl_buf[64];
                snprintf(lbl_buf, sizeof(lbl_buf), ".L%zu_%u", func_idx, (unsigned)instr->label_idx);
                const char *src_name = BE(c)->get_hw_reg_name(phys_src1);
                if (c->isa == ISA_AARCH64) {
                    fprintf(out, "\tcmp\t%s, #0\n", src_name);
                } else if (c->isa == ISA_RISCV64 || c->isa == ISA_RISCV32) {
                    fprintf(out, "\tbeq\t%s, zero, %s\n", src_name, lbl_buf);
                    return;
                } else {
                    fprintf(out, "\ttestq\t%%%s, %%%s\n", src_name, src_name);
                }
                BE(c)->emit_jeq_asm(c, lbl_buf, out);
            }
            return;

        case OP_JNE:
            if (BE(c)->emit_jne_asm) {
                char lbl_buf[64];
                snprintf(lbl_buf, sizeof(lbl_buf), ".L%zu_%u", func_idx, (unsigned)instr->label_idx);
                const char *src_name = BE(c)->get_hw_reg_name(phys_src1);
                if (c->isa == ISA_AARCH64) {
                    fprintf(out, "\tcmp\t%s, #0\n", src_name);
                } else if (c->isa == ISA_RISCV64 || c->isa == ISA_RISCV32) {
                    fprintf(out, "\tbne\t%s, zero, %s\n", src_name, lbl_buf);
                    return;
                } else {
                    fprintf(out, "\ttestq\t%%%s, %%%s\n", src_name, src_name);
                }
                BE(c)->emit_jne_asm(c, lbl_buf, out);
            }
            return;

        case OP_JL:
            if (BE(c)->emit_jlt_asm) {
                char lbl_buf[64];
                snprintf(lbl_buf, sizeof(lbl_buf), ".L%zu_%u", func_idx, (unsigned)instr->label_idx);
                BE(c)->emit_jlt_asm(c, lbl_buf, out);
            }
            return;

        case OP_JLE:
            if (BE(c)->emit_jle_asm) {
                char lbl_buf[64];
                snprintf(lbl_buf, sizeof(lbl_buf), ".L%zu_%u", func_idx, (unsigned)instr->label_idx);
                BE(c)->emit_jle_asm(c, lbl_buf, out);
            }
            return;

        case OP_JG:
            if (BE(c)->emit_jgt_asm) {
                char lbl_buf[64];
                snprintf(lbl_buf, sizeof(lbl_buf), ".L%zu_%u", func_idx, (unsigned)instr->label_idx);
                BE(c)->emit_jgt_asm(c, lbl_buf, out);
            }
            return;

        case OP_JCOND:
            if (BE(c)->emit_jeq_asm || BE(c)->emit_jne_asm || BE(c)->emit_jlt_asm ||
                BE(c)->emit_jle_asm || BE(c)->emit_jgt_asm || BE(c)->emit_jge_asm) {
                char lbl_buf[64];
                snprintf(lbl_buf, sizeof(lbl_buf), ".L%zu_%u", func_idx, (unsigned)instr->label_idx);
                switch (instr->cond) {
                    case 0: if (BE(c)->emit_jlt_asm) BE(c)->emit_jlt_asm(c, lbl_buf, out); break;
                    case 1: if (BE(c)->emit_jle_asm) BE(c)->emit_jle_asm(c, lbl_buf, out); break;
                    case 2: if (BE(c)->emit_jgt_asm) BE(c)->emit_jgt_asm(c, lbl_buf, out); break;
                    case 3: if (BE(c)->emit_jge_asm) BE(c)->emit_jge_asm(c, lbl_buf, out); break;
                    case 4: if (BE(c)->emit_jeq_asm) BE(c)->emit_jeq_asm(c, lbl_buf, out); break;
                    case 5: if (BE(c)->emit_jne_asm) BE(c)->emit_jne_asm(c, lbl_buf, out); break;
                }
            }
            return;

        case OP_LEA_RIP: {
            size_t rodata_offset = (size_t)instr->imm64;
            const char *dst_name = BE(c)->get_hw_reg_name(phys_dst);
            if (c->isa == ISA_AARCH64) {
                fprintf(out, "\tadrp\t%s, .Lrodata+%zu\n", dst_name, rodata_offset);
                fprintf(out, "\tadd\t%s, %s, :lo12:.Lrodata+%zu\n", dst_name, dst_name, rodata_offset);
            } else if (c->isa == ISA_RISCV64 || c->isa == ISA_RISCV32) {
                /* NOTE: `lla dst, .Lrodata+off` (and %pcrel_hi(sym+off)) are
                 * broken in binutils 2.41 for symbol+offset references on
                 * RISC-V (LO12 reloc references a phantom .L0, address comes
                 * out off by one). Use a plain-label load + immediate add. */
                if (rodata_offset == 0) {
                    fprintf(out, "\tlla\t%s, .Lrodata\n", dst_name);
                } else if (phys_dst != 5 /* t0 */) {
                    if (rodata_offset <= 2047) {
                        fprintf(out, "\tlla\tt0, .Lrodata\n");
                        fprintf(out, "\taddi\t%s, t0, %zu\n", dst_name, rodata_offset);
                    } else {
                        fprintf(out, "\tlla\tt0, .Lrodata\n");
                        fprintf(out, "\tli\tt1, %zu\n", rodata_offset);
                        fprintf(out, "\tadd\t%s, t0, t1\n", dst_name);
                    }
                } else {
                    /* dst is t0: use t1 as the base holder */
                    if (rodata_offset <= 2047) {
                        fprintf(out, "\tlla\tt1, .Lrodata\n");
                        fprintf(out, "\taddi\tt0, t1, %zu\n", rodata_offset);
                    } else {
                        fprintf(out, "\tlla\tt1, .Lrodata\n");
                        fprintf(out, "\tli\tt2, %zu\n", rodata_offset);
                        fprintf(out, "\tadd\tt0, t1, t2\n");
                    }
                }
            } else {
                fprintf(out, "\tleaq\t.Lrodata+%zu(%%rip),%%%s\n", rodata_offset, dst_name);
            }
            return;
        }

        case OP_LEA_LABEL: {
            char lbl_buf[64];
            /* imm64 != 0: cross-scope `&&label` - the label symbol belongs
             * to the ANCESTOR function (1-based owner fn index, so 0 keeps
             * meaning the current function); the asm labels are
             * .L<func>_<ir> in the single .s output, so reference the
             * owner's label directly. */
            size_t owner = (instr->imm64 != 0) ? (size_t)(instr->imm64 - 1) : (size_t)func_idx;
            snprintf(lbl_buf, sizeof(lbl_buf), ".L%zu_%u", owner, (unsigned)instr->label_idx);
            const char *dst_name = BE(c)->get_hw_reg_name(phys_dst);
            if (c->isa == ISA_AARCH64) {
                fprintf(out, "\tadrp\t%s, %s\n", dst_name, lbl_buf);
                fprintf(out, "\tadd\t%s, %s, :lo12:%s\n", dst_name, dst_name, lbl_buf);
            } else if (c->isa == ISA_RISCV64 || c->isa == ISA_RISCV32) {
                fprintf(out, "\tlla\t%s, %s\n", dst_name, lbl_buf);
            } else {
                fprintf(out, "\tleaq\t%s(%%rip),%%%s\n", lbl_buf, dst_name);
            }
            return;
        }

        case OP_JMP_INDIRECT: {
            const char *src_name = BE(c)->get_hw_reg_name(phys_src1);
            if (c->isa == ISA_AARCH64) {
                fprintf(out, "\tbr\t%s\n", src_name);
            } else if (c->isa == ISA_RISCV64 || c->isa == ISA_RISCV32) {
                fprintf(out, "\tjr\t%s\n", src_name);
            } else {
                fprintf(out, "\tjmp\t*%%%s\n", src_name);
            }
            return;
        }

        case OP_NONLOCAL_GOTO: {
            /* Sprint 99: asm labels are deterministic (.L<func>_<ir>) in the
             * single .s output, so the jump is a plain branch to the
             * ancestor's label after restoring its (FP, SP). */
            char tlbl[64];
            snprintf(tlbl, sizeof(tlbl), ".L%zu_%u",
                     (size_t)instr->imm64, (unsigned)instr->label_idx);
            const char *r1 = BE(c)->get_hw_reg_name(phys_src1);
            const char *r2 = BE(c)->get_hw_reg_name(phys_src2);
            if (c->isa == ISA_AARCH64) {
                fprintf(out, "\tmov\tx29, %s\n\tmov\tsp, %s\n\tb\t%s\n", r1, r2, tlbl);
            } else if (c->isa == ISA_RISCV64 || c->isa == ISA_RISCV32) {
                fprintf(out, "\tmv\ts0, %s\n\tmv\tsp, %s\n\tj\t%s\n", r1, r2, tlbl);
            } else {
                fprintf(out, "\tmov\t%%%s, %%rbp\n\tmov\t%%%s, %%rsp\n\tjmp\t%s\n", r1, r2, tlbl);
            }
            return;
        }

        case OP_SETJMP: {
            int s2 = BE(c)->scratch_reg2;
            int ret_reg = BE(c)->get_int_reg ? BE(c)->get_int_reg(VREG_0) : 0;
            int32_t w = (c->isa == ISA_RISCV32) ? 4 : 8;
            char lbl_buf[64];
            snprintf(lbl_buf, sizeof(lbl_buf), ".Lsetjmp_%zu_%u", func_idx, (unsigned)instr->label_idx);
            const char *s2_name = BE(c)->get_hw_reg_name(s2);
            BE(c)->emit_store_asm(c, BE(c)->fp_reg, phys_src1, 0, out);
            BE(c)->emit_mov_reg_asm(c, s2, BE(c)->sp_reg, out);
            BE(c)->emit_store_asm(c, s2, phys_src1, 2 * w, out);
            if (c->isa == ISA_AARCH64) {
                fprintf(out, "\tadr\t%s, %s\n", s2_name, lbl_buf);
            } else if (c->isa == ISA_RISCV64 || c->isa == ISA_RISCV32) {
                fprintf(out, "\tlla\t%s, %s\n", s2_name, lbl_buf);
            } else {
                fprintf(out, "\tleaq\t%s(%%rip), %%%s\n", lbl_buf, s2_name);
            }
            BE(c)->emit_store_asm(c, s2, phys_src1, 1 * w, out);
            BE(c)->emit_mov_imm_asm(c, ret_reg, 0, out);
            fprintf(out, "%s:\n", lbl_buf);
            return;
        }

        case OP_LONGJMP: {
            int s1 = BE(c)->scratch_reg1;
            int s2 = BE(c)->scratch_reg2;
            int ret_reg = BE(c)->get_int_reg ? BE(c)->get_int_reg(VREG_0) : 0;
            int32_t w = (c->isa == ISA_RISCV32) ? 4 : 8;
            const char *s2_name = BE(c)->get_hw_reg_name(s2);
            if (s1 != phys_src1) {
                BE(c)->emit_mov_reg_asm(c, s1, phys_src1, out);
            }
            if (ret_reg != phys_src2) {
                BE(c)->emit_mov_reg_asm(c, ret_reg, phys_src2, out);
            }
            BE(c)->emit_load_asm(c, BE(c)->fp_reg, s1, 0, out);
            BE(c)->emit_load_asm(c, s2, s1, 2 * w, out);
            BE(c)->emit_mov_reg_asm(c, BE(c)->sp_reg, s2, out);
            BE(c)->emit_load_asm(c, s2, s1, 1 * w, out);
            if (c->isa == ISA_AARCH64) {
                fprintf(out, "\tbr\t%s\n", s2_name);
            } else if (c->isa == ISA_RISCV64 || c->isa == ISA_RISCV32) {
                fprintf(out, "\tjr\t%s\n", s2_name);
            } else {
                fprintf(out, "\tjmp\t*%%%s\n", s2_name);
            }
            return;
        }

        case OP_MOV_GLOBAL: {
            const char *symbol_name = (const char *)(intptr_t)instr->imm64;
            const char *dst_name = BE(c)->get_hw_reg_name(phys_dst);
            if (c->isa == ISA_AARCH64) {
                fprintf(out, "\tadrp\t%s, %s\n", dst_name, symbol_name);
                fprintf(out, "\tadd\t%s, %s, :lo12:%s\n", dst_name, dst_name, symbol_name);
            } else if (c->isa == ISA_RISCV64 || c->isa == ISA_RISCV32) {
                fprintf(out, "\tlla\t%s, %s\n", dst_name, symbol_name);
            } else {
                fprintf(out, "\tleaq\t%s(%%rip),%%%s\n", symbol_name, dst_name);
            }
            return;
        }

        case OP_TLS_LEA: {
            /* Address of a _Thread_local static (initial-exec, x86-64):
             * the canonical GCC idiom - load the thread pointer, then add
             * the @tpoff displacement (the assembler emits TPOFF32). */
            const char *symbol_name = (const char *)(intptr_t)instr->imm64;
            const char *dst_name = BE(c)->get_hw_reg_name(phys_dst);
            if (c->isa != ISA_X86_64) {
                /* Unreachable: the TLS backend rejects other ISAs. */
                fprintf(out, "\tmovq\t$0,%%%s\n", dst_name);
                return;
            }
            fprintf(out, "\tmovq\t%%fs:0,%%%s\n", dst_name);
            fprintf(out, "\tleaq\t%s@tpoff(%%%s),%%%s\n", symbol_name, dst_name, dst_name);
            return;
        }

        case OP_CALL: {
            /* Get function name from imm64 (stored as pointer) */
            const char *func_name = (const char *)(intptr_t)instr->imm64;

            /* Collect call arguments: args 0/1 from OP_CALL itself,
             * args 2+ from OP_CALL_ARG pseudo-instructions that follow
             * in the stream (Sprint 151c). */
            int call_arg_count = (int)instr->temp_dest;
            int unpacked_temps[IR_CALL_MAX_ARGS];
            int unpacked_is_fp[IR_CALL_MAX_ARGS];
            int unpacked_stack_pos[IR_CALL_MAX_ARGS];
            for (int a = 0; a < IR_CALL_MAX_ARGS; a++) {
                unpacked_temps[a] = 0;
                unpacked_is_fp[a] = 0;
                unpacked_stack_pos[a] = 255;
            }
            int n = 0;
            if (call_arg_count > 0 && instr->temp_src1 != TEMP_NONE) {
                unpacked_temps[n] = (int)instr->temp_src1;
                unpacked_is_fp[n] = instr->cond;   /* Sprint 151c: is_fp of arg 0 */
                /* Sprint 158: arg 0/1 stack positions in imm (255 = register). */
                unpacked_stack_pos[n] = (uint8_t)(instr->imm & 0xFF);
                n++;
            }
            if (call_arg_count > 1 && instr->temp_src2 != TEMP_NONE) {
                unpacked_temps[n] = (int)instr->temp_src2;
                unpacked_is_fp[n] = instr->width;  /* Sprint 151c: is_fp of arg 1 */
                unpacked_stack_pos[n] = (uint8_t)((instr->imm >> 8) & 0xFF);
                n++;
            }
            for (int a = 2; a < call_arg_count && n < IR_CALL_MAX_ARGS; a++) {
                size_t ca_idx = idx + 1 + (a - 2);
                if (ca_idx >= ir_get_count(c)) break;
                const Instr *ca_instr = ir_get(c, ca_idx);
                if (ca_instr->op != OP_CALL_ARG) break;
                unpacked_temps[n] = (int)ca_instr->temp_src1;
                unpacked_is_fp[n] = ca_instr->cond;
                unpacked_stack_pos[n] = ca_instr->width;
                n++;
            }

            if (BE(c)->emit_call_asm) {
                c->asm_call_seq++;  /* index into the lowering ir_ctx call tables */
                BE(c)->emit_call_asm(c, func_name, n, unpacked_temps, unpacked_is_fp, unpacked_stack_pos, out);
            }
            return;
        }

        case OP_SYSCALL: {
            /* OS syscall intrinsic - same argument packing as OP_CALL. */
            int s_arg_count = (int)instr->temp_dest;
            int unpacked_temps[IR_CALL_MAX_ARGS];
            int n = 0;
            if (s_arg_count > 0 && instr->temp_src1 != TEMP_NONE) {
                unpacked_temps[n++] = (int)instr->temp_src1;
            }
            if (s_arg_count > 1 && instr->temp_src2 != TEMP_NONE && n < 7) {
                unpacked_temps[n++] = (int)instr->temp_src2;
            }
            for (int a = 2; a < s_arg_count && n < 7; a++) {
                size_t ca_idx = idx + 1 + (a - 2);
                if (ca_idx >= ir_get_count(c)) break;
                const Instr *ca_instr = ir_get(c, ca_idx);
                if (ca_instr->op != OP_CALL_ARG) break;
                if (ca_instr->cond != 0) continue; /* syscall args are integer */
                unpacked_temps[n++] = (int)ca_instr->temp_src1;
            }
            if (BE(c)->emit_syscall_asm) {
                BE(c)->emit_syscall_asm(c, n, unpacked_temps, out);
            } else {
                fprintf(out, "\t# SYSCALL (unsupported on this target)\n");
            }
            return;
        }

        case OP_CLONE: {
            int s_arg_count = (int)instr->temp_dest;
            int unpacked_temps[IR_CALL_MAX_ARGS];
            int n = 0;
            if (s_arg_count > 0 && instr->temp_src1 != TEMP_NONE) {
                unpacked_temps[n++] = (int)instr->temp_src1;
            }
            if (s_arg_count > 1 && instr->temp_src2 != TEMP_NONE && n < 5) {
                unpacked_temps[n++] = (int)instr->temp_src2;
            }
            for (int a = 2; a < s_arg_count && n < 5; a++) {
                size_t ca_idx = idx + 1 + (a - 2);
                if (ca_idx >= ir_get_count(c)) break;
                const Instr *ca_instr = ir_get(c, ca_idx);
                if (ca_instr->op != OP_CALL_ARG) break;
                if (ca_instr->cond != 0) continue; /* clone args are integer */
                unpacked_temps[n++] = (int)ca_instr->temp_src1;
            }
            if (BE(c)->emit_clone_asm) {
                BE(c)->emit_clone_asm(c, n, unpacked_temps, out);
            } else {
                fprintf(out, "\t# CLONE (unsupported on this target)\n");
            }
            return;
        }

        case OP_RET:
            if (BE(c)->emit_ret_asm) {
                BE(c)->emit_ret_asm(c, out);
            }
            return;

        /* Floating Point */
        case OP_FLOAD:
            if (BE(c)->emit_load_fp_asm) {
                BE(c)->emit_load_fp_asm(c, phys_dst, zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, (int32_t)instr->imm64), out);
            }
            return;

        case OP_FSTORE:
            if (BE(c)->emit_store_fp_asm) {
                BE(c)->emit_store_fp_asm(c, phys_src1, zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, (int32_t)instr->imm64), out);
            }
            return;

        case OP_FADD:
            if (BE(c)->emit_fp_add_asm) {
                BE(c)->emit_fp_add_asm(c, phys_dst, phys_src1, phys_src2, out);
            }
            return;

        case OP_FSUB:
            if (BE(c)->emit_fp_sub_asm) {
                BE(c)->emit_fp_sub_asm(c, phys_dst, phys_src1, phys_src2, out);
            }
            return;

        case OP_FMUL:
            if (BE(c)->emit_fp_mul_asm) {
                BE(c)->emit_fp_mul_asm(c, phys_dst, phys_src1, phys_src2, out);
            }
            return;

        case OP_FDIV:
            if (BE(c)->emit_fp_div_asm) {
                BE(c)->emit_fp_div_asm(c, phys_dst, phys_src1, phys_src2, out);
            }
            return;

        case OP_FMOV:
            if (BE(c)->emit_fp_mov_asm) {
                BE(c)->emit_fp_mov_asm(c, phys_dst, phys_src1, out);
            }
            return;

        case OP_CVTSI2SD:
            if (BE(c)->emit_fp_cvtsf_asm) {
                BE(c)->emit_fp_cvtsf_asm(c, phys_dst, phys_src1, out);
            }
            return;

        case OP_CVTTSD2SI:
            if (BE(c)->emit_fp_cvtsi_asm) {
                BE(c)->emit_fp_cvtsi_asm(c, phys_dst, phys_src1, out);
            }
            return;

        /* VLA */
        case OP_VLA_ALLOC:
            c->lowering.vla.cur_sp_slot_disp = instr->imm;
            if (BE(c)->emit_vla_alloc_asm) {
                BE(c)->emit_vla_alloc_asm(c, phys_dst, phys_src1, out);
            }
            return;

        /* Parameter saving */
        case OP_PARAM_SAVE_INT:
            if (BE(c)->emit_param_save_int_asm) {
                BE(c)->emit_param_save_int_asm(c, (int)instr->imm, (int32_t)instr->imm64, out);
            }
            return;
        case OP_PARAM_SAVE_FP:
            if (BE(c)->emit_param_save_fp_asm) {
                BE(c)->emit_param_save_fp_asm(c, (int)instr->imm, (int32_t)instr->imm64, (int)instr->cond, out);
            }
            return;
        case OP_PARAM_LOAD: {
            bool is_fp = instr->cond != 0;
            int arg_idx = (int)instr->imm;
            if (is_fp) {
                if (arg_idx >= 0 && arg_idx < BE(c)->conv->fp_arg_reg_count) {
                    int src_reg = BE(c)->conv->fp_arg_regs[arg_idx];
                    if (phys_dst != src_reg && BE(c)->emit_fp_mov_asm) {
                        BE(c)->emit_fp_mov_asm(c, phys_dst, src_reg, out);
                    }
                }
            } else {
                if (arg_idx >= 0 && arg_idx < BE(c)->conv->int_arg_reg_count) {
                    int src_reg = BE(c)->conv->int_arg_regs[arg_idx];
                    if (phys_dst != src_reg && BE(c)->emit_mov_reg_asm) {
                        BE(c)->emit_mov_reg_asm(c, phys_dst, src_reg, out);
                    }
                }
            }
            return;
        }
        case OP_VARIADIC_SPILL:
            if (BE(c)->emit_variadic_spill_asm) {
                BE(c)->emit_variadic_spill_asm(c, (int)instr->imm, (int32_t)instr->imm64, out);
            }
            return;

        /* SIMD/Vector Operations - Scalar Single-Precision */
        case OP_VADDSS:
            if (BE(c)->emit_vaddss_asm) {
                BE(c)->emit_vaddss_asm(c, phys_dst, phys_src1, phys_src2, out);
            }
            return;

        case OP_VSUBSS:
            if (BE(c)->emit_vsubss_asm) {
                BE(c)->emit_vsubss_asm(c, phys_dst, phys_src1, phys_src2, out);
            }
            return;

        case OP_VMULSS:
            if (BE(c)->emit_vmulss_asm) {
                BE(c)->emit_vmulss_asm(c, phys_dst, phys_src1, phys_src2, out);
            }
            return;

        case OP_VDIVSS:
            if (BE(c)->emit_vdivss_asm) {
                BE(c)->emit_vdivss_asm(c, phys_dst, phys_src1, phys_src2, out);
            }
            return;

        case OP_VMOVSS:
            if (BE(c)->emit_vmovss_asm) {
                BE(c)->emit_vmovss_asm(c, phys_dst, phys_src1, out);
            }
            return;

        /* SIMD/Vector Operations - Scalar Double-Precision */
        case OP_VADDSD:
            if (BE(c)->emit_vaddsd_asm) {
                BE(c)->emit_vaddsd_asm(c, phys_dst, phys_src1, phys_src2, out);
            }
            return;

        case OP_VSUBSD:
            if (BE(c)->emit_vsubsd_asm) {
                BE(c)->emit_vsubsd_asm(c, phys_dst, phys_src1, phys_src2, out);
            }
            return;

        case OP_VMULSD:
            if (BE(c)->emit_vmulsd_asm) {
                BE(c)->emit_vmulsd_asm(c, phys_dst, phys_src1, phys_src2, out);
            }
            return;

        case OP_VDIVSD:
            if (BE(c)->emit_vdivsd_asm) {
                BE(c)->emit_vdivsd_asm(c, phys_dst, phys_src1, phys_src2, out);
            }
            return;

        case OP_VMOVSD:
            if (BE(c)->emit_vmovsd_asm) {
                BE(c)->emit_vmovsd_asm(c, phys_dst, phys_src1, out);
            }
            return;

        /* SIMD/Vector Operations - Packed Single-Precision */
        case OP_VADDPS:
            if (BE(c)->emit_vaddps_asm) {
                BE(c)->emit_vaddps_asm(c, phys_dst, phys_src1, phys_src2, out);
            }
            return;

        case OP_VSUBPS:
            if (BE(c)->emit_vsubps_asm) {
                BE(c)->emit_vsubps_asm(c, phys_dst, phys_src1, phys_src2, out);
            }
            return;

        case OP_VMULPS:
            if (BE(c)->emit_vmulps_asm) {
                BE(c)->emit_vmulps_asm(c, phys_dst, phys_src1, phys_src2, out);
            }
            return;

        case OP_VDIVPS:
            if (BE(c)->emit_vdivps_asm) {
                BE(c)->emit_vdivps_asm(c, phys_dst, phys_src1, phys_src2, out);
            }
            return;

        case OP_VMOVAPS:
            if (BE(c)->emit_vmovaps_asm) {
                BE(c)->emit_vmovaps_asm(c, phys_dst, phys_src1, out);
            }
            return;

        case OP_VANDPS:
            if (BE(c)->emit_vandps_asm) {
                BE(c)->emit_vandps_asm(c, phys_dst, phys_src1, phys_src2, out);
            }
            return;

        case OP_VORPS:
            if (BE(c)->emit_vorps_asm) {
                BE(c)->emit_vorps_asm(c, phys_dst, phys_src1, phys_src2, out);
            }
            return;

        case OP_VXORPS:
            if (BE(c)->emit_vxorps_asm) {
                BE(c)->emit_vxorps_asm(c, phys_dst, phys_src1, phys_src2, out);
            }
            return;

        case OP_VSHUFPS:
            if (BE(c)->emit_vshufps_asm) {
                BE(c)->emit_vshufps_asm(c, phys_dst, phys_src1, phys_src2, instr->imm, out);
            }
            return;

        /* SIMD/Vector Operations - Packed Double-Precision */
        case OP_VADDPD:
            if (BE(c)->emit_vaddpd_asm) {
                BE(c)->emit_vaddpd_asm(c, phys_dst, phys_src1, phys_src2, out);
            }
            return;

        case OP_VSUBPD:
            if (BE(c)->emit_vsubpd_asm) {
                BE(c)->emit_vsubpd_asm(c, phys_dst, phys_src1, phys_src2, out);
            }
            return;

        case OP_VMULPD:
            if (BE(c)->emit_vmulpd_asm) {
                BE(c)->emit_vmulpd_asm(c, phys_dst, phys_src1, phys_src2, out);
            }
            return;

        case OP_VDIVPD:
            if (BE(c)->emit_vdivpd_asm) {
                BE(c)->emit_vdivpd_asm(c, phys_dst, phys_src1, phys_src2, out);
            }
            return;

        /* SIMD/Vector Operations - Packed 32-bit Integer */
        case OP_VPADDD:
            if (BE(c)->emit_vpaddd_asm) {
                BE(c)->emit_vpaddd_asm(c, phys_dst, phys_src1, phys_src2, out);
            }
            return;

        case OP_VPSUBD:
            if (BE(c)->emit_vpsubd_asm) {
                BE(c)->emit_vpsubd_asm(c, phys_dst, phys_src1, phys_src2, out);
            }
            return;

        case OP_VPMULLD:
            if (BE(c)->emit_vpmulld_asm) {
                BE(c)->emit_vpmulld_asm(c, phys_dst, phys_src1, phys_src2, out);
            }
            return;

        case OP_VMOVAPD:
            if (BE(c)->emit_vmovapd_asm) {
                BE(c)->emit_vmovapd_asm(c, phys_dst, phys_src1, out);
            }
            return;

        /* SIMD/Vector Memory Operations */
        case OP_VLOAD:
            if (BE(c)->emit_vload_asm) {
                BE(c)->emit_vload_asm(c, phys_dst, phys_src1, instr->imm, out);
            }
            return;

        case OP_VLOADSS: {
            const char *dst_name = BE(c)->get_vec_reg_name(phys_dst);
            const char *addr_name = BE(c)->get_hw_reg_name(phys_src1);
            if (c->isa == ISA_AARCH64) {
                if (instr->imm != 0) fprintf(out, "\tldr\ts%s, [%s, #%d]\n", dst_name + 1, addr_name, instr->imm);
                else fprintf(out, "\tldr\ts%s, [%s]\n", dst_name + 1, addr_name);
                /* widen into the 64-bit double lane (FP model) */
                fprintf(out, "\tfmov\td%s, s%s\n", dst_name + 1, dst_name + 1);
            } else if (c->isa == ISA_RISCV64 || c->isa == ISA_RISCV32) {
                const char *fp_name = (phys_dst >= 0 && phys_dst < 32) ? BE(c)->regs->fp_regs[phys_dst] : "fa0";
                fprintf(out, "\tflw\t%s, %d(%s)\n", fp_name, instr->imm, addr_name);
                fprintf(out, "\tfcvt.d.s\t%s, %s\n", fp_name, fp_name);  /* widen (FP model) */
            } else {
                /* movss + cvtss2sd: 4-byte float memory -> 64-bit double
                 * register (matches the MC emitter's widening) */
                fprintf(out, "\tmovss\t%d(%%%s),%%%s\n", instr->imm, addr_name, dst_name);
                fprintf(out, "\tcvtss2sd\t%%%s,%%%s\n", dst_name, dst_name);
            }
            return;
        }

        case OP_VLOADSD: {
            const char *dst_name = BE(c)->get_vec_reg_name(phys_dst);
            const char *addr_name = BE(c)->get_hw_reg_name(phys_src1);
            if (c->isa == ISA_AARCH64) {
                if (instr->imm != 0) fprintf(out, "\tldr\td%s, [%s, #%d]\n", dst_name + 1, addr_name, instr->imm);
                else fprintf(out, "\tldr\td%s, [%s]\n", dst_name + 1, addr_name);
            } else if (c->isa == ISA_RISCV64 || c->isa == ISA_RISCV32) {
                const char *fp_name = (phys_dst >= 0 && phys_dst < 32) ? BE(c)->regs->fp_regs[phys_dst] : "fa0";
                fprintf(out, "\tfld\t%s, %d(%s)\n", fp_name, instr->imm, addr_name);
            } else {
                fprintf(out, "\tmovsd\t%d(%%%s),%%%s\n", instr->imm, addr_name, dst_name);
            }
            return;
        }

        case OP_VSTORE:
            if (BE(c)->emit_vstore_asm) {
                BE(c)->emit_vstore_asm(c, phys_src2, phys_src1, instr->imm, out);
            }
            return;

        case OP_VSTORESS: {
            const char *val_name = BE(c)->get_vec_reg_name(phys_src2);
            const char *addr_name = BE(c)->get_hw_reg_name(phys_src1);
            /* FP model: the register holds a 64-bit double. Narrow into the
             * FP scratch (outside the XMM pool, so the source survives) and
             * store 4 bytes - mirrors x86_emit_vstoress in the MC path. A bare
             * movss would store the low half of the double (0 for 1.5). */
            if (c->isa == ISA_AARCH64) {
                int s = BE(c)->fp_scratch_reg1;
                fprintf(out, "\tfcvt\ts%d, d%s\n", s, val_name + 1);
                if (instr->imm != 0) fprintf(out, "\tstr\ts%d, [%s, #%d]\n", s, addr_name, instr->imm);
                else fprintf(out, "\tstr\ts%d, [%s]\n", s, addr_name);
            } else if (c->isa == ISA_RISCV64 || c->isa == ISA_RISCV32) {
                const char *fp_name = (phys_src2 >= 0 && phys_src2 < 32) ? BE(c)->regs->fp_regs[phys_src2] : "fa0";
                int s = BE(c)->fp_scratch_reg1;
                const char *s_name = (s >= 0 && s < 32) ? BE(c)->regs->fp_regs[s] : "ft7";
                fprintf(out, "\tfcvt.s.d\t%s, %s\n", s_name, fp_name);
                fprintf(out, "\tfsw\t%s, %d(%s)\n", s_name, instr->imm, addr_name);
            } else {
                const char *s_name = BE(c)->get_vec_reg_name(BE(c)->fp_scratch_reg1);
                fprintf(out, "\tcvtsd2ss\t%%%s,%%%s\n", val_name, s_name);
                fprintf(out, "\tmovss\t%%%s,%d(%%%s)\n", s_name, instr->imm, addr_name);
            }
            return;
        }

        case OP_VSTORESD: {
            const char *val_name = BE(c)->get_vec_reg_name(phys_src2);
            const char *addr_name = BE(c)->get_hw_reg_name(phys_src1);
            if (c->isa == ISA_AARCH64) {
                if (instr->imm != 0) fprintf(out, "\tstr\td%s, [%s, #%d]\n", val_name + 1, addr_name, instr->imm);
                else fprintf(out, "\tstr\td%s, [%s]\n", val_name + 1, addr_name);
            } else if (c->isa == ISA_RISCV64 || c->isa == ISA_RISCV32) {
                const char *fp_name = (phys_src2 >= 0 && phys_src2 < 32) ? BE(c)->regs->fp_regs[phys_src2] : "fa0";
                fprintf(out, "\tfsd\t%s, %d(%s)\n", fp_name, instr->imm, addr_name);
            } else {
                fprintf(out, "\tmovsd\t%%%s,%d(%%%s)\n", val_name, instr->imm, addr_name);
            }
            return;
        }

        /* Sprint 115m: raw 4-byte ABI-float access - the register's low 32
         * bits ARE the float pattern (no widen/narrow through the
         * double-width scalar model). Used at external call boundaries. */
        case OP_VLOADSS_RAW: {
            const char *dst_name = BE(c)->get_vec_reg_name(phys_dst);
            const char *addr_name = BE(c)->get_hw_reg_name(phys_src1);
            if (c->isa == ISA_AARCH64) {
                if (instr->imm != 0) fprintf(out, "\tldr\ts%s, [%s, #%d]\n", dst_name + 1, addr_name, instr->imm);
                else fprintf(out, "\tldr\ts%s, [%s]\n", dst_name + 1, addr_name);
            } else if (c->isa == ISA_RISCV64 || c->isa == ISA_RISCV32) {
                const char *fp_name = (phys_dst >= 0 && phys_dst < 32) ? BE(c)->regs->fp_regs[phys_dst] : "fa0";
                fprintf(out, "\tflw\t%s, %d(%s)\n", fp_name, instr->imm, addr_name);
            } else {
                fprintf(out, "\tmovss\t%d(%%%s),%%%s\n", instr->imm, addr_name, dst_name);
            }
            return;
        }

        case OP_VSTORESS_RAW: {
            const char *val_name = BE(c)->get_vec_reg_name(phys_src2);
            const char *addr_name = BE(c)->get_hw_reg_name(phys_src1);
            if (c->isa == ISA_AARCH64) {
                if (instr->imm != 0) fprintf(out, "\tstr\ts%s, [%s, #%d]\n", val_name + 1, addr_name, instr->imm);
                else fprintf(out, "\tstr\ts%s, [%s]\n", val_name + 1, addr_name);
            } else if (c->isa == ISA_RISCV64 || c->isa == ISA_RISCV32) {
                const char *fp_name = (phys_src2 >= 0 && phys_src2 < 32) ? BE(c)->regs->fp_regs[phys_src2] : "fa0";
                fprintf(out, "\tfsw\t%s, %d(%s)\n", fp_name, instr->imm, addr_name);
            } else {
                fprintf(out, "\tmovss\t%%%s,%d(%%%s)\n", val_name, instr->imm, addr_name);
            }
            return;
        }

        case OP_CVTTSD2SS: {
            /* Model double -> ABI float (in-register narrow). */
            const char *dst_name = BE(c)->get_vec_reg_name(phys_dst);
            const char *src_name = BE(c)->get_vec_reg_name(phys_src1);
            if (c->isa == ISA_AARCH64) {
                fprintf(out, "\tfcvt\ts%d, d%d\n", dst_name + 1, src_name + 1);
            } else if (c->isa == ISA_RISCV64 || c->isa == ISA_RISCV32) {
                const char *d = (phys_dst >= 0 && phys_dst < 32) ? BE(c)->regs->fp_regs[phys_dst] : "ft8";
                const char *s = (phys_src1 >= 0 && phys_src1 < 32) ? BE(c)->regs->fp_regs[phys_src1] : "fa0";
                fprintf(out, "\tfcvt.s.d\t%s, %s\n", d, s);
            } else {
                fprintf(out, "\tcvtsd2ss\t%%%s,%%%s\n", src_name, dst_name);
            }
            return;
        }

        case OP_FMOV_IMM: {
            size_t rodata_offset = (size_t)instr->imm64;
            const char *dst_name = BE(c)->get_vec_reg_name(phys_dst);
            if (c->isa == ISA_AARCH64) {
                /* 8-byte double in rodata - load the d-lane, not s */
                fprintf(out, "\tadrp\tx9, .Lrodata+%zu\n", rodata_offset);
                fprintf(out, "\tldr\td%s, [x9, :lo12:.Lrodata+%zu]\n", dst_name + 1, rodata_offset);
            } else if (c->isa == ISA_RISCV64 || c->isa == ISA_RISCV32) {
                /* 8-byte double in rodata - fld, not flw.
                 * Plain-label load (binutils 2.41 mangles sym+offset on RV). */
                const char *fp_name = (phys_dst >= 0 && phys_dst < 32) ? BE(c)->regs->fp_regs[phys_dst] : "fa0";
                if (rodata_offset == 0) {
                    fprintf(out, "\tlla\tt0, .Lrodata\n");
                } else if (rodata_offset <= 2047) {
                    fprintf(out, "\tlla\tt0, .Lrodata\n");
                    fprintf(out, "\taddi\tt0, t0, %zu\n", rodata_offset);
                } else {
                    fprintf(out, "\tlla\tt0, .Lrodata\n");
                    fprintf(out, "\tli\tt1, %zu\n", rodata_offset);
                    fprintf(out, "\tadd\tt0, t0, t1\n");
                }
                fprintf(out, "\tfld\t%s, 0(t0)\n", fp_name);
            } else {
                /* rodata holds 8-byte double bit patterns (64-bit double FP
                 * model) - movsd, not movss. */
                fprintf(out, "\tmovsd\t.Lrodata+%zu(%%rip),%%%s\n", rodata_offset, dst_name);
            }
            return;
        }


        /* SIMD/Vector Conversions */
        case OP_VCVT_SI2SS:
            if (BE(c)->emit_vcvt_si2ss_asm) {
                BE(c)->emit_vcvt_si2ss_asm(c, phys_dst, phys_src1, out);
            }
            return;

        case OP_VCVT_SS2SI:
            if (BE(c)->emit_vcvt_ss2si_asm) {
                BE(c)->emit_vcvt_ss2si_asm(c, phys_dst, phys_src1, out);
            }
            return;

        /* Scalar single<->double conversions (GNU vector per-lane lowering) */
        case OP_CVTSS2SD:
            if (BE(c)->emit_cvtss2sd_asm) {
                BE(c)->emit_cvtss2sd_asm(c, phys_dst, phys_src1, out);
            }
            return;

        case OP_CVTSD2SS:
            if (BE(c)->emit_cvtsd2ss_asm) {
                BE(c)->emit_cvtsd2ss_asm(c, phys_dst, phys_src1, out);
            }
            return;

        /* SIMD/Vector Comparison */
        case OP_VCMPSS:
            if (BE(c)->emit_vcmpss_asm) {
                BE(c)->emit_vcmpss_asm(c, phys_dst, phys_src1, phys_src2, instr->imm, out);
            }
            return;

        case OP_VCMPSD:
            if (BE(c)->emit_vcmpsd_asm) {
                BE(c)->emit_vcmpsd_asm(c, phys_dst, phys_src1, phys_src2, instr->imm, out);
            }
            return;

        /* GNU __int128 (Sprint 109): 128-bit ops on a (lo,hi) GPR128 pair.
         * phys_dst/phys_src1/phys_src2 are the low halves and g128_*hi the
         * high halves, both spill-aware (see asm_emit_instruction). */
        case OP_ADD128:
            if (BE(c)->emit_add128_asm) BE(c)->emit_add128_asm(c, phys_dst, g128_dhi, phys_src1, g128_s1hi, phys_src2, g128_s2hi, out);
            return;
        case OP_SUB128:
            if (BE(c)->emit_sub128_asm) BE(c)->emit_sub128_asm(c, phys_dst, g128_dhi, phys_src1, g128_s1hi, phys_src2, g128_s2hi, out);
            return;
        case OP_MUL128:
            if (BE(c)->emit_mul128_asm) BE(c)->emit_mul128_asm(c, phys_dst, g128_dhi, phys_src1, g128_s1hi, phys_src2, g128_s2hi, out);
            return;
        case OP_AND128:
            if (BE(c)->emit_and128_asm) BE(c)->emit_and128_asm(c, phys_dst, g128_dhi, phys_src1, g128_s1hi, phys_src2, g128_s2hi, out);
            return;
        case OP_OR128:
            if (BE(c)->emit_or128_asm) BE(c)->emit_or128_asm(c, phys_dst, g128_dhi, phys_src1, g128_s1hi, phys_src2, g128_s2hi, out);
            return;
        case OP_XOR128:
            if (BE(c)->emit_xor128_asm) BE(c)->emit_xor128_asm(c, phys_dst, g128_dhi, phys_src1, g128_s1hi, phys_src2, g128_s2hi, out);
            return;
        case OP_NOT128:
            if (BE(c)->emit_not128_asm) BE(c)->emit_not128_asm(c, phys_dst, g128_dhi, phys_src1, g128_s1hi, out);
            return;
        case OP_NEG128:
            if (BE(c)->emit_neg128_asm) BE(c)->emit_neg128_asm(c, phys_dst, g128_dhi, phys_src1, g128_s1hi, out);
            return;
        case OP_MOV128:
            if (BE(c)->emit_mov128_asm) BE(c)->emit_mov128_asm(c, phys_dst, g128_dhi, phys_src1, g128_s1hi, out);
            return;
        case OP_MOV128_IMM:
            if (BE(c)->emit_mov128_imm_asm) BE(c)->emit_mov128_imm_asm(c, phys_dst, g128_dhi, instr->imm64, out);
            return;
        case OP_LOAD128:
            if (BE(c)->emit_load128_asm) BE(c)->emit_load128_asm(c, phys_dst, g128_dhi, phys_src1, 0, out);
            return;
        case OP_STORE128:
            if (BE(c)->emit_store128_asm) BE(c)->emit_store128_asm(c, phys_src1, g128_s1hi, phys_src2, 0, out);
            return;
        case OP_ZEXT64_128:
            if (BE(c)->emit_zext64_128_asm) BE(c)->emit_zext64_128_asm(c, phys_dst, g128_dhi, phys_src1, out);
            return;
        case OP_SEXT64_128:
            if (BE(c)->emit_sext64_128_asm) BE(c)->emit_sext64_128_asm(c, phys_dst, g128_dhi, phys_src1, out);
            return;
        case OP_TRUNC128_64:
            if (BE(c)->emit_trunc128_64_asm) BE(c)->emit_trunc128_64_asm(c, phys_dst, phys_src1, out);
            return;
        case OP_LO128:
            if (BE(c)->emit_mov_reg_asm) BE(c)->emit_mov_reg_asm(c, phys_dst, phys_src1, out);  /* dest = src1.lo */
            return;
        case OP_HI128:
            if (BE(c)->emit_mov_reg_asm) BE(c)->emit_mov_reg_asm(c, phys_dst, g128_s1hi, out);  /* dest = src1.hi */
            return;
        case OP_MK128:
            if (BE(c)->emit_mov_reg_asm) {
                BE(c)->emit_mov_reg_asm(c, phys_dst, phys_src1, out);  /* dest.lo = src1 */
                BE(c)->emit_mov_reg_asm(c, g128_dhi, phys_src2, out);  /* dest.hi = src2 */
            }
            return;

        /* --- Sprint 170 (rv32 i64): 64-bit (lo,hi) pair ops --- */
        case OP_ADD64:
            if (BE(c)->emit_add64_asm) BE(c)->emit_add64_asm(c, phys_dst, g64_dhi, phys_src1, g64_s1hi, phys_src2, g64_s2hi, out);
            return;
        case OP_SUB64:
            if (BE(c)->emit_sub64_asm) BE(c)->emit_sub64_asm(c, phys_dst, g64_dhi, phys_src1, g64_s1hi, phys_src2, g64_s2hi, out);
            return;
        case OP_MUL64:
            if (BE(c)->emit_mul64_asm) BE(c)->emit_mul64_asm(c, phys_dst, g64_dhi, phys_src1, g64_s1hi, phys_src2, g64_s2hi, out);
            return;
        case OP_AND64:
            if (BE(c)->emit_and64_asm) BE(c)->emit_and64_asm(c, phys_dst, g64_dhi, phys_src1, g64_s1hi, phys_src2, g64_s2hi, out);
            return;
        case OP_OR64:
            if (BE(c)->emit_or64_asm) BE(c)->emit_or64_asm(c, phys_dst, g64_dhi, phys_src1, g64_s1hi, phys_src2, g64_s2hi, out);
            return;
        case OP_XOR64:
            if (BE(c)->emit_xor64_asm) BE(c)->emit_xor64_asm(c, phys_dst, g64_dhi, phys_src1, g64_s1hi, phys_src2, g64_s2hi, out);
            return;
        case OP_NOT64:
            if (BE(c)->emit_not64_asm) BE(c)->emit_not64_asm(c, phys_dst, g64_dhi, phys_src1, g64_s1hi, out);
            return;
        case OP_NEG64:
            if (BE(c)->emit_neg64_asm) BE(c)->emit_neg64_asm(c, phys_dst, g64_dhi, phys_src1, g64_s1hi, out);
            return;
        case OP_SHL64:
            if (BE(c)->emit_shl64_asm) BE(c)->emit_shl64_asm(c, phys_dst, g64_dhi, phys_src1, g64_s1hi, phys_src2, out);
            return;
        case OP_SHR64:
            if (BE(c)->emit_shr64_asm) BE(c)->emit_shr64_asm(c, phys_dst, g64_dhi, phys_src1, g64_s1hi, phys_src2, out);
            return;
        case OP_SAR64:
            if (BE(c)->emit_sar64_asm) BE(c)->emit_sar64_asm(c, phys_dst, g64_dhi, phys_src1, g64_s1hi, phys_src2, out);
            return;
        case OP_CMP64:
            /* RISC-V: record the operand pairs for flag re-materialization.
             * Also clear cmp_is_64 on every other flag producer handled
             * below/above; the consumer emitters read c->lowering. */
            if (BE(c)->emit_cmp64_asm) BE(c)->emit_cmp64_asm(c, phys_src1, g64_s1hi, phys_src2, g64_s2hi, out);
            return;
        case OP_MOV64:
            if (BE(c)->emit_mov64_asm) BE(c)->emit_mov64_asm(c, phys_dst, g64_dhi, phys_src1, g64_s1hi, out);
            return;
        case OP_LOAD64:
            if (BE(c)->emit_load64_asm) BE(c)->emit_load64_asm(c, phys_dst, g64_dhi, phys_src1, 0, out);
            return;
        case OP_STORE64:
            if (BE(c)->emit_store64_asm) BE(c)->emit_store64_asm(c, phys_src1, g64_s1hi, phys_src2, 0, out);
            return;
        case OP_ZEXT32_64:
            if (BE(c)->emit_zext32_64_asm) BE(c)->emit_zext32_64_asm(c, phys_dst, g64_dhi, phys_src1, out);
            return;
        case OP_SEXT32_64:
            if (BE(c)->emit_sext32_64_asm) BE(c)->emit_sext32_64_asm(c, phys_dst, g64_dhi, phys_src1, out);
            return;
        case OP_TRUNC64_32:
            if (BE(c)->emit_trunc64_32_asm) BE(c)->emit_trunc64_32_asm(c, phys_dst, phys_src1, g64_s1hi, out);
            return;
        case OP_HI64:
            BE(c)->emit_mov_reg_asm(c, phys_dst, g64_s1hi, out);
            return;
        case OP_MK64:
            BE(c)->emit_mov_reg_asm(c, phys_dst, phys_src1, out);
            BE(c)->emit_mov_reg_asm(c, g64_dhi, phys_src2, out);
            return;
        case OP_MOV64_IMM:
            if (BE(c)->emit_mov64_imm_asm) BE(c)->emit_mov64_imm_asm(c, phys_dst, g64_dhi, instr->imm64, out);
            return;
        case OP_DIV64:
            if (BE(c)->emit_div64_asm) BE(c)->emit_div64_asm(c, phys_dst, g64_dhi, phys_src1, g64_s1hi, phys_src2, g64_s2hi, out);
            return;
        case OP_MOD64:
            if (BE(c)->emit_mod64_asm) BE(c)->emit_mod64_asm(c, phys_dst, g64_dhi, phys_src1, g64_s1hi, phys_src2, g64_s2hi, out);
            return;
        case OP_UDIV64:
            if (BE(c)->emit_udiv64_asm) BE(c)->emit_udiv64_asm(c, phys_dst, g64_dhi, phys_src1, g64_s1hi, phys_src2, g64_s2hi, out);
            return;
        case OP_UMOD64:
            if (BE(c)->emit_umod64_asm) BE(c)->emit_umod64_asm(c, phys_dst, g64_dhi, phys_src1, g64_s1hi, phys_src2, g64_s2hi, out);
            return;

        default:
            /* Unknown opcode - emit comment */
            fprintf(out, "\t# UNKNOWN: %s\n", ir_opcode_name(instr->op));
            return;
    }
}

/* Emit a single IR instruction as assembly */
void asm_emit_instruction(ZeroCCCompiler *c, const Instr *instr, size_t idx, size_t func_idx) {
    FILE *out = c->asm_out ? c->asm_out : stdout;

    /* Skip DEAD, NOP, and in-stream argument/label payload pseudo-instructions */
    if (instr->op == OP_DEAD || instr->op == OP_NOP ||
        instr->op == OP_CALL_ARG || instr->op == OP_ASM_ARG ||
        instr->op == OP_ASM_LABEL) {
        return;
    }

    /* Sprint 135: GNU inline asm - the backend interpolates the operand
     * specifiers into the template and writes the text. Handled before
     * phys-reg/spill resolution (operands are resolved inside the helper). */
    if (instr->op == OP_ASM) {
        if (BE(c)->emit_inline_asm_asm) {
            BE(c)->emit_inline_asm_asm(c, idx, out);
        } else {
            fprintf(out, "\t# inline asm (not supported on this backend)\n");
        }
        return;
    }

    /* Skip EPILOGUE - handled separately */
    if (instr->op == OP_EPILOGUE) {
        return;
    }

    /* Skip CALLEE_SAVE_STORES/LOADS - handled in prologue/epilogue */
    if (instr->op == OP_CALLEE_SAVE_STORES || instr->op == OP_CALLEE_SAVE_LOADS) {
        return;
    }

    /* Get physical registers */
    int phys_dst = instr->temp_dest== TEMP_NONE ? BE(c)->fp_reg : asm_get_phys_reg(&c->regalloc, instr->temp_dest);
    int phys_src1 = instr->temp_src1== TEMP_NONE ? BE(c)->fp_reg : asm_get_phys_reg(&c->regalloc, instr->temp_src1);
    int phys_src2 = instr->temp_src2== TEMP_NONE ? BE(c)->fp_reg : asm_get_phys_reg(&c->regalloc, instr->temp_src2);

    /* Check for spilled temps that need reloading / storing */
    int32_t spill_base_offset = c->regalloc.spill_base_offset;
    int spill_src1 = (instr->op == OP_CALL || instr->op == OP_SYSCALL || instr->op == OP_CLONE) ? -1 : regalloc_get_spill_slot_for_temp(&c->regalloc, instr->temp_src1);
    int spill_src2 = (instr->op == OP_CALL || instr->op == OP_SYSCALL || instr->op == OP_CLONE) ? -1 : regalloc_get_spill_slot_for_temp(&c->regalloc, instr->temp_src2);
    int spill_dst  = (instr->op == OP_CALL || instr->op == OP_SYSCALL || instr->op == OP_CLONE) ? -1 : regalloc_get_spill_slot_for_temp(&c->regalloc, instr->temp_dest);

    /* --- 128-bit (GPR128) spill handling (mirrors ast_lowering_mc.c) ---
     * A spilled REG_CLASS_GPR128 temp must reload BOTH halves (slot and
     * slot+8) into a reserved (lo,hi) scratch pair, and a spilled dst must
     * store BOTH halves back. The scratch pairs are reserved (excluded from
     * the 64-bit and GPR128 value pools) so they never hold a live value.
     * Assignment order: src1 -> pair0, src2 -> pair1, dst -> pair0 if free
     * else pair1, else reuse src1's pair in-place (the 128-bit emits
     * tolerate d==a aliasing). */
    bool op128 = (instr->op == OP_ADD128 || instr->op == OP_SUB128 ||
                  instr->op == OP_MUL128 || instr->op == OP_AND128 ||
                  instr->op == OP_OR128  || instr->op == OP_XOR128 ||
                  instr->op == OP_NOT128 || instr->op == OP_NEG128 ||
                  instr->op == OP_MOV128 || instr->op == OP_MOV128_IMM ||
                  instr->op == OP_ZEXT64_128 || instr->op == OP_SEXT64_128 ||
                  instr->op == OP_TRUNC128_64 ||
                  instr->op == OP_LO128 || instr->op == OP_HI128 ||
                  instr->op == OP_MK128 ||
                  instr->op == OP_SHL128 || instr->op == OP_SHR128 || instr->op == OP_SAR128 ||
                  instr->op == OP_CMP128 ||
                  instr->op == OP_LOAD128 || instr->op == OP_STORE128);
    /* Operand widths are decided by the op's semantics, not the temp's
     * allocated class: temp IDs are reused and the live-range class array is
     * last-write-wins, so a 64-bit operand (MK128 halves, cast sources, shift
     * counts, memory base pointers) can wear a stale REG_CLASS_GPR128.
     * Mirrors ast_lowering_mc.c. (Sprint 109.) */
    bool d_is_128 = op128 && !
        (instr->op == OP_LO128 || instr->op == OP_HI128 || instr->op == OP_TRUNC128_64);
    bool s1_is_128 = instr->op == OP_ADD128 || instr->op == OP_SUB128 ||
        instr->op == OP_MUL128 || instr->op == OP_AND128 ||
        instr->op == OP_OR128  || instr->op == OP_XOR128 ||
        instr->op == OP_NOT128 || instr->op == OP_NEG128 ||
        instr->op == OP_MOV128 || instr->op == OP_SHL128 ||
        instr->op == OP_SHR128 || instr->op == OP_SAR128 ||
        instr->op == OP_LO128  || instr->op == OP_HI128 ||
        instr->op == OP_CMP128 || instr->op == OP_STORE128 ||
        instr->op == OP_TRUNC128_64;
    bool s2_is_128 = instr->op == OP_ADD128 || instr->op == OP_SUB128 ||
        instr->op == OP_MUL128 || instr->op == OP_AND128 ||
        instr->op == OP_OR128  || instr->op == OP_XOR128 ||
        instr->op == OP_CMP128;
    bool d128_sp = d_is_128 && spill_dst >= 0;
    bool s128_sp = op128 && s1_is_128 && spill_src1 >= 0;
    bool s228_sp = op128 && s2_is_128 && spill_src2 >= 0;
    int g128_np = BE(c)->g128_scratch_pair_count;
    int pair_s1 = -1, pair_s2 = -1, pair_dst = -1;
    if (s128_sp && g128_np > 0) pair_s1 = 0;
    if (s228_sp && g128_np > 1) pair_s2 = 1;
    if (d128_sp) {
        if (g128_np > 0 && pair_s1 < 0) pair_dst = 0;
        else if (g128_np > 1 && pair_s2 < 0) pair_dst = 1;
        else pair_dst = pair_s1;  /* all three spilled: in-place into src1's pair */
    }
    int g128_dlo = phys_dst, g128_dhi = regalloc_get_reg_hi_for_temp(&c->regalloc, instr->temp_dest);
    int g128_s1lo = phys_src1, g128_s1hi = regalloc_get_reg_hi_for_temp(&c->regalloc, instr->temp_src1);
    int g128_s2lo = phys_src2, g128_s2hi = regalloc_get_reg_hi_for_temp(&c->regalloc, instr->temp_src2);
    /* MUL128 with the dst AND both sources spilled: compute directly between
     * the spill slots (mirrors ast_lowering_mc.c). The register-form emit
     * writes d partway through the sequence, so d may not alias a source,
     * and on a single-scratch-pair ISA no pair remains for the third value. */
    if (instr->op == OP_MUL128 && d128_sp && s128_sp && s228_sp &&
        BE(c)->emit_mul128_spilled_asm) {
        int32_t off_d = spill_base_offset + (spill_dst + 1) * 8;
        int32_t off_a = spill_base_offset + (spill_src1 + 1) * 8;
        int32_t off_b = spill_base_offset + (spill_src2 + 1) * 8;
        BE(c)->emit_mul128_spilled_asm(c,
            zero_cc_frame_disp(c, -off_d), zero_cc_frame_disp(c, -off_d - 8),
            zero_cc_frame_disp(c, -off_a), zero_cc_frame_disp(c, -off_a - 8),
            zero_cc_frame_disp(c, -off_b), zero_cc_frame_disp(c, -off_b - 8), out);
        return;
    }
    /* The 5 two-step 128-bit emits are NOT d==b alias-safe. When BOTH the
     * dst and src2 are spilled on a 1-scratch-pair ISA, compute directly
     * between frame slots (mirrors ast_lowering_mc.c). */
    if ((instr->op == OP_ADD128 || instr->op == OP_SUB128 ||
         instr->op == OP_AND128 || instr->op == OP_OR128 ||
         instr->op == OP_XOR128) &&
        d128_sp && s228_sp && g128_np <= 1 &&
        BE(c)->emit_bin128_sp_asm) {
        int32_t off_d = spill_base_offset + (spill_dst + 1) * 8;
        int32_t off_a = spill_base_offset + (spill_src1 + 1) * 8;
        int32_t off_b = spill_base_offset + (spill_src2 + 1) * 8;
        BE(c)->emit_bin128_sp_asm(c, (int)instr->op,
            s128_sp ? 1 : 0,
            s128_sp ? (int)zero_cc_frame_disp(c, -off_a) : phys_src1,
            s128_sp ? (int)zero_cc_frame_disp(c, -off_a - 8)
                    : regalloc_get_reg_hi_for_temp(&c->regalloc, instr->temp_src1),
            zero_cc_frame_disp(c, -off_b), zero_cc_frame_disp(c, -off_b - 8),
            zero_cc_frame_disp(c, -off_d), zero_cc_frame_disp(c, -off_d - 8), out);
        return;
    }
    /* STORE128 with both the 128-bit value and the 64-bit base spilled:
     * the generic base reload targets scratch_reg2, which on x86-64 (r11)
     * and RISC-V (t1) aliases a half of the GPR128 pair the value reloads
     * into. Stream instead (mirrors ast_lowering_mc.c, Sprint 109). */
    if (instr->op == OP_STORE128 && s128_sp && spill_src2 >= 0) {
        int32_t off_v = spill_base_offset + (spill_src1 + 1) * 8;
        int32_t off_b = spill_base_offset + (spill_src2 + 1) * 8;
        int base_r = BE(c)->scratch_reg2, tmp_r = BE(c)->scratch_reg1;
        BE(c)->emit_load_asm(c, base_r, zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -off_b), out);
        BE(c)->emit_load_asm(c, tmp_r, zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -off_v), out);
        BE(c)->emit_store_asm(c, tmp_r, base_r, 0, out);
        BE(c)->emit_load_asm(c, tmp_r, zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -off_v - 8), out);
        BE(c)->emit_store_asm(c, tmp_r, base_r, 8, out);
        return;
    }
    /* MK128 with a spilled 128-bit dst: stream each half from its home
     * (register or slot) straight into the dst slots (mirrors MC). */
    if (instr->op == OP_MK128 && d128_sp) {
        int32_t off_d = spill_base_offset + (spill_dst + 1) * 8;
        int tmp = BE(c)->scratch_reg1;
        if (spill_src1 >= 0) {
            int32_t off_s1 = spill_base_offset + (spill_src1 + 1) * 8;
            BE(c)->emit_load_asm(c, tmp, zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -off_s1), out);
        }
        BE(c)->emit_store_asm(c, spill_src1 >= 0 ? tmp : phys_src1, zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -off_d), out);
        if (spill_src2 >= 0) {
            int32_t off_s2 = spill_base_offset + (spill_src2 + 1) * 8;
            BE(c)->emit_load_asm(c, tmp, zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -off_s2), out);
        }
        BE(c)->emit_store_asm(c, spill_src2 >= 0 ? tmp : phys_src2, zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -off_d - 8), out);
        return;
    }
    /* MUL128 with a MIX of spilled/register operands on a 1-scratch-pair
     * ISA (Sprint 109, mirrors ast_lowering_mc.c): the pair logic below
     * cannot express these. Each operand keeps its natural home. */
    if (instr->op == OP_MUL128 && g128_np <= 1 &&
        (d128_sp || s128_sp || s228_sp) &&
        !(d128_sp && s128_sp && s228_sp) &&
        BE(c)->emit_mul128_mixed_asm) {
        int32_t off_d = spill_base_offset + (spill_dst + 1) * 8;
        int32_t off_a = spill_base_offset + (spill_src1 + 1) * 8;
        int32_t off_b = spill_base_offset + (spill_src2 + 1) * 8;
        int d_lo = phys_dst, d_hi = regalloc_get_reg_hi_for_temp(&c->regalloc, instr->temp_dest);
        int a_lo = phys_src1, a_hi = regalloc_get_reg_hi_for_temp(&c->regalloc, instr->temp_src1);
        int b_lo = phys_src2, b_hi = regalloc_get_reg_hi_for_temp(&c->regalloc, instr->temp_src2);
        BE(c)->emit_mul128_mixed_asm(c,
            d128_sp ? 1 : 0,
            d128_sp ? (int)zero_cc_frame_disp(c, -off_d) : d_lo,
            d128_sp ? (int)zero_cc_frame_disp(c, -off_d - 8) : d_hi,
            s128_sp ? 1 : 0,
            s128_sp ? (int)zero_cc_frame_disp(c, -off_a) : a_lo,
            s128_sp ? (int)zero_cc_frame_disp(c, -off_a - 8) : a_hi,
            s228_sp ? 1 : 0,
            s228_sp ? (int)zero_cc_frame_disp(c, -off_b) : b_lo,
            s228_sp ? (int)zero_cc_frame_disp(c, -off_b - 8) : b_hi, out);
        return;
    }
    if (s128_sp && pair_s1 >= 0) {
        int32_t off = spill_base_offset + (spill_src1 + 1) * 8;
        if (BE(c)->emit_load_asm) {
            BE(c)->emit_load_asm(c, BE(c)->g128_scratch_lo[pair_s1], zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -off), out);
            BE(c)->emit_load_asm(c, BE(c)->g128_scratch_hi[pair_s1], zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -off - 8), out);
        }
        g128_s1lo = BE(c)->g128_scratch_lo[pair_s1];
        g128_s1hi = BE(c)->g128_scratch_hi[pair_s1];
    }
    if (s228_sp && pair_s2 >= 0) {
        int32_t off = spill_base_offset + (spill_src2 + 1) * 8;
        if (BE(c)->emit_load_asm) {
            BE(c)->emit_load_asm(c, BE(c)->g128_scratch_lo[pair_s2], zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -off), out);
            BE(c)->emit_load_asm(c, BE(c)->g128_scratch_hi[pair_s2], zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -off - 8), out);
        }
        g128_s2lo = BE(c)->g128_scratch_lo[pair_s2];
        g128_s2hi = BE(c)->g128_scratch_hi[pair_s2];
    } else if (s228_sp && !d128_sp && g128_np <= 1 && pair_s2 < 0 &&
               (instr->op == OP_ADD128 || instr->op == OP_SUB128 ||
                instr->op == OP_AND128 || instr->op == OP_OR128 ||
                instr->op == OP_XOR128)) {
        /* 1-pair ISA, dst register-resident, src2 spilled: the two-step
         * emits are d==b unsafe, so src2 goes to pair0 instead of the dst
         * pair. If src1 is also spilled it owns pair0, so relocate src1
         * into the dst's register pair (d==a IS safe: the initial mov is
         * skipped and every source read precedes the destination write).
         * (Mirrors ast_lowering_mc.c, Sprint 109.) */
        int32_t off = spill_base_offset + (spill_src2 + 1) * 8;
        if (spill_src1 < 0) {
            if (BE(c)->emit_load_asm) {
                BE(c)->emit_load_asm(c, BE(c)->g128_scratch_lo[0], zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -off), out);
                BE(c)->emit_load_asm(c, BE(c)->g128_scratch_hi[0], zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -off - 8), out);
            }
            g128_s2lo = BE(c)->g128_scratch_lo[0];
            g128_s2hi = BE(c)->g128_scratch_hi[0];
        } else {
            int dhi_r = regalloc_get_reg_hi_for_temp(&c->regalloc, instr->temp_dest);
            int32_t off_s1 = spill_base_offset + (spill_src1 + 1) * 8;
            BE(c)->emit_load_asm(c, phys_dst, zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -off_s1), out);
            BE(c)->emit_load_asm(c, dhi_r, zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -off_s1 - 8), out);
            g128_s1lo = phys_dst;
            g128_s1hi = dhi_r;
            BE(c)->emit_load_asm(c, BE(c)->g128_scratch_lo[0], zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -off), out);
            BE(c)->emit_load_asm(c, BE(c)->g128_scratch_hi[0], zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -off - 8), out);
            g128_s2lo = BE(c)->g128_scratch_lo[0];
            g128_s2hi = BE(c)->g128_scratch_hi[0];
        }
    } else if (s228_sp && !d128_sp && pair_s1 < 0 && spill_src1 < 0 && g128_np > 0) {
        /* 1-pair ISA fallback for the remaining ops (MUL128, CMP128):
         * reload src2 into pair0; safe when src1 is fully register-resident.
         * (Mirrors ast_lowering_mc.c.) */
        int32_t off = spill_base_offset + (spill_src2 + 1) * 8;
        if (BE(c)->emit_load_asm) {
            BE(c)->emit_load_asm(c, BE(c)->g128_scratch_lo[0], zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -off), out);
            BE(c)->emit_load_asm(c, BE(c)->g128_scratch_hi[0], zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -off - 8), out);
        }
        g128_s2lo = BE(c)->g128_scratch_lo[0];
        g128_s2hi = BE(c)->g128_scratch_hi[0];
    }
    if (d128_sp) {
        if (pair_dst >= 0 && pair_dst != pair_s1) {
            g128_dlo = BE(c)->g128_scratch_lo[pair_dst];
            g128_dhi = BE(c)->g128_scratch_hi[pair_dst];
        } else if (pair_dst == pair_s1) {
            g128_dlo = g128_s1lo; g128_dhi = g128_s1hi;  /* in-place */
        }
    }
    if (op128) {
        phys_dst = g128_dlo; phys_src1 = g128_s1lo; phys_src2 = g128_s2lo;
    }

    /* --- 64-bit (GPR64) spill handling (Sprint 170 rv32 i64, mirrors MC) ---
     * Spilled pair operands reload into the reserved scratch pairs (lo at
     * slot*8, hi at slot*8+4 - ONE 8-byte slot), a spilled dst stores back
     * from its pair. A 64-bit shift count spilled while a pair scratch is in
     * use goes to scratch_reg1 (the generic src2 target is pair0's low half). */
    bool op64 = (instr->op == OP_ADD64 || instr->op == OP_SUB64 ||
                 instr->op == OP_MUL64 || instr->op == OP_AND64 ||
                 instr->op == OP_OR64  || instr->op == OP_XOR64 ||
                 instr->op == OP_NOT64 || instr->op == OP_NEG64 ||
                 instr->op == OP_SHL64 || instr->op == OP_SHR64 ||
                 instr->op == OP_SAR64 || instr->op == OP_CMP64 ||
                 instr->op == OP_MOV64 || instr->op == OP_LOAD64 ||
                 instr->op == OP_STORE64 || instr->op == OP_MOV64_IMM ||
                 instr->op == OP_ZEXT32_64 || instr->op == OP_SEXT32_64 ||
                 instr->op == OP_TRUNC64_32 ||
                 instr->op == OP_HI64 || instr->op == OP_MK64 ||
                 instr->op == OP_DIV64 || instr->op == OP_MOD64 ||
                 instr->op == OP_UDIV64 || instr->op == OP_UMOD64);
    bool d64_is = op64 && !(instr->op == OP_CMP64 || instr->op == OP_STORE64 ||
                            instr->op == OP_TRUNC64_32 || instr->op == OP_HI64);
    bool s1_64 = instr->op == OP_ADD64 || instr->op == OP_SUB64 ||
        instr->op == OP_MUL64 || instr->op == OP_AND64 ||
        instr->op == OP_OR64  || instr->op == OP_XOR64 ||
        instr->op == OP_NOT64 || instr->op == OP_NEG64 ||
        instr->op == OP_SHL64 || instr->op == OP_SHR64 ||
        instr->op == OP_SAR64 || instr->op == OP_CMP64 ||
        instr->op == OP_MOV64 || instr->op == OP_STORE64 ||
        instr->op == OP_TRUNC64_32 || instr->op == OP_HI64 ||
        instr->op == OP_DIV64 || instr->op == OP_MOD64 ||
        instr->op == OP_UDIV64 || instr->op == OP_UMOD64;
    bool s2_64 = instr->op == OP_ADD64 || instr->op == OP_SUB64 ||
        instr->op == OP_MUL64 || instr->op == OP_AND64 ||
        instr->op == OP_OR64  || instr->op == OP_XOR64 ||
        instr->op == OP_CMP64 ||
        instr->op == OP_DIV64 || instr->op == OP_MOD64 ||
        instr->op == OP_UDIV64 || instr->op == OP_UMOD64;
    bool d64_sp = d64_is && spill_dst >= 0;
    bool s164_sp = op64 && s1_64 && spill_src1 >= 0;
    bool s264_sp = op64 && s2_64 && spill_src2 >= 0;
    int g64_np = BE(c)->g128_scratch_pair_count;
    int p64_s1 = -1, p64_s2 = -1, p64_dst = -1;
    if (s164_sp && g64_np > 0) p64_s1 = 0;
    if (s264_sp && g64_np > 1) p64_s2 = 1;
    if (d64_sp) {
        if (g64_np > 0 && p64_s1 < 0) p64_dst = 0;
        else if (g64_np > 1 && p64_s2 < 0) p64_dst = 1;
        else p64_dst = p64_s1;  /* all three spilled: in-place into src1's pair */
    }
    int g64_dlo = phys_dst, g64_dhi = regalloc_get_reg_hi_for_temp(&c->regalloc, instr->temp_dest);
    int g64_s1lo = phys_src1, g64_s1hi = regalloc_get_reg_hi_for_temp(&c->regalloc, instr->temp_src1);
    int g64_s2lo = phys_src2, g64_s2hi = regalloc_get_reg_hi_for_temp(&c->regalloc, instr->temp_src2);
    /* STORE64 with the 64-bit value AND the 32-bit base spilled: reload the
     * base to scratch_reg1 (the generic src2 target is pair0's low half) and
     * store straight from the value pair (mirrors MC). */
    if (instr->op == OP_STORE64 && s164_sp && spill_src2 >= 0) {
        int32_t off_v = spill_base_offset + (spill_src1 + 1) * 8;
        int32_t off_b = spill_base_offset + (spill_src2 + 1) * 8;
        int base_r = BE(c)->scratch_reg1, tmp_r = BE(c)->scratch_reg2;
        BE(c)->emit_load_asm(c, base_r, zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -off_b), out);
        if (g64_np > 0) {
            BE(c)->emit_load_asm(c, BE(c)->g128_scratch_lo[0], zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -off_v), out);
            BE(c)->emit_load_asm(c, BE(c)->g128_scratch_hi[0], zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -off_v - 4), out);
            BE(c)->emit_store_asm(c, BE(c)->g128_scratch_lo[0], base_r, 0, out);
            BE(c)->emit_store_asm(c, BE(c)->g128_scratch_hi[0], base_r, 4, out);
        } else {
            BE(c)->emit_load_asm(c, tmp_r, zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -off_v), out);
            BE(c)->emit_store_asm(c, tmp_r, base_r, 0, out);
            BE(c)->emit_load_asm(c, tmp_r, zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -off_v - 4), out);
            BE(c)->emit_store_asm(c, tmp_r, base_r, 4, out);
        }
        return;  /* result is in memory; nothing to store back */
    }
    if (instr->op == OP_MK64 && d64_sp) {
        int32_t off_d = spill_base_offset + (spill_dst + 1) * 8;
        int tmp = BE(c)->scratch_reg1;
        if (spill_src1 >= 0) {
            int32_t off_s1 = spill_base_offset + (spill_src1 + 1) * 8;
            BE(c)->emit_load_asm(c, tmp, zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -off_s1), out);
        }
        BE(c)->emit_store_asm(c, spill_src1 >= 0 ? tmp : phys_src1, zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -off_d), out);
        if (spill_src2 >= 0) {
            int32_t off_s2 = spill_base_offset + (spill_src2 + 1) * 8;
            BE(c)->emit_load_asm(c, tmp, zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -off_s2), out);
        }
        BE(c)->emit_store_asm(c, spill_src2 >= 0 ? tmp : phys_src2, zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -off_d - 4), out);
        return;
    }
    if (s164_sp && p64_s1 >= 0) {
        int32_t off = spill_base_offset + (spill_src1 + 1) * 8;
        if (BE(c)->emit_load_asm) {
            BE(c)->emit_load_asm(c, BE(c)->g128_scratch_lo[p64_s1], zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -off), out);
            BE(c)->emit_load_asm(c, BE(c)->g128_scratch_hi[p64_s1], zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -off - 4), out);
        }
        g64_s1lo = BE(c)->g128_scratch_lo[p64_s1];
        g64_s1hi = BE(c)->g128_scratch_hi[p64_s1];
    }
    if (s264_sp && p64_s2 >= 0) {
        int32_t off = spill_base_offset + (spill_src2 + 1) * 8;
        if (BE(c)->emit_load_asm) {
            BE(c)->emit_load_asm(c, BE(c)->g128_scratch_lo[p64_s2], zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -off), out);
            BE(c)->emit_load_asm(c, BE(c)->g128_scratch_hi[p64_s2], zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -off - 4), out);
        }
        g64_s2lo = BE(c)->g128_scratch_lo[p64_s2];
        g64_s2hi = BE(c)->g128_scratch_hi[p64_s2];
    }
    if (d64_sp) {
        if (p64_dst >= 0 && p64_dst != p64_s1) {
            g64_dlo = BE(c)->g128_scratch_lo[p64_dst];
            g64_dhi = BE(c)->g128_scratch_hi[p64_dst];
        } else if (p64_dst == p64_s1) {
            g64_dlo = g64_s1lo; g64_dhi = g64_s1hi;  /* in-place */
        }
    }
    bool sh64_cnt_sp = (instr->op == OP_SHL64 || instr->op == OP_SHR64 ||
                        instr->op == OP_SAR64) && spill_src2 >= 0 &&
                       (s164_sp || d64_sp);
    if (sh64_cnt_sp) {
        int32_t slot_off = spill_base_offset + (spill_src2 + 1) * 8;
        if (BE(c)->emit_load_asm) {
            BE(c)->emit_load_asm(c, BE(c)->scratch_reg1, zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -slot_off), out);
        }
        phys_src2 = BE(c)->scratch_reg1;
    }
    if (op64) {
        phys_dst = g64_dlo; phys_src1 = g64_s1lo; phys_src2 = g64_s2lo;
    }

    /* Reload spilled sources into scratch registers. FP-class values use
     * the FP scratch XMMs (emit_load_fp_asm); GPR values use the GPR
     * scratches (C8). GPR128 operands are handled above (skipped here). */
    if (spill_src1 >= 0 && !s128_sp && !s164_sp) {
        int32_t slot_off = spill_base_offset + (spill_src1 + 1) * 8;
        if (regalloc_get_class_for_temp(&c->regalloc, instr->temp_src1) == REG_CLASS_XMM) {
            if (BE(c)->emit_load_fp_asm) {
                BE(c)->emit_load_fp_asm(c, BE(c)->fp_scratch_reg1, zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -slot_off), out);
            }
            phys_src1 = BE(c)->fp_scratch_reg1;
        } else {
            if (BE(c)->emit_load_asm) {
                BE(c)->emit_load_asm(c, BE(c)->scratch_reg1, zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -slot_off), out);
            }
            phys_src1 = BE(c)->scratch_reg1;
        }
    }
    if (spill_src2 >= 0 && !s228_sp && !s264_sp && !sh64_cnt_sp) {
        int32_t slot_off = spill_base_offset + (spill_src2 + 1) * 8;
        if (regalloc_get_class_for_temp(&c->regalloc, instr->temp_src2) == REG_CLASS_XMM) {
            if (BE(c)->emit_load_fp_asm) {
                BE(c)->emit_load_fp_asm(c, BE(c)->fp_scratch_reg2, zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -slot_off), out);
            }
            phys_src2 = BE(c)->fp_scratch_reg2;
        } else {
            if (BE(c)->emit_load_asm) {
                BE(c)->emit_load_asm(c, BE(c)->scratch_reg2, zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -slot_off), out);
            }
            phys_src2 = BE(c)->scratch_reg2;
        }
    }
    if (spill_dst >= 0 && !d128_sp && !d64_sp) {
        if (regalloc_get_class_for_temp(&c->regalloc, instr->temp_dest) == REG_CLASS_XMM) {
            phys_dst = BE(c)->fp_scratch_reg1;
        } else {
            phys_dst = BE(c)->scratch_reg1;
        }
    }

    asm_emit_instruction_core(c, instr, idx, func_idx, phys_dst, phys_src1, phys_src2,
                              g128_dhi, g128_s1hi, g128_s2hi,
                              g64_dhi, g64_s1hi, g64_s2hi, out);

    /* C99 width semantics (Sprint 119, slice 2): canonicalize a
     * narrow-int result to its C width before any spill-back so the
     * stored value is the extended one. Mark-first: a C99 width mark
     * is a per-op parameter (byte widths 1/2/4 signed or 8+1/2/4
     * unsigned, plus bitfield bit widths 3..7) and a marked op is
     * never OVF-adjacent - the zero_san trap consumes the mark slot
     * (see below). OP_OVF_TRAP is never extended itself: its width is
     * CHECK metadata, not a value mark.
     *
     * Clean-value deferral: when the preceding ALU op's mark was
     * stolen by this OP_OVF_TRAP, the check must see the raw 64-bit
     * result and the flags the ALU op set. The trap frame therefore
     * emits the ALU result's tag-driven C99 extend AFTER the check
     * (same temp => same physical register). On values that survive
     * the check that deferred extend is a provable no-op: the check
     * traps exactly when the extension would change the value. An
     * in-place SEXTn/ZEXTn on the same temp after the trap makes the
     * deferred extend redundant too. */
    {
        const Instr *ext = NULL;
        int ew = 0, esign = 0;
        const Instr *buf = ir_get_buffer(c);
        size_t n = ir_get_count(c);
        if (instr->op == OP_OVF_TRAP && idx > 0) {
            const Instr *pv = &buf[idx - 1];
            if (pv->op != OP_OVF_TRAP && pv->temp_dest != TEMP_NONE &&
                pv->temp_dest == instr->temp_dest &&
                zc_tag_extend(c, pv, &ew, &esign) &&
                !zc_next_canonicalizes(c, idx + 1 < n ? &buf[idx + 1] : NULL,
                                       instr->temp_dest)) {
                ext = pv;
            }
        } else if (instr->op != OP_OVF_TRAP && instr->temp_dest != TEMP_NONE &&
                   zc_mark_extend(instr, &ew, &esign)) {
            ext = instr;
        }
        if (ext && BE(c)->emit_extend_asm) {
            BE(c)->emit_extend_asm(c, phys_dst, phys_dst, ew, esign, out);
        }
    }

    /* If destination temp was spilled, store the scratch register back to
     * the frame spill slot (FP scratch for FP-class). A spilled GPR128 dst
     * stores BOTH halves to the two slots.
     * OP_OVF_TRAP is a pure check: the preceding op's own store-back
     * already persisted the value, so re-storing here would overwrite the
     * slot with scratch-register garbage on ISAs where the check is skipped. */
    if (spill_dst >= 0 && instr->op != OP_OVF_TRAP) {
        int32_t slot_off = spill_base_offset + (spill_dst + 1) * 8;
        if (d128_sp) {
            if (BE(c)->emit_store_asm) {
                BE(c)->emit_store_asm(c, g128_dlo, zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -slot_off), out);
                BE(c)->emit_store_asm(c, g128_dhi, zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -slot_off - 8), out);
            }
        } else if (d64_sp) {
            /* Sprint 170 (rv32 i64): ONE 8-byte slot - lo at slot*8, hi at
             * slot*8+4. */
            if (BE(c)->emit_store_asm) {
                BE(c)->emit_store_asm(c, g64_dlo, zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -slot_off), out);
                BE(c)->emit_store_asm(c, g64_dhi, zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -slot_off - 4), out);
            }
        } else if (regalloc_get_class_for_temp(&c->regalloc, instr->temp_dest) == REG_CLASS_XMM) {
            if (BE(c)->emit_store_fp_asm) {
                BE(c)->emit_store_fp_asm(c, BE(c)->fp_scratch_reg1, zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -slot_off), out);
            }
        } else if (BE(c)->emit_store_asm) {
            BE(c)->emit_store_asm(c, BE(c)->scratch_reg1, zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -slot_off), out);
        }
    }
}

/* Emit all IR instructions as assembly */
void emit_ir_to_asm(ZeroCCCompiler *c, int32_t spill_base_offset) {
    c->regalloc.spill_base_offset = spill_base_offset;
    c->asm_call_seq = 0;  /* per-function OP_CALL sequence (ir_ctx tables) */

    /* Get IR from buffer */
    const Instr *instrs = ir_get_buffer(c);
    size_t count = ir_get_count(c);
    size_t func_idx = c->lowering.current_func_idx;

    /* Emit all IR instructions */
    for (size_t i = 0; i < count; i++) {
        asm_emit_instruction(c, &instrs[i], i, func_idx);
    }

    /* Emit epilogue */
    if (c->lowering.has_stack_protector && BE(c)->emit_stack_guard_check_asm) {
        FILE *out = c->asm_out ? c->asm_out : stdout;
        BE(c)->emit_stack_guard_check_asm(c, c->lowering.canary_stack_off, out);
    }
    if (BE(c)->emit_epilogue_asm) {
        BE(c)->emit_epilogue_asm(c);
    }
}
