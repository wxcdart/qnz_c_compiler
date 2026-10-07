#include "ir.h"

/* Global instruction buffer (shared across all functions) */
Instr instrs[MAX_INSTRS];
size_t instr_count = 0;

/* Per-call integer argument lists (see ir.h) */
IRCallArgs ir_call_args_table[IR_CALL_MAX_SITES];
size_t ir_call_args_count = 0;

/* IR Emitter Functions */
void ir_emit(Instr instr) {
    if (instr_count >= MAX_INSTRS) {
        fprintf(stderr, "Error: Function too large (>64K instructions)\n");
        exit(1);
    }
    instrs[instr_count] = instr;
    instr_count++;
}

void ir_emit_label(uint16_t label_idx) {
    Instr instr = {.op = OP_LABEL, .temp_dest = 0xFF, .temp_src1 = 0xFF, .temp_src2 = 0xFF, .cond = 0, .flags_out = 0xFF, .flags_in = 0xFF, .imm = 0, .imm64 = 0, .label_idx = label_idx};
    ir_emit(instr);
}

void ir_emit_jmp(uint16_t label_idx) {
    Instr instr = {.op = OP_JMP, .temp_dest = 0xFF, .temp_src1 = 0xFF, .temp_src2 = 0xFF, .cond = 0, .flags_out = 0xFF, .flags_in = 0xFF, .imm = 0, .imm64 = 0, .label_idx = label_idx};
    ir_emit(instr);
}

void ir_emit_je(uint8_t temp_cond, uint16_t label_idx) {
    Instr instr = {.op = OP_JE, .temp_dest = 0xFF, .temp_src1 = temp_cond, .temp_src2 = 0xFF, .cond = temp_cond, .flags_out = 0xFF, .flags_in = 0xFF, .imm = 0, .imm64 = 0, .label_idx = label_idx};
    ir_emit(instr);
}

void ir_emit_jcond(uint8_t temp_cond, uint16_t label_idx) {
    Instr instr = {.op = OP_JCOND, .temp_dest = 0xFF, .temp_src1 = temp_cond, .temp_src2 = 0xFF, .cond = temp_cond, .flags_out = 0xFF, .flags_in = REG_FLAGS, .imm = 0, .imm64 = 0, .label_idx = label_idx};
    ir_emit(instr);
}

void ir_emit_jne(uint8_t temp_cond, uint16_t label_idx) {
    Instr instr = {.op = OP_JNE, .temp_dest = 0xFF, .temp_src1 = temp_cond, .temp_src2 = 0xFF, .cond = temp_cond, .flags_out = 0xFF, .flags_in = 0xFF, .imm = 0, .imm64 = 0, .label_idx = label_idx};
    ir_emit(instr);
}

void ir_emit_jmp_label(uint16_t label_idx) {
    Instr instr = {.op = OP_JMP_LABEL, .temp_dest = 0xFF, .temp_src1 = 0xFF, .temp_src2 = 0xFF, .cond = 0, .flags_out = 0xFF, .flags_in = 0xFF, .imm = 0, .imm64 = 0, .label_idx = label_idx};
    ir_emit(instr);
}

/* Clear IR buffer for next function */
void ir_clear(void) {
    instr_count = 0;
}

