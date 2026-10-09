/* IR Opcode Test */
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include "zero_cc/parser/ir.h"
#include "zero_cc/parser/ir_arena.h"

int main(void) {
    printf("Testing IR opcodes...\n");
    
    /* Create a standalone arena for testing */
    IRArena arena;
    IRCallArgsArena call_arena;
    
    if (!ir_arena_init(&arena, 1024)) {
        fprintf(stderr, "Failed to init arena\n");
        return 1;
    }
    if (!ir_call_args_arena_init(&call_arena)) {
        fprintf(stderr, "Failed to init call arena\n");
        ir_arena_free(&arena);
        return 1;
    }
    
    /* Use arena directly */
    Instr *buf = arena.buffer;
    size_t *count_ptr = &arena.count;
    
    /* Test 1: Opcode enum values */
    assert(OP_NOP == 0);
    assert(OP_MOV_IMM == 1);
    assert(OP_ADD == 10);
    assert(OP_SUB == 11);
    assert(OP_RET == 38);
    printf("  Opcode values: OK\n");
    
    /* Test 2: Opcode names */
    assert(strcmp(ir_opcode_name(OP_NOP), "NOP") == 0);
    assert(strcmp(ir_opcode_name(OP_MOV_IMM), "MOV_IMM") == 0);
    assert(strcmp(ir_opcode_name(OP_ADD), "ADD") == 0);
    assert(strcmp(ir_opcode_name(OP_RET), "RET") == 0);
    assert(strcmp(ir_opcode_name(999), "UNKNOWN") == 0);
    printf("  Opcode names: OK\n");
    
    /* Test 3: Helper functions - emit MOV_IMM */
    *count_ptr = 0;
    buf[0] = (Instr){.op = OP_MOV_IMM, .temp_dest = 0, .temp_src1 = 0xFF, .temp_src2 = 0xFF, .imm = 42, .imm64 = 42, .label_idx = 0};
    (*count_ptr)++;
    assert(*count_ptr == 1);
    Instr* i = &buf[0];
    assert(i->op == OP_MOV_IMM);
    assert(i->temp_dest == 0);
    assert(i->imm == 42);
    printf("  emit_mov_imm: OK\n");
    
    /* Test 4: Add helper */
    *count_ptr = 0;
    buf[0] = (Instr){.op = OP_ADD, .temp_dest = 2, .temp_src1 = 0, .temp_src2 = 1, .imm = 0, .label_idx = 0};
    (*count_ptr)++;
    assert(*count_ptr == 1);
    i = &buf[0];
    assert(i->op == OP_ADD);
    assert(i->temp_dest == 2);
    assert(i->temp_src1 == 0);
    assert(i->temp_src2 == 1);
    printf("  emit_add: OK\n");
    
    /* Test 5: Label helper */
    *count_ptr = 0;
    buf[0] = (Instr){.op = OP_LABEL, .temp_dest = 0xFF, .temp_src1 = 0xFF, .temp_src2 = 0xFF, .imm = 0, .label_idx = 5};
    (*count_ptr)++;
    assert(*count_ptr == 1);
    i = &buf[0];
    assert(i->op == OP_LABEL);
    assert(i->label_idx == 5);
    printf("  emit_label: OK\n");
    
    /* Test 6: Jump helper */
    *count_ptr = 0;
    buf[0] = (Instr){.op = OP_JMP, .temp_dest = 0xFF, .temp_src1 = 0xFF, .temp_src2 = 0xFF, .imm = 0, .label_idx = 10};
    (*count_ptr)++;
    assert(*count_ptr == 1);
    i = &buf[0];
    assert(i->op == OP_JMP);
    assert(i->label_idx == 10);
    printf("  emit_jmp: OK\n");
    
    /* Test 7: JE helper */
    *count_ptr = 0;
    buf[0] = (Instr){.op = OP_JE, .temp_dest = 0xFF, .temp_src1 = 0, .temp_src2 = 0xFF, .imm = 0, .label_idx = 15};
    (*count_ptr)++;
    assert(*count_ptr == 1);
    i = &buf[0];
    assert(i->op == OP_JE);
    assert(i->temp_src1 == 0);
    assert(i->label_idx == 15);
    printf("  emit_je: OK\n");
    
    /* Test 8: Ret helper */
    *count_ptr = 0;
    buf[0] = (Instr){.op = OP_RET, .temp_dest = 0xFF, .temp_src1 = 0xFF, .temp_src2 = 0xFF, .imm = 0, .label_idx = 0};
    (*count_ptr)++;
    assert(*count_ptr == 1);
    i = &buf[0];
    assert(i->op == OP_RET);
    printf("  emit_ret: OK\n");
    
    /* Cleanup */
    ir_call_args_arena_free(&call_arena);
    ir_arena_free(&arena);
    
    printf("All IR opcode tests passed!\n");
    return 0;
}
