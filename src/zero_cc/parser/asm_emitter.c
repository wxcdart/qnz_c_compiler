/* Assembly Emitter - IR to Assembly Translation Implementation
 *
 * This module implements ISA-agnostic assembly emission from IR.
 * It translates IR instructions to GNU assembler syntax using the
 * backend's register information for ISA-specific details.
 */

#include "asm_emitter.h"
#include "ir.h"
#include "regalloc.h"
#include "../isa/isa_backend.h"
#include <stdio.h>
#include <string.h>

/* =========================================================================
 * Helper Functions
 * ========================================================================= */

/* Get physical register for a temporary */
static inline int asm_get_phys_reg(RegAllocator *ra, int temp) {
    return regalloc_get_reg_for_temp(ra, temp);
}

/* Get register name */
const char *asm_get_reg_name(int reg, int is_fp) {
    if (reg < 0 || reg > 31) return is_fp ? "%xmm0" : "%rax";
    
    /* x86-64 default names - will be overridden by backend info */
    if (is_fp) {
        static const char *xmm_regs[] = {
            "%xmm0", "%xmm1", "%xmm2", "%xmm3", "%xmm4", "%xmm5", "%xmm6", "%xmm7",
            "%xmm8", "%xmm9", "%xmm10", "%xmm11", "%xmm12", "%xmm13", "%xmm14", "%xmm15"
        };
        return reg < 16 ? xmm_regs[reg] : "%xmm0";
    } else {
        static const char *gpr_regs[] = {
            "%rax", "%rbx", "%rcx", "%rdx", "%rsp", "%rbp", "%rsi", "%rdi",
            "%r8", "%r9", "%r10", "%r11", "%r12", "%r13", "%r14", "%r15"
        };
        return reg < 16 ? gpr_regs[reg] : "%rax";
    }
}

/* Get backend-specific register name */
/* Note: Backend register names don't have '%' prefix, we add it here */
/* We use a simple round-robin buffer scheme to avoid static buffer issues */
static const char *asm_get_backend_int_reg_name(ZeroCCCompiler *c, int reg) {
    if (!c->isa_backend || !c->isa_backend->regs || !c->isa_backend->regs->int_regs) {
        return asm_get_reg_name(reg, 0);
    }
    if (reg < 16) {
        /* Use round-robin buffers to support multiple register names in same fprintf */
        static char bufs[4][16];
        static int idx = 0;
        char *buf = bufs[idx++ % 4];
        snprintf(buf, sizeof(bufs[0]), "%%%s", c->isa_backend->regs->int_regs[reg]);
        return buf;
    }
    return "%rax";  /* Default fallback */
}

/* =========================================================================
 * Core Emission Functions
 * ========================================================================= */