/* Debug: Dump IR to file */
void ir_dump(FILE *f) {
    size_t count = instr_count;
    for (size_t i = 0; i < count; i++) {
        Instr *instr = &instrs[i];
        
        /* Print instruction index */
        fprintf(f, "%4zu: ", i);
        
        /* Print instruction */
        switch (instr->op) {
            case OP_MOV: 
                         if (instr->temp_src1 == 0xFF) {
                             /* This is actually a LOAD from stack (temp_src1=0xFF is frame pointer marker) */
                             fprintf(f, "LOAD r%u, [fp%+d]", instr->temp_dest, (int)instr->imm);
                         } else {
                             fprintf(f, "MOV r%u, ", instr->temp_dest);
                             if (instr->flags_in == REG_FLAGS) fprintf(f, "[flags] ");
                             fprintf(f, "r%u", instr->temp_src1);
                         }
                         break;
            case OP_MOV_IMM: fprintf(f, "MOV_IMM r%u, %d", instr->temp_dest, (int)instr->imm);
                             break;
            case OP_MOV_MEM: fprintf(f, "MOV_MEM [sp-%u], r%u", (unsigned)instr->imm, instr->temp_src1);
                             break;
            case OP_ADD: fprintf(f, "ADD r%u, r%u, r%u", instr->temp_dest, instr->temp_src1, instr->temp_src2);
                         break;
            case OP_SUB: fprintf(f, "SUB r%u, r%u, r%u", instr->temp_dest, instr->temp_src1, instr->temp_src2);
                         break;
            case OP_MUL: fprintf(f, "MUL r%u, r%u, r%u", instr->temp_dest, instr->temp_src1, instr->temp_src2);
                         break;
            case OP_DIV: fprintf(f, "DIV r%u, r%u, r%u", instr->temp_dest, instr->temp_src1, instr->temp_src2);
                         break;
            case OP_MOD: fprintf(f, "MOD r%u, r%u, r%u", instr->temp_dest, instr->temp_src1, instr->temp_src2);
                         break;
            case OP_AND: fprintf(f, "AND r%u, r%u, r%u", instr->temp_dest, instr->temp_src1, instr->temp_src2);
                         break;
            case OP_OR:  fprintf(f, "OR r%u, r%u, r%u", instr->temp_dest, instr->temp_src1, instr->temp_src2);
                         break;
            case OP_XOR: fprintf(f, "XOR r%u, r%u, r%u", instr->temp_dest, instr->temp_src1, instr->temp_src2);
                         break;
            case OP_SHL: fprintf(f, "SHL r%u, r%u, r%u", instr->temp_dest, instr->temp_src1, instr->temp_src2);
                         break;
            case OP_SHR: fprintf(f, "SHR r%u, r%u, r%u", instr->temp_dest, instr->temp_src1, instr->temp_src2);
                         break;
            case OP_SAR: fprintf(f, "SAR r%u, r%u, r%u", instr->temp_dest, instr->temp_src1, instr->temp_src2);
                         break;
            case OP_NEG: fprintf(f, "NEG r%u, r%u", instr->temp_dest, instr->temp_src1);
                         break;
            case OP_NOT: fprintf(f, "NOT r%u, r%u", instr->temp_dest, instr->temp_src1);
                         break;
            case OP_CMP: fprintf(f, "CMP r%u, ", instr->temp_src1);
                         if (instr->flags_in == REG_FLAGS) fprintf(f, "[flags] ");
                         fprintf(f, "r%u", instr->temp_src2);
                         break;
            case OP_COND_SET: fprintf(f, "COND_SET r%u, cc_%u", instr->temp_dest, instr->cond);
                              break;
            case OP_JCOND: fprintf(f, "JCOND cc_%u, L%u", instr->cond, instr->label_idx);
                           break;
            case OP_JMP: fprintf(f, "JMP L%u", instr->label_idx);
                         break;
            case OP_JE: fprintf(f, "JE r%u, L%u", instr->temp_src1, instr->label_idx);
                        break;
            case OP_JNE: fprintf(f, "JNE r%u, L%u", instr->temp_src1, instr->label_idx);
                         break;
            case OP_JL: fprintf(f, "JL r%u, L%u", instr->temp_src1, instr->label_idx);
                        break;
            case OP_JLE: fprintf(f, "JLE r%u, L%u", instr->temp_src1, instr->label_idx);
                         break;
            case OP_JG: fprintf(f, "JG r%u, L%u", instr->temp_src1, instr->label_idx);
                        break;
            case OP_RET: fprintf(f, "RET");
                         break;
            case OP_CALL: fprintf(f, "CALL fn_%u", (unsigned)instr->imm);
                          break;
            case OP_LEA: fprintf(f, "LEA r%u, [fp%+d]", instr->temp_dest, (int)instr->imm);
                         break;
            case OP_FLOAD: fprintf(f, "FLOAD xmm%u, [fp%+d]", instr->temp_dest, (int)instr->imm);
                           break;
            case OP_FSTORE: fprintf(f, "FSTORE [fp%+d], xmm%u", (int)instr->imm, instr->temp_src1);
                            break;
            case OP_FADD: fprintf(f, "FADD xmm%u, xmm%u, xmm%u", instr->temp_dest, instr->temp_src1, instr->temp_src2);
                          break;
            case OP_FSUB: fprintf(f, "FSUB xmm%u, xmm%u, xmm%u", instr->temp_dest, instr->temp_src1, instr->temp_src2);
                          break;
            case OP_FMUL: fprintf(f, "FMUL xmm%u, xmm%u, xmm%u", instr->temp_dest, instr->temp_src1, instr->temp_src2);
                          break;
            case OP_FDIV: fprintf(f, "FDIV xmm%u, xmm%u, xmm%u", instr->temp_dest, instr->temp_src1, instr->temp_src2);
                          break;
            case OP_CVTSI2SD: fprintf(f, "CVTSI2SD xmm%u, r%u", instr->temp_dest, instr->temp_src1);
                              break;
            case OP_CVTTSD2SI: fprintf(f, "CVTTSD2SI r%u, xmm%u", instr->temp_dest, instr->temp_src1);
                               break;
            case OP_LABEL: fprintf(f, "L%u:", instr->label_idx);
                           break;
            case OP_PROLOGUE: fprintf(f, "PROLOGUE %u", (unsigned)instr->imm);
                              break;
            case OP_EPILOGUE: fprintf(f, "EPILOGUE");
                              break;
            case OP_NOP: fprintf(f, "NOP");
                         break;
            case OP_DEAD: fprintf(f, "DEAD");
                          break;
            case OP_MOV_REG: fprintf(f, "MOV_REG r%u, r%u", instr->temp_dest, instr->temp_src1);
                             break;
            case OP_MOV_GLOBAL: fprintf(f, "MOV_GLOBAL r%u, %s", instr->temp_dest, (char*)instr->imm64);
                                break;
            case OP_VLA_ALLOC: fprintf(f, "VLA_ALLOC r%u, r%u", instr->temp_dest, instr->temp_src1);
                               break;
            case OP_ATOMIC_XADD: fprintf(f, "ATOMIC_XADD r%u, [r%u], r%u", instr->temp_dest, instr->temp_src1, instr->temp_src2);
                                 break;
            case OP_ATOMIC_XCHG: fprintf(f, "ATOMIC_XCHG r%u, [r%u], r%u", instr->temp_dest, instr->temp_src1, instr->temp_src2);
                                 break;
            case OP_STORE_BYTE: fprintf(f, "STORE_BYTE [fp%+d], r%u", (int)instr->imm, instr->temp_src1);
                                break;
            case OP_STORE_16: fprintf(f, "STORE_16 [fp%+d], r%u", (int)instr->imm, instr->temp_src1);
                              break;
            case OP_LOAD_BYTE: fprintf(f, "LOAD_BYTE r%u, [fp%+d]", instr->temp_dest, (int)instr->imm);
                               break;
            case OP_LOAD_16: fprintf(f, "LOAD_16 r%u, [fp%+d]", instr->temp_dest, (int)instr->imm);
                             break;
            case OP_CMP_ZERO: fprintf(f, "CMP_ZERO r%u", instr->temp_src1);
                              break;
            default: fprintf(f, "OP_%u", instr->op);
                     break;
        }
        
        /* Print flag annotations */
        fprintf(f, " [flags_out=");
        if (instr->flags_out == REG_FLAGS) fprintf(f, "r0");
        else fprintf(f, "r%u", instr->flags_out);
        fprintf(f, "] [flags_in=");
        if (instr->flags_in == REG_FLAGS) fprintf(f, "r0");
        else fprintf(f, "r%u", instr->flags_in);
        fprintf(f, "]\n");
    }
}
