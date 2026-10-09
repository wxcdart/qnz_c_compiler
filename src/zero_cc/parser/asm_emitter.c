/* Assembly Emitter - IR to Assembly Translation
 *
 * This module implements ISA-agnostic assembly emission from IR.
 * It delegates to backend-specific functions for ISA-specific assembly generation.
 * 
 * Option A: Full Backend-Driven Architecture
 * All assembly emission is delegated to BE(c)->emit_*_asm() functions.
 */

#include "asm_emitter.h"
#include "ir.h"
#include "regalloc.h"
#include "../isa/isa_backend.h"
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
    
    /* Emit .globl and label */
    if (func_idx < c->function_count && c->functions[func_idx].name[0]) {
        fprintf(out, "\t.globl %s\n", c->functions[func_idx].name);
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

/* Get register name with % prefix */
static const char *asm_get_reg_name_prefixed(ZeroCCCompiler *c, int phys_reg, int is_fp) {
    static char bufs[4][16];
    static int idx = 0;
    char *buf = bufs[idx++ % 4];
    
    if (BE(c)->get_hw_reg_name && !is_fp) {
        snprintf(buf, sizeof(bufs[0]), "%%%s", BE(c)->get_hw_reg_name(phys_reg));
    } else if (BE(c)->regs && (is_fp ? BE(c)->regs->fp_regs : BE(c)->regs->int_regs)) {
        const char **regs = is_fp ? BE(c)->regs->fp_regs : BE(c)->regs->int_regs;
        snprintf(buf, sizeof(bufs[0]), "%%%s", phys_reg < 32 ? regs[phys_reg] : "rax");
    } else {
        snprintf(buf, sizeof(bufs[0]), "%%r%d", phys_reg);
    }
    return buf;
}

/* Get vector register name with prefix */
static const char *asm_get_vec_reg_name(ZeroCCCompiler *c, int vec_reg_idx) {
    static char buf[16];
    
    if (BE(c)->get_vec_reg_name) {
        const char *name = BE(c)->get_vec_reg_name(vec_reg_idx);
        snprintf(buf, sizeof(buf), "%%%s", name);
    } else {
        /* Fallback: use ISA-specific naming */
        if (c->regalloc.target_isa == ISA_AARCH64) {
            snprintf(buf, sizeof(buf), "%%v%d", vec_reg_idx);
        } else if (c->regalloc.target_isa == ISA_RISCV64) {
            snprintf(buf, sizeof(buf), "%%v%d", vec_reg_idx);
        } else {
            snprintf(buf, sizeof(buf), "%%xmm%d", vec_reg_idx);
        }
    }
    return buf;
}

/* =========================================================================
 * Core Emission Functions - Delegate to Backend
 * ========================================================================= */

/* Emit a single IR instruction as assembly */
void asm_emit_instruction(ZeroCCCompiler *c, const Instr *instr, size_t idx, size_t func_idx) {
    FILE *out = c->asm_out ? c->asm_out : stdout;
    
    /* Skip DEAD instructions */
    if (instr->op == OP_DEAD) {
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
    int phys_dst = instr->temp_dest == 0xFF ? BE(c)->fp_reg : asm_get_phys_reg(&c->regalloc, instr->temp_dest);
    int phys_src1 = instr->temp_src1 == 0xFF ? BE(c)->fp_reg : asm_get_phys_reg(&c->regalloc, instr->temp_src1);
    int phys_src2 = instr->temp_src2 == 0xFF ? BE(c)->fp_reg : asm_get_phys_reg(&c->regalloc, instr->temp_src2);
    
    switch (instr->op) {
        /* Data Movement */
        case OP_MOV_IMM:
            if (BE(c)->emit_mov_imm_asm) {
                BE(c)->emit_mov_imm_asm(c, phys_dst, instr->imm64, out);
            }
            return;
            
        case OP_MOV_REG:
            if (BE(c)->emit_mov_reg_asm) {
                BE(c)->emit_mov_reg_asm(c, phys_dst, phys_src1, out);
            }
            return;
            
        case OP_MOV:  /* Load from stack */
            if (BE(c)->emit_load_asm) {
                BE(c)->emit_load_asm(c, phys_dst, BE(c)->sp_reg, instr->imm, out);
            }
            return;
            
        case OP_MOV_MEM:  /* Store to stack */
            if (BE(c)->emit_store_asm) {
                BE(c)->emit_store_asm(c, phys_src1, BE(c)->sp_reg, instr->imm, out);
            }
            return;
            
        case OP_LEA:
            if (BE(c)->emit_lea_asm) {
                BE(c)->emit_lea_asm(c, phys_dst, BE(c)->sp_reg, instr->imm, out);
            }
            return;
            
        case OP_DEREF:
            if (BE(c)->emit_deref_asm) {
                BE(c)->emit_deref_asm(c, phys_dst, phys_src1, out);
            }
            return;
            
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
                BE(c)->emit_div_asm(c, phys_dst, phys_src1, phys_src2, out);
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
            
        /* Comparison */
        case OP_CMP:
            if (BE(c)->emit_cmp_asm) {
                BE(c)->emit_cmp_asm(c, phys_src1, phys_src2, out);
            }
            return;
            
        case OP_COND_SET:
            if (BE(c)->emit_cond_set_asm) {
                BE(c)->emit_cond_set_asm(c, phys_dst, instr->cond, out);
            }
            return;
            
        /* Control Flow */
        case OP_LABEL:
            if (BE(c)->emit_label_asm) {
                /* Get label name from labels array */
                const char *label_name = NULL;
                for (int i = 0; i < c->label_count; i++) {
                    if (c->labels[i].name && instr->label_idx == (size_t)i) {
                        label_name = c->labels[i].name;
                        break;
                    }
                }
                if (label_name) {
                    BE(c)->emit_label_asm(c, label_name, out);
                }
            }
            return;
            
        case OP_JMP:
            if (BE(c)->emit_jmp_asm) {
                /* Get label name from labels array */
                const char *label_name = NULL;
                for (int i = 0; i < c->label_count; i++) {
                    if (c->labels[i].name && instr->label_idx == (size_t)i) {
                        label_name = c->labels[i].name;
                        break;
                    }
                }
                if (label_name) {
                    BE(c)->emit_jmp_asm(c, label_name, out);
                }
            }
            return;
            
        case OP_JE:
            if (BE(c)->emit_jeq_asm) {
                const char *label_name = NULL;
                for (int i = 0; i < c->label_count; i++) {
                    if (c->labels[i].name && instr->label_idx == (size_t)i) {
                        label_name = c->labels[i].name;
                        break;
                    }
                }
                if (label_name) {
                    BE(c)->emit_jeq_asm(c, label_name, out);
                }
            }
            return;
            
        case OP_JNE:
            if (BE(c)->emit_jne_asm) {
                const char *label_name = NULL;
                for (int i = 0; i < c->label_count; i++) {
                    if (c->labels[i].name && instr->label_idx == (size_t)i) {
                        label_name = c->labels[i].name;
                        break;
                    }
                }
                if (label_name) {
                    BE(c)->emit_jne_asm(c, label_name, out);
                }
            }
            return;
            
        case OP_JL:
            if (BE(c)->emit_jlt_asm) {
                const char *label_name = NULL;
                for (int i = 0; i < c->label_count; i++) {
                    if (c->labels[i].name && instr->label_idx == (size_t)i) {
                        label_name = c->labels[i].name;
                        break;
                    }
                }
                if (label_name) {
                    BE(c)->emit_jlt_asm(c, label_name, out);
                }
            }
            return;
            
        case OP_JLE:
            if (BE(c)->emit_jle_asm) {
                const char *label_name = NULL;
                for (int i = 0; i < c->label_count; i++) {
                    if (c->labels[i].name && instr->label_idx == (size_t)i) {
                        label_name = c->labels[i].name;
                        break;
                    }
                }
                if (label_name) {
                    BE(c)->emit_jle_asm(c, label_name, out);
                }
            }
            return;
            
        case OP_JG:
            if (BE(c)->emit_jgt_asm) {
                const char *label_name = NULL;
                for (int i = 0; i < c->label_count; i++) {
                    if (c->labels[i].name && instr->label_idx == (size_t)i) {
                        label_name = c->labels[i].name;
                        break;
                    }
                }
                if (label_name) {
                    BE(c)->emit_jgt_asm(c, label_name, out);
                }
            }
            return;
            
        case OP_JCOND:
            /* Conditional jump - emit based on cond field */
            if (BE(c)->emit_jeq_asm || BE(c)->emit_jne_asm || BE(c)->emit_jlt_asm || 
                BE(c)->emit_jle_asm || BE(c)->emit_jgt_asm || BE(c)->emit_jge_asm) {
                const char *label_name = NULL;
                for (int i = 0; i < c->label_count; i++) {
                    if (c->labels[i].name && instr->label_idx == (size_t)i) {
                        label_name = c->labels[i].name;
                        break;
                    }
                }
                if (label_name) {
                    /* Use cond field to determine which jump */
                    switch (instr->cond) {
                        case 0: if (BE(c)->emit_jlt_asm) BE(c)->emit_jlt_asm(c, label_name, out); break;
                        case 1: if (BE(c)->emit_jle_asm) BE(c)->emit_jle_asm(c, label_name, out); break;
                        case 2: if (BE(c)->emit_jgt_asm) BE(c)->emit_jgt_asm(c, label_name, out); break;
                        case 3: if (BE(c)->emit_jge_asm) BE(c)->emit_jge_asm(c, label_name, out); break;
                        case 4: if (BE(c)->emit_jeq_asm) BE(c)->emit_jeq_asm(c, label_name, out); break;
                        case 5: if (BE(c)->emit_jne_asm) BE(c)->emit_jne_asm(c, label_name, out); break;
                    }
                }
            }
            return;
            
        case OP_CALL: {
            /* Get function name from imm64 (stored as pointer) */
            const char *func_name = (const char *)(intptr_t)instr->imm64;
            
            /* Look up recorded call arguments */
            const IRCallArgs *call_args = ir_call_args_find(idx);
            
            if (BE(c)->emit_call_asm) {
                if (call_args) {
                    /* Unpack uint8_t temps array into int array for backend */
                    int unpacked_temps[IR_CALL_MAX_ARGS];
                    int unpacked_is_fp[IR_CALL_MAX_ARGS];
                    for (int i = 0; i < call_args->count; i++) {
                        unpacked_temps[i] = (int)call_args->temps[i];
                        unpacked_is_fp[i] = (int)call_args->is_fp[i];
                    }
                    BE(c)->emit_call_asm(c, func_name, call_args->count, unpacked_temps, unpacked_is_fp, out);
                } else {
                    BE(c)->emit_call_asm(c, func_name, 0, NULL, NULL, out);
                }
            }
            return;
        }
            
        case OP_RET:
            if (BE(c)->emit_ret_asm) {
                BE(c)->emit_ret_asm(c, out);
            }
            return;
            
        /* Floating Point */
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
            
        /* VLA */
        case OP_VLA_ALLOC:
            if (BE(c)->emit_vla_alloc_asm) {
                BE(c)->emit_vla_alloc_asm(c, phys_dst, phys_src1, out);
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
            
        case OP_VSTORE:
            if (BE(c)->emit_vstore_asm) {
                BE(c)->emit_vstore_asm(c, phys_src1, phys_src2, instr->imm, out);
            }
            return;
            
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

/* Emit all IR instructions as assembly */
void emit_ir_to_asm(ZeroCCCompiler *c, int32_t spill_base_offset) {
    FILE *out = c->asm_out ? c->asm_out : stdout;
    
    /* Get IR from buffer */
    const Instr *instrs = ir_get_buffer();
    size_t count = ir_get_count();
    
    /* Emit all IR instructions */
    for (size_t i = 0; i < count; i++) {
        asm_emit_instruction(c, &instrs[i], i, 0);
    }
    
    /* Emit epilogue */
    if (BE(c)->emit_epilogue_asm) {
        BE(c)->emit_epilogue_asm(c);
    }
}