/* Emit a single IR instruction as assembly */
void asm_emit_instruction(ZeroCCCompiler *c, const Instr *instr, size_t idx, size_t func_idx) {
    FILE *out = c->asm_out ? c->asm_out : stdout;
    
    int phys_dst = (instr->temp_dest == 0xFF) ? BE(c)->fp_reg : 
                   asm_get_phys_reg(&c->regalloc, instr->temp_dest);
    int phys_src1 = (instr->temp_src1 == 0xFF) ? BE(c)->fp_reg : 
                    asm_get_phys_reg(&c->regalloc, instr->temp_src1);
    int phys_src2 = (instr->temp_src2 == 0xFF) ? BE(c)->fp_reg : 
                    asm_get_phys_reg(&c->regalloc, instr->temp_src2);
    
    const char *dst = asm_get_backend_int_reg_name(c, phys_dst);
    const char *src1 = asm_get_backend_int_reg_name(c, phys_src1);
    const char *src2 = asm_get_backend_int_reg_name(c, phys_src2);
    
    /* Emit IR opcode comment */
    fprintf(out, "\t# %zu: %s\n", idx, ir_opcode_name(instr->op));
    
    switch (instr->op) {
        /* Data movement */
        case OP_NOP:
            fprintf(out, "\tnop\n");
            return;
            
        case OP_MOV_IMM:
            fprintf(out, "\tmovq $%lld, %s\n", (long long)instr->imm64, dst);
            return;
            
        case OP_MOV_REG:
            fprintf(out, "\tmovq %s, %s\n", src1, dst);
            return;
            
        case OP_MOV:
            fprintf(out, "\tmovq %d(%%rbp), %s\n", (int)instr->imm, dst);
            return;
            
        case OP_MOV_MEM:
            fprintf(out, "\tmovq %s, %d(%%rbp)\n", src1, (int)instr->imm);
            return;
            
        case OP_LEA:
            fprintf(out, "\tleaq %d(%%rbp), %s\n", (int)instr->imm, dst);
            return;
            
        /* Arithmetic */
        case OP_ADD:
            /* dst = src1 + src2 */
            fprintf(out, "\tmovq %s, %s\n", src1, dst);
            fprintf(out, "\taddq %s, %s\n", src2, dst);
            return;
            
        case OP_SUB:
            /* dst = src1 - src2 */
            fprintf(out, "\tmovq %s, %s\n", src1, dst);
            fprintf(out, "\tsubq %s, %s\n", src2, dst);
            return;
            
        case OP_MUL:
            /* dst = src1 * src2 */
            fprintf(out, "\tmovq %s, %s\n", src1, dst);
            fprintf(out, "\timulq %s, %s\n", src2, dst);
            return;
            
        case OP_DIV:
            /* dst = src1 / src2 (signed) */
            fprintf(out, "\tmovq %s, %%rax\n", src1);
            fprintf(out, "\tcqo\n");
            fprintf(out, "\tidivq %s\n", src2);
            fprintf(out, "\tmovq %%rax, %s\n", dst);
            return;
            
        case OP_MOD:
            fprintf(out, "\tdivq %s\n", src1);
            fprintf(out, "\tmovq %%rdx, %s\n", dst);
            return;
            
        case OP_NEG:
            fprintf(out, "\tnegq %s\n", dst);
            return;
            
        case OP_AND:
            fprintf(out, "\tandq %s, %s\n", src1, dst);
            return;
            
        case OP_OR:
            fprintf(out, "\torq %s, %s\n", src1, dst);
            return;
            
        case OP_XOR:
            fprintf(out, "\txorq %s, %s\n", src1, dst);
            return;
            
        case OP_NOT:
            fprintf(out, "\tnotq %s\n", dst);
            return;
            
        case OP_SHL:
            fprintf(out, "\tmovq %s, %%rcx\n", src2);
            fprintf(out, "\tshlq %%cl, %s\n", dst);
            return;
            
        case OP_SHR:
            fprintf(out, "\tmovq %s, %%rcx\n", src2);
            fprintf(out, "\tshrq %%cl, %s\n", dst);
            return;
            
        case OP_SAR:
            fprintf(out, "\tmovq %s, %%rcx\n", src2);
            fprintf(out, "\tsarq %%cl, %s\n", dst);
            return;
            
        /* Comparison */
        case OP_CMP:
            fprintf(out, "\tcmpq %s, %s\n", src1, src2);
            return;
            
        case OP_COND_SET:
            fprintf(out, "\tset%s %%al\n", instr->cond == 0 ? "l" : "e");
            fprintf(out, "\tmovzbl %%al, %%eax\n");
            fprintf(out, "\tmovq %%rax, %s\n", dst);
            return;
            
        /* Control flow */
        case OP_JMP:
            fprintf(out, "\tjmp .L%zu_%zu\n", func_idx, instr->label_idx);
            return;
            
        case OP_JE:
            fprintf(out, "\tje .L%zu_%zu\n", func_idx, instr->label_idx);
            return;
            
        case OP_JNE:
            fprintf(out, "\tjne .L%zu_%zu\n", func_idx, instr->label_idx);
            return;
            
        case OP_JL:
            fprintf(out, "\tjl .L%zu_%zu\n", func_idx, instr->label_idx);
            return;
            
        case OP_JLE:
            fprintf(out, "\tjle .L%zu_%zu\n", func_idx, instr->label_idx);
            return;
            
        case OP_JG:
            fprintf(out, "\tjg .L%zu_%zu\n", func_idx, instr->label_idx);
            return;
            
        case OP_JCOND:
            fprintf(out, "\ttestq %s, %s\n", dst, dst);
            fprintf(out, "\t%s .L%zu_%zu\n", instr->cond == 0 ? "je" : "jne", func_idx, instr->label_idx);
            return;
            
        case OP_LABEL:
            fprintf(out, ".L%zu_%zu:\n", func_idx, instr->label_idx);
            return;
            return;
            
        /* Function */
        case OP_CALL:
            {
                /* Get function name */
                const char *fn_name = (const char *)(intptr_t)instr->imm64;
                
                /* Get call arguments from IR call args table */
                const IRCallArgs *call_args = ir_call_args_find(idx);
                int arg_count = call_args ? call_args->count : 0;
                
                /* System V AMD64 ABI: first 6 int args in rdi, rsi, rdx, rcx, r8, r9 */
                static const char *int_arg_regs[] = {"rdi", "rsi", "rdx", "rcx", "r8", "r9"};
                int int_arg_count = 6;  /* x86-64 has 6 integer argument registers */
                
                /* Handle stack arguments (args beyond 6) - push in reverse order */
                int n_stack = (arg_count > int_arg_count) ? arg_count - int_arg_count : 0;
                if (n_stack > 0) {
                    /* Push stack arguments in reverse order */
                    for (int a = arg_count - 1; a >= int_arg_count; a--) {
                        int temp = call_args->temps[a];
                        int phys = asm_get_phys_reg(&c->regalloc, temp);
                        const char *reg = asm_get_backend_int_reg_name(c, phys);
                        fprintf(out, "\tpushq %s\n", reg);
                    }
                    /* Adjust stack for 16-byte alignment (need even number of pushes) */
                    if (n_stack % 2 == 1) {
                        fprintf(out, "\tsubq $8, %%rsp\n");
                    }
                }
                
                /* Move first 6 args to argument registers */
                int n_reg_args = (arg_count < int_arg_count) ? arg_count : int_arg_count;
                for (int a = 0; a < n_reg_args; a++) {
                    int temp = call_args->temps[a];
                    int phys = asm_get_phys_reg(&c->regalloc, temp);
                    const char *reg = asm_get_backend_int_reg_name(c, phys);
                    fprintf(out, "\tmovq %s, %%%s\n", reg, int_arg_regs[a]);
                }
                
                /* Emit call */
                fprintf(out, "\tcall %s\n", fn_name);
                
                /* Clean up stack arguments */
                if (n_stack > 0) {
                    int cleanup = n_stack * 8;
                    if (n_stack % 2 == 1) cleanup += 8;  /* Extra alignment padding */
                    fprintf(out, "\taddq $%d, %%rsp\n", cleanup);
                }
            }
            return;
            
        case OP_CALL_INDIRECT:
            fprintf(out, "\tcall *%s\n", src1);
            return;
            
        case OP_RET:
            fprintf(out, "\tret\n");
            return;
            
        /* Frame management */
        case OP_PROLOGUE:
            fprintf(out, "\t# PROLOGUE\n");
            fprintf(out, "\tpush %%rbp\n");
            fprintf(out, "\tmov %%rsp, %%rbp\n");
            fprintf(out, "\tsub $%zu, %%rsp\n", (size_t)instr->imm64);
            return;
            
        case OP_EPILOGUE:
            fprintf(out, "\t# EPILOGUE\n");
            return;
            
        case OP_PARAM_SAVE_INT:
            {
                int seq_idx = (int)instr->imm;
                if (BE(c)->regs && BE(c)->regs->int_arg_regs && seq_idx < 8) {
                    const char *reg_name = BE(c)->regs->int_arg_regs[seq_idx];
                    if (reg_name) {
                        /* Stack slots are below rbp, so use negative offset */
                        fprintf(out, "\tmovq %%%s, -%d(%%rbp)\n", reg_name, (int)instr->imm64);
                    }
                }
            }
            return;
            
        case OP_PARAM_SAVE_FP:
            {
                int seq_idx = (int)instr->imm;
                if (BE(c)->regs && BE(c)->regs->fp_arg_regs && seq_idx < 8) {
                    const char *reg_name = BE(c)->regs->fp_arg_regs[seq_idx];
                    if (reg_name) {
                        fprintf(out, "\tmovsd %%%s, %d(%%rbp)\n", reg_name, (int)instr->imm64);
                    }
                }
            }
            return;
            
        case OP_VARIADIC_SPILL:
            fprintf(out, "\t# VARIADIC_SPILL param_count=%d\n", (int)instr->imm);
            return;
            
        /* Memory operations */
        case OP_DEREF:
            fprintf(out, "\tmovq (%s), %s\n", src1, dst);
            return;
            
        case OP_LOAD_32:
            fprintf(out, "\tmovl (%s), %s\n", src1, dst);
            return;
            
        case OP_STORE_32:
            fprintf(out, "\tmovl %s, (%s)\n", src2, src1);
            return;
            
        case OP_STORE_64:
            fprintf(out, "\tmovq %s, (%s)\n", src2, src1);
            return;
            
        case OP_VLA_ALLOC:
            fprintf(out, "\tsub %s, %%rsp\n", src1);
            fprintf(out, "\tand $-16, %%rsp\n");
            fprintf(out, "\tmov %%rsp, %s\n", dst);
            return;
            
        case OP_CALLEE_SAVE_STORES:
            fprintf(out, "\t# CALLEE_SAVE_STORES mask=0x%x\n", (unsigned)instr->imm);
            return;
            
        case OP_CALLEE_SAVE_LOADS:
            fprintf(out, "\t# CALLEE_SAVE_LOADS mask=0x%x\n", (unsigned)instr->imm);
            return;
            
        case OP_DEAD:
            fprintf(out, "\t# DEAD\n");
            return;
            
        /* Floating point */
        case OP_FADD:
            {
                int phys_src1 = asm_get_phys_reg(&c->regalloc, instr->temp_src1);
                int phys_dest = asm_get_phys_reg(&c->regalloc, instr->temp_dest);
                fprintf(out, "\taddsd %%xmm%u, %%xmm%u\n", phys_src1, phys_dest);
            }
            return;
            
        case OP_FSUB:
            {
                int phys_src1 = asm_get_phys_reg(&c->regalloc, instr->temp_src1);
                int phys_dest = asm_get_phys_reg(&c->regalloc, instr->temp_dest);
                fprintf(out, "\tsubsd %%xmm%u, %%xmm%u\n", phys_src1, phys_dest);
            }
            return;
            
        case OP_FMUL:
            {
                int phys_src1 = asm_get_phys_reg(&c->regalloc, instr->temp_src1);
                int phys_dest = asm_get_phys_reg(&c->regalloc, instr->temp_dest);
                fprintf(out, "\tmulsd %%xmm%u, %%xmm%u\n", phys_src1, phys_dest);
            }
            return;
            
        case OP_FDIV:
            {
                int phys_src1 = asm_get_phys_reg(&c->regalloc, instr->temp_src1);
                int phys_dest = asm_get_phys_reg(&c->regalloc, instr->temp_dest);
                fprintf(out, "\tdivsd %%xmm%u, %%xmm%u\n", phys_src1, phys_dest);
            }
            return;
            
        case OP_CVTSI2SD:
            fprintf(out, "\tcvtsi2sd %s, %%xmm%u\n", src1, instr->temp_dest);
            return;
            
        case OP_CVTTSD2SI:
            fprintf(out, "\tcvttss2si %%xmm%u, %s\n", instr->temp_src1, dst);
            return;
            
        /* SIMD operations */
        case OP_VADDSS:
            {
                int phys_src1 = asm_get_phys_reg(&c->regalloc, instr->temp_src1);
                int phys_dest = asm_get_phys_reg(&c->regalloc, instr->temp_dest);
                fprintf(out, "\taddss %%xmm%u, %%xmm%u\n", phys_src1, phys_dest);
            }
            return;
            
        case OP_VSUBSS:
            {
                int phys_src1 = asm_get_phys_reg(&c->regalloc, instr->temp_src1);
                int phys_dest = asm_get_phys_reg(&c->regalloc, instr->temp_dest);
                fprintf(out, "\tsubss %%xmm%u, %%xmm%u\n", phys_src1, phys_dest);
            }
            return;
            
        case OP_VMULSS:
            {
                int phys_src1 = asm_get_phys_reg(&c->regalloc, instr->temp_src1);
                int phys_dest = asm_get_phys_reg(&c->regalloc, instr->temp_dest);
                fprintf(out, "\tmulss %%xmm%u, %%xmm%u\n", phys_src1, phys_dest);
            }
            return;
            
        case OP_VDIVSS:
            {
                int phys_src1 = asm_get_phys_reg(&c->regalloc, instr->temp_src1);
                int phys_dest = asm_get_phys_reg(&c->regalloc, instr->temp_dest);
                fprintf(out, "\tdivss %%xmm%u, %%xmm%u\n", phys_src1, phys_dest);
            }
            return;
            
        case OP_VMOV:
            {
                int phys_src1 = asm_get_phys_reg(&c->regalloc, instr->temp_src1);
                int phys_dest = asm_get_phys_reg(&c->regalloc, instr->temp_dest);
                fprintf(out, "\tmovapd %%xmm%u, %%xmm%u\n", phys_src1, phys_dest);
            }
            return;
            
        case OP_VLOAD:
            {
                int phys_dest = asm_get_phys_reg(&c->regalloc, instr->temp_dest);
                fprintf(out, "\tmovapd (%s), %%xmm%u\n", src1, phys_dest);
            }
            return;
            
        case OP_VSTORE:
            {
                int phys_src2 = asm_get_phys_reg(&c->regalloc, instr->temp_src2);
                fprintf(out, "\tmovapd %%xmm%u, (%s)\n", phys_src2, src1);
            }
            return;
            
        default:
            fprintf(out, "\t# UNKNOWN opcode=%u\n", (unsigned)instr->op);
            return;
    }
}

