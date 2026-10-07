/* IR Opcode Test */
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include "zero_cc/parser/ir.h"

int main(void) {
    printf("Testing IR opcodes...\n");
    
    /* Test 1: Opcode enum values */
    assert(OP_NOP == 0);
    assert(OP_MOV_IMM == 1);
    assert(OP_ADD == 10);  /* After 8 data movement + 2 arithmetic (ADD_IMM, SUB_IMM) */
    assert(OP_SUB == 11);
    assert(OP_RET == 37);  /* After data movement, arithmetic, logical, comparison, control flow (+ JMP_LABEL, CALL, CALL_INDIRECT, CMP_BRANCH) */
    printf("  Opcode values: OK\n");
    
    /* Test 2: Opcode names */
    assert(strcmp(ir_opcode_name(OP_NOP), "NOP") == 0);
    assert(strcmp(ir_opcode_name(OP_MOV_IMM), "MOV_IMM") == 0);
    assert(strcmp(ir_opcode_name(OP_ADD), "ADD") == 0);
    assert(strcmp(ir_opcode_name(OP_RET), "RET") == 0);
    assert(strcmp(ir_opcode_name(999), "UNKNOWN") == 0);  /* Invalid opcode */
    printf("  Opcode names: OK\n");
    
    /* Test 3: Helper functions */
    ir_reset();
    ir_emit_mov_imm(0, 42);
    assert(ir_get_count() == 1);
    Instr* i = ir_get(0);
    assert(i->op == OP_MOV_IMM);
    assert(i->temp_dest == 0);
    assert(i->imm == 42);
    printf("  emit_mov_imm: OK\n");
    
    /* Test 4: Add helper */
    ir_reset();
    ir_emit_add(2, 0, 1);
    assert(ir_get_count() == 1);
    i = ir_get(0);
    assert(i->op == OP_ADD);
    assert(i->temp_dest == 2);
    assert(i->temp_src1 == 0);
    assert(i->temp_src2 == 1);
    printf("  emit_add: OK\n");
    
    /* Test 5: Label helper */
    ir_reset();
    ir_emit_label(5);
    assert(ir_get_count() == 1);
    i = ir_get(0);
    assert(i->op == OP_LABEL);
    assert(i->label_idx == 5);
    printf("  emit_label: OK\n");
    
    /* Test 6: Jump helper */
    ir_reset();
    ir_emit_jmp(10);
    assert(ir_get_count() == 1);
    i = ir_get(0);
    assert(i->op == OP_JMP);
    assert(i->label_idx == 10);
    printf("  emit_jmp: OK\n");
    
    /* Test 7: JE helper */
    ir_reset();
    ir_emit_je(0, 15);
    assert(ir_get_count() == 1);
    i = ir_get(0);
    assert(i->op == OP_JE);
    assert(i->temp_src1 == 0);
    assert(i->label_idx == 15);
    printf("  emit_je: OK\n");
    
    /* Test 8: Ret helper */
    ir_reset();
    ir_emit_ret();
    assert(ir_get_count() == 1);
    i = ir_get(0);
    assert(i->op == OP_RET);
    printf("  emit_ret: OK\n");
    
    printf("All IR opcode tests passed!\n");
    return 0;
}
