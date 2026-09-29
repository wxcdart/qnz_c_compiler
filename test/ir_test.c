/* IR Infrastructure Test */
#include <stdio.h>
#include <assert.h>
#include "zero_cc/parser/ir.h"

int main(void) {
    printf("Testing IR infrastructure...\n");
    
    /* Test 1: Reset */
    ir_reset();
    assert(instr_count == 0);
    printf("  Reset: OK\n");
    
    /* Test 2: Emit instructions */
    Instr instr1 = {
        .op = 1,
        .temp_dest = 0,
        .temp_src1 = 1,
        .temp_src2 = 2,
        .imm = 42,
        .label_idx = 0
    };
    ir_emit(instr1);
    assert(instr_count == 1);
    printf("  Emit: OK\n");
    
    /* Test 3: Get instruction */
    Instr* i = ir_get(0);
    assert(i != NULL);
    assert(i->op == 1);
    assert(i->temp_dest == 0);
    assert(i->imm == 42);
    printf("  Get: OK\n");
    
    /* Test 4: Multiple emissions */
    ir_reset();
    for (int j = 0; j < 100; j++) {
        Instr instr = {
            .op = j % 256,
            .temp_dest = j,
            .temp_src1 = j + 1,
            .temp_src2 = j + 2,
            .imm = j * 2,
            .label_idx = 0
        };
        ir_emit(instr);
    }
    assert(instr_count == 100);
    printf("  Multiple emissions: OK\n");
    
    /* Test 5: Reset after emissions */
    ir_reset();
    assert(instr_count == 0);
    printf("  Reset after emissions: OK\n");
    
    printf("All IR tests passed!\n");
    return 0;
}
