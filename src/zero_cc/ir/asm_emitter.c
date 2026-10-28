/* Assembly Emitter - IR to Assembly Translation
 *
 * This module implements ISA-agnostic assembly emission from IR.
 * It delegates to backend-specific functions for ISA-specific assembly generation.
 *
 * Option A: Full Backend-Driven Architecture
 * All assembly emission is delegated to BE(c)->emit_*_asm() functions.
 */

#include "zero_cc/ir/asm_emitter.h"
#include "zero_cc/ir/ir.h"
#include "zero_cc/ir/regalloc.h"
#include "zero_cc/isa/isa_backend.h"
#include <stdio.h>
#include <string.h>

/* Get physical register for a temporary */
static inline int asm_get_phys_reg(RegAllocator *ra, int temp) {
    return regalloc_get_reg_for_temp(ra, temp);
}

/* Emit function label (.globl and func:) */
void emit_function_label_asm(ZeroCCCompiler *c) {
    FILE *out = c->asm_out ? c->asm_out : stdout;
    size_t func_idx = c->function_count - 1;

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
}



/* =========================================================================
 * Core Emission Functions - Delegate to Backend
 * ========================================================================= */

static void asm_emit_instruction_core(ZeroCCCompiler *c, const Instr *instr, size_t idx, size_t func_idx,
                                      int phys_dst, int phys_src1, int phys_src2, FILE *out) {
    switch (instr->op) {
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
            } else if (c->isa == ISA_RISCV64 || c->isa == ISA_RISCV32) {
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

        case OP_MUL:
            if (BE(c)->emit_mul_asm) {
                BE(c)->emit_mul_asm(c, phys_dst, phys_src1, phys_src2, out);
            }
            return;

        case OP_DIV:
            if (BE(c)->emit_div_asm) {
                c->lowering.div_is_unsigned = (instr->cond != 0 || instr->width > 8);
                BE(c)->emit_div_asm(c, phys_dst, phys_src1, phys_src2, out);
                c->lowering.div_is_unsigned = 0;
            }
            return;

        case OP_MOD:
            if (BE(c)->emit_mod_asm) {
                c->lowering.div_is_unsigned = (instr->cond != 0 || instr->width > 8);
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
            if ((rc1 == REG_CLASS_XMM || rc1 == REG_CLASS_VEC ||
                 rc2 == REG_CLASS_XMM || rc2 == REG_CLASS_VEC)
                && BE(c)->emit_fp_cmp_asm) {
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
         * riscv `mv t0,r` (cond_set reads t0). (Sprint 41: was x86-hardcoded
         * `testq`, which broke aarch64/riscv -S output. */
        case OP_CMP_ZERO: {
            const char *r = BE(c)->get_hw_reg_name(phys_src1);
            if (c->isa == ISA_AARCH64) {
                fprintf(out, "\tcmp\t%s, #0\n", r);
            } else if (c->isa == ISA_RISCV64 || c->isa == ISA_RISCV32) {
                fprintf(out, "\tmv\tt0, %s\n", r);
            } else {
                fprintf(out, "\ttestq\t%%%s,%%%s\n", r, r);
            }
            return;
        }

        /* Control Flow */
        case OP_LABEL:
            if (BE(c)->emit_label_asm) {
                char lbl_buf[64];
                snprintf(lbl_buf, sizeof(lbl_buf), ".L%zu_%u", func_idx, (unsigned)instr->label_idx);
                BE(c)->emit_label_asm(c, lbl_buf, out);
            }
            return;

        case OP_JMP: {
            /* Fallthrough optimization: skip jump if next active instruction is target label */
            size_t count = ir_get_count();
            const Instr *buf = ir_get_buffer();
            size_t next_idx = idx + 1;
            while (next_idx < count && (buf[next_idx].op == OP_DEAD || buf[next_idx].op == OP_NOP)) {
                next_idx++;
            }
            if (next_idx < count && buf[next_idx].op == OP_LABEL && buf[next_idx].label_idx == instr->label_idx) {
                return;
            }
            /* Option 2: Inline epilogue on early returns when no static-local mirror or CSR restore is required */
            if (instr->label_idx == c->lowering.epilogue_label &&
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
                fprintf(out, "\tlla\t%s, .Lrodata+%zu\n", dst_name, rodata_offset);
            } else {
                fprintf(out, "\tleaq\t.Lrodata+%zu(%%rip),%%%s\n", rodata_offset, dst_name);
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

            /* Look up recorded call arguments */
            const IRCallArgs *call_args = ir_call_args_find(idx);

            if (BE(c)->emit_call_asm) {
                if (call_args) {
                    /* Unpack uint8_t arrays into int arrays for backend */
                    int unpacked_temps[IR_CALL_MAX_ARGS];
                    int unpacked_is_fp[IR_CALL_MAX_ARGS];
                    int unpacked_stack_pos[IR_CALL_MAX_ARGS];
                    for (int i = 0; i < call_args->count; i++) {
                        unpacked_temps[i] = (int)call_args->temps[i];
                        unpacked_is_fp[i] = (int)call_args->is_fp[i];
                        unpacked_stack_pos[i] = (int)call_args->stack_pos[i];
                    }
                    BE(c)->emit_call_asm(c, func_name, call_args->count, unpacked_temps, unpacked_is_fp, unpacked_stack_pos, out);
                } else {
                    BE(c)->emit_call_asm(c, func_name, 0, NULL, NULL, NULL, out);
                }
            }
            return;
        }

        case OP_SYSCALL: {
            /* OS syscall intrinsic - same argument packing as OP_CALL. */
            const IRCallArgs *syscall_args = ir_call_args_find(idx);
            if (BE(c)->emit_syscall_asm) {
                if (syscall_args) {
                    int unpacked_temps[IR_CALL_MAX_ARGS];
                    int n = 0;
                    for (int i = 0; i < syscall_args->count && n < 7; i++) {
                        if (syscall_args->is_fp[i] != 0) continue;
                        unpacked_temps[n++] = (int)syscall_args->temps[i];
                    }
                    BE(c)->emit_syscall_asm(c, n, unpacked_temps, out);
                } else if (instr->temp_dest > 0) {
                    int fallback[2] = {(int)instr->temp_src1, (int)instr->temp_src2};
                    int n = (instr->temp_dest > 1 && instr->temp_src2 != TEMP_NONE) ? 2 : 1;
                    BE(c)->emit_syscall_asm(c, n, fallback, out);
                }
            } else {
                fprintf(out, "\t# SYSCALL (unsupported on this target)\n");
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
                BE(c)->emit_param_save_fp_asm(c, (int)instr->imm, (int32_t)instr->imm64, out);
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

        /* Floating-point Literals and Operations */
        case OP_FMOV_IMM: {
            size_t rodata_offset = (size_t)instr->imm64;
            const char *dst_name = BE(c)->get_vec_reg_name(phys_dst);
            if (c->isa == ISA_AARCH64) {
                /* 8-byte double in rodata - load the d-lane, not s */
                fprintf(out, "\tadrp\tx9, .Lrodata+%zu\n", rodata_offset);
                fprintf(out, "\tldr\td%s, [x9, :lo12:.Lrodata+%zu]\n", dst_name + 1, rodata_offset);
            } else if (c->isa == ISA_RISCV64 || c->isa == ISA_RISCV32) {
                /* 8-byte double in rodata - fld, not flw */
                const char *fp_name = (phys_dst >= 0 && phys_dst < 32) ? BE(c)->regs->fp_regs[phys_dst] : "fa0";
                fprintf(out, "\tlla\tt0, .Lrodata+%zu\n", rodata_offset);
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

        default:
            /* Unknown opcode - emit comment */
            fprintf(out, "\t# UNKNOWN: %s\n", ir_opcode_name(instr->op));
            return;
    }
}

/* Emit a single IR instruction as assembly */
void asm_emit_instruction(ZeroCCCompiler *c, const Instr *instr, size_t idx, size_t func_idx) {
    FILE *out = c->asm_out ? c->asm_out : stdout;

    /* Skip DEAD and NOP instructions */
    if (instr->op == OP_DEAD || instr->op == OP_NOP) {
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
    int spill_src1 = (instr->op == OP_CALL) ? -1 : regalloc_get_spill_slot_for_temp(&c->regalloc, instr->temp_src1);
    int spill_src2 = (instr->op == OP_CALL) ? -1 : regalloc_get_spill_slot_for_temp(&c->regalloc, instr->temp_src2);
    int spill_dst  = (instr->op == OP_CALL) ? -1 : regalloc_get_spill_slot_for_temp(&c->regalloc, instr->temp_dest);

    /* Reload spilled sources into scratch registers. FP-class values use
     * the FP scratch XMMs (emit_load_fp_asm); GPR values use the GPR
     * scratches (C8). */
    if (spill_src1 >= 0) {
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
    if (spill_src2 >= 0) {
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
    if (spill_dst >= 0) {
        if (regalloc_get_class_for_temp(&c->regalloc, instr->temp_dest) == REG_CLASS_XMM) {
            phys_dst = BE(c)->fp_scratch_reg1;
        } else {
            phys_dst = BE(c)->scratch_reg1;
        }
    }

    asm_emit_instruction_core(c, instr, idx, func_idx, phys_dst, phys_src1, phys_src2, out);

    /* C99 width semantics: if the op produced a narrow-int result,
     * extend it to full 64-bit width now (sign or zero per the mark),
     * before any spill-back so the stored value is the extended one. */
    if (instr->width != 0 && instr->temp_dest != TEMP_NONE && BE(c)->emit_extend_asm) {
        BE(c)->emit_extend_asm(c, phys_dst, phys_dst,
                               (int)(instr->width & 7), (instr->width < 8) ? 1 : 0, out);
    }

    /* If destination temp was spilled, store the scratch register back to
     * the frame spill slot (FP scratch for FP-class). */
    if (spill_dst >= 0) {
        int32_t slot_off = spill_base_offset + (spill_dst + 1) * 8;
        if (regalloc_get_class_for_temp(&c->regalloc, instr->temp_dest) == REG_CLASS_XMM) {
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

    /* Get IR from buffer */
    const Instr *instrs = ir_get_buffer();
    size_t count = ir_get_count();
    size_t func_idx = c->function_count > 0 ? c->function_count - 1 : 0;

    /* Emit all IR instructions */
    for (size_t i = 0; i < count; i++) {
        asm_emit_instruction(c, &instrs[i], i, func_idx);
    }

    /* Emit epilogue */
    if (BE(c)->emit_epilogue_asm) {
        BE(c)->emit_epilogue_asm(c);
    }
}
