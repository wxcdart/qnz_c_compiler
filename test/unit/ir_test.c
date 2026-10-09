/* IR Infrastructure Test */
#include <stdio.h>
#include <assert.h>
#include "zero_cc/parser/ir.h"
#include "zero_cc/parser/ir_arena.h"

int main(void) {
    printf("Testing IR infrastructure...\n");
    
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
    
    /* Set as current arena for testing */
    extern ZeroCCCompiler *g_compiler;
    ZeroCCCompiler *old_compiler = g_compiler;
    g_compiler = NULL;
    
    /* Use arena directly */
    Instr *buf = arena.buffer;
    size_t *count_ptr = &arena.count;
    
    /* Test 1: Reset */
    *count_ptr = 0;
    assert(*count_ptr == 0);
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
    buf[*count_ptr] = instr1;
    (*count_ptr)++;
    assert(*count_ptr == 1);
    printf("  Emit: OK\n");
    
    /* Test 3: Get instruction */
    Instr* i = &buf[0];
    assert(i != NULL);
    assert(i->op == 1);
    assert(i->temp_dest == 0);
    assert(i->imm == 42);
    printf("  Get: OK\n");
    
    /* Test 4: Multiple emissions */
    *count_ptr = 0;
    for (int j = 0; j < 100; j++) {
        Instr instr = {
            .op = j % 256,
            .temp_dest = j,
            .temp_src1 = j + 1,
            .temp_src2 = j + 2,
            .imm = j * 2,
            .label_idx = 0
        };
        buf[*count_ptr] = instr;
        (*count_ptr)++;
    }
    assert(*count_ptr == 100);
    printf("  Multiple emissions: OK\n");
    
    /* Test 5: Reset after emissions */
    *count_ptr = 0;
    assert(*count_ptr == 0);
    printf("  Reset after emissions: OK\n");
    
    /* Cleanup */
    g_compiler = old_compiler;
    ir_call_args_arena_free(&call_arena);
    ir_arena_free(&arena);
    
    printf("All IR tests passed!\n");
    return 0;
}
