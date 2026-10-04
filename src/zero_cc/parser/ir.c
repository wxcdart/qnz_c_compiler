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
    Instr instr = {.op = OP_LABEL, .temp_dest = 0xFF, .temp_src1 = 0xFF, .temp_src2 = 0xFF, .imm = 0, .imm64 = 0, .label_idx = label_idx};
    ir_emit(instr);
}

void ir_emit_jmp(uint16_t label_idx) {
    Instr instr = {.op = OP_JMP, .temp_dest = 0xFF, .temp_src1 = 0xFF, .temp_src2 = 0xFF, .imm = 0, .imm64 = 0, .label_idx = label_idx};
    ir_emit(instr);
}

void ir_emit_je(uint8_t temp_cond, uint16_t label_idx) {
    Instr instr = {.op = OP_JE, .temp_dest = 0xFF, .temp_src1 = temp_cond, .temp_src2 = 0xFF, .imm = 0, .imm64 = 0, .label_idx = label_idx};
    ir_emit(instr);
}

void ir_emit_jne(uint8_t temp_cond, uint16_t label_idx) {
    Instr instr = {.op = OP_JNE, .temp_dest = 0xFF, .temp_src1 = temp_cond, .temp_src2 = 0xFF, .imm = 0, .imm64 = 0, .label_idx = label_idx};
    ir_emit(instr);
}

void ir_emit_jmp_label(uint16_t label_idx) {
    Instr instr = {.op = OP_JMP_LABEL, .temp_dest = 0xFF, .temp_src1 = 0xFF, .temp_src2 = 0xFF, .imm = 0, .imm64 = 0, .label_idx = label_idx};
    ir_emit(instr);
}

/* Clear IR buffer for next function */
void ir_clear(void) {
    instr_count = 0;
}