/* Emit function prologue */
void asm_emit_prologue(FILE *out, size_t frame_size) {
    fprintf(out, "\tpush %%rbp\n");
    fprintf(out, "\tmov %%rsp, %%rbp\n");
    fprintf(out, "\tsub $%zu, %%rsp\n", frame_size);
}

/* Emit function epilogue */
void asm_emit_epilogue(FILE *out) {
    fprintf(out, "\tleave\n");
    fprintf(out, "\tret\n");
}

/* Emit a label */
void asm_emit_label(FILE *out, const char *label) {
    fprintf(out, "%s:\n", label);
}

/* =========================================================================
 * Main Entry Point
 * ========================================================================= */

/* Emit assembly from IR with allocated registers */
void emit_ir_to_asm(ZeroCCCompiler *c, int32_t spill_base_offset) {
    (void)spill_base_offset; /* Not used for assembly emission */
    FILE *out = c->asm_out ? c->asm_out : stdout;
    
    const Instr *instrs = ir_get_buffer();
    size_t count = ir_get_count();
    size_t func_idx = c->function_count - 1;
    
    /* Emit function label */
    if (func_idx < c->function_count && c->functions[func_idx].name[0]) {
        fprintf(out, "\t.globl %s\n", c->functions[func_idx].name);
        fprintf(out, "%s:\n", c->functions[func_idx].name);
    }
    
    /* Emit instructions */
    for (size_t i = 0; i < count; i++) {
        const Instr *instr = &instrs[i];
        
        /* Skip dead instructions */
        if (instr->op == OP_DEAD) {
            continue;
        }
        
        /* Skip EPILOGUE - it's handled by leave/ret at the end */
        if (instr->op == OP_EPILOGUE) {
            continue;
        }
        
        asm_emit_instruction(c, instr, i, func_idx);
    }
    
    /* Emit epilogue */
    asm_emit_epilogue(out);
}
