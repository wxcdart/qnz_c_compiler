#include <stdio.h>
#include <assert.h>
#include <string.h>
#include "../src/zero_cc/arena.h"
#include "../src/zero_cc/parser.h"
#include "../src/zero_cc/semantic/type.h"
#include "../src/zero_cc/semantic/scope.h"
#include "../src/zero_cc/semantic/diagnostics.h"
#include "../src/zero_cc/semantic/visitor.h"
#include "../src/zero_cc/semantic/stmt.h"

int main(void) {
    Arena *perm = arena_create(1024 * 1024);
    Arena *tmp = arena_create(1024 * 1024);
    assert(perm != NULL && tmp != NULL);

    DiagBuf diags;
    diag_init(&diags, tmp);

    ZeroCCASTArena ast_arena;
    zero_cc_ast_arena_init(&ast_arena, 256);

    SemContext ctx;
    bool ok = sem_context_init(&ctx, perm, tmp, &diags, &ast_arena);
    assert(ok);

    /* ---------------------------------------------------------
     * Test 1: Return type checking
     * (matching vs incompatible return type produces diag)
     * --------------------------------------------------------- */
    printf("Testing return statement validation...\n");

    /* Case 1a: Return in int function with int expr -> OK */
    ctx.current_fn_ret_type = type_int(perm);
    size_t err_count = diags.count;

    ASTRef int_ret_expr = zero_cc_ast_alloc_node(&ast_arena, NODE_INT);
    ZeroCCASTNode *int_ret_node = zero_cc_ast_get(&ast_arena, int_ret_expr);
    int_ret_node->int_val = 100;

    ASTRef ret1_ref = zero_cc_ast_alloc_node(&ast_arena, NODE_RETURN);
    ZeroCCASTNode *ret1_node = zero_cc_ast_get(&ast_arena, ret1_ref);
    ret1_node->left = int_ret_expr;

    sem_analyze_node(&ctx, ret1_node);
    assert(diags.count == err_count); /* No new errors */

    /* Case 1b: Return in int function with incompatible pointer type -> error diag */
    ASTRef str_ret_expr = zero_cc_ast_alloc_node(&ast_arena, NODE_STRING);
    ZeroCCASTNode *str_ret_node = zero_cc_ast_get(&ast_arena, str_ret_expr);
    str_ret_node->s.str = "hello";

    ASTRef ret2_ref = zero_cc_ast_alloc_node(&ast_arena, NODE_RETURN);
    ZeroCCASTNode *ret2_node = zero_cc_ast_get(&ast_arena, ret2_ref);
    ret2_node->left = str_ret_expr;

    sem_analyze_node(&ctx, ret2_node);
    assert(diags.count == err_count + 1); /* Incompatible return type */
    err_count = diags.count;

    /* Case 1c: Return in void function with value -> error diag */
    ctx.current_fn_ret_type = type_void(perm);

    ASTRef ret3_ref = zero_cc_ast_alloc_node(&ast_arena, NODE_RETURN);
    ZeroCCASTNode *ret3_node = zero_cc_ast_get(&ast_arena, ret3_ref);
    ret3_node->left = int_ret_expr;

    sem_analyze_node(&ctx, ret3_node);
    assert(diags.count == err_count + 1); /* void function should not return a value */
    err_count = diags.count;

    /* Case 1d: Return in non-void function with no value -> error diag */
    ctx.current_fn_ret_type = type_int(perm);

    ASTRef ret4_ref = zero_cc_ast_alloc_node(&ast_arena, NODE_RETURN);
    ZeroCCASTNode *ret4_node = zero_cc_ast_get(&ast_arena, ret4_ref);
    ret4_node->left = AST_NULL;

    sem_analyze_node(&ctx, ret4_node);
    assert(diags.count == err_count + 1); /* non-void function should return a value */
    err_count = diags.count;

    /* Case 1e: Return in void function with no value -> OK */
    ctx.current_fn_ret_type = type_void(perm);

    ASTRef ret5_ref = zero_cc_ast_alloc_node(&ast_arena, NODE_RETURN);
    ZeroCCASTNode *ret5_node = zero_cc_ast_get(&ast_arena, ret5_ref);
    ret5_node->left = AST_NULL;

    sem_analyze_node(&ctx, ret5_node);
    assert(diags.count == err_count);

    /* Reset ctx */
    ctx.current_fn_ret_type = NULL;

    /* ---------------------------------------------------------
     * Test 2: Scalar condition validation for if / while / for
     * --------------------------------------------------------- */
    printf("Testing scalar condition validation...\n");

    /* Case 2a: if with scalar (int) condition -> OK */
    ASTRef if1_ref = zero_cc_ast_alloc_node(&ast_arena, NODE_IF);
    ZeroCCASTNode *if1_node = zero_cc_ast_get(&ast_arena, if1_ref);
    if1_node->left = int_ret_expr; /* condition = int */
    if1_node->right = AST_NULL;
    if1_node->aux.alt = AST_NULL;

    sem_analyze_node(&ctx, if1_node);
    assert(diags.count == err_count);

    /* Case 2b: while with scalar (pointer) condition -> OK */
    ASTRef while1_ref = zero_cc_ast_alloc_node(&ast_arena, NODE_WHILE);
    ZeroCCASTNode *while1_node = zero_cc_ast_get(&ast_arena, while1_ref);
    while1_node->left = str_ret_expr; /* condition = char* (pointer is scalar) */
    while1_node->right = AST_NULL;

    sem_analyze_node(&ctx, while1_node);
    assert(diags.count == err_count);

    /* Case 2c: if with non-scalar (struct) condition -> error diag */
    TypeInfo *struct_t = type_new(perm);
    struct_t->kind = TYPE_STRUCT;
    struct_t->details.record.tag = "MyStruct";
    scope_put_symbol(ctx.current_scope, "s_val", SYMBOL_VAR, struct_t);

    ASTRef struct_ident_ref = zero_cc_ast_alloc_node(&ast_arena, NODE_IDENT);
    ZeroCCASTNode *struct_ident = zero_cc_ast_get(&ast_arena, struct_ident_ref);
    struct_ident->s.str = "s_val";

    ASTRef if2_ref = zero_cc_ast_alloc_node(&ast_arena, NODE_IF);
    ZeroCCASTNode *if2_node = zero_cc_ast_get(&ast_arena, if2_ref);
    if2_node->left = struct_ident_ref; /* struct condition */
    if2_node->right = AST_NULL;
    if2_node->aux.alt = AST_NULL;

    sem_analyze_node(&ctx, if2_node);
    assert(diags.count == err_count + 1);
    err_count = diags.count;

    /* Case 2d: while with non-scalar (void) condition -> error diag */
    TypeInfo *void_t = type_void(perm);
    scope_put_symbol(ctx.current_scope, "v_val", SYMBOL_VAR, void_t);

    ASTRef void_ident_ref = zero_cc_ast_alloc_node(&ast_arena, NODE_IDENT);
    ZeroCCASTNode *void_ident = zero_cc_ast_get(&ast_arena, void_ident_ref);
    void_ident->s.str = "v_val";

    ASTRef while2_ref = zero_cc_ast_alloc_node(&ast_arena, NODE_WHILE);
    ZeroCCASTNode *while2_node = zero_cc_ast_get(&ast_arena, while2_ref);
    while2_node->left = void_ident_ref; /* void condition */
    while2_node->right = AST_NULL;

    sem_analyze_node(&ctx, while2_node);
    assert(diags.count == err_count + 1);
    err_count = diags.count;

    /* ---------------------------------------------------------
     * Test 3: Break / Continue validation
     * (inside loop ok, outside loop produces error diag)
     * --------------------------------------------------------- */
    printf("Testing break and continue validation...\n");

    /* Case 3a: break outside loop or switch -> error diag */
    ASTRef brk1_ref = zero_cc_ast_alloc_node(&ast_arena, NODE_BREAK);
    ZeroCCASTNode *brk1_node = zero_cc_ast_get(&ast_arena, brk1_ref);

    sem_analyze_node(&ctx, brk1_node);
    assert(diags.count == err_count + 1);
    err_count = diags.count;

    /* Case 3b: continue outside loop -> error diag */
    ASTRef cont1_ref = zero_cc_ast_alloc_node(&ast_arena, NODE_CONTINUE);
    ZeroCCASTNode *cont1_node = zero_cc_ast_get(&ast_arena, cont1_ref);

    sem_analyze_node(&ctx, cont1_node);
    assert(diags.count == err_count + 1);
    err_count = diags.count;

    /* Case 3c: break inside switch -> OK */
    ASTRef switch_ref = zero_cc_ast_alloc_node(&ast_arena, NODE_SWITCH);
    ZeroCCASTNode *switch_node = zero_cc_ast_get(&ast_arena, switch_ref);
    switch_node->left = int_ret_expr; /* int condition */
    switch_node->right = brk1_ref;    /* body has break */

    sem_analyze_node(&ctx, switch_node);
    assert(diags.count == err_count);

    /* Case 3d: continue inside switch (not in loop) -> error diag */
    switch_node->right = cont1_ref;

    sem_analyze_node(&ctx, switch_node);
    assert(diags.count == err_count + 1);
    err_count = diags.count;

    /* Case 3e: break and continue inside while loop -> OK */
    ASTRef block_ref = zero_cc_ast_alloc_node(&ast_arena, NODE_BLOCK);
    ZeroCCASTNode *block_node = zero_cc_ast_get(&ast_arena, block_ref);
    block_node->left = brk1_ref;
    brk1_node->next = cont1_ref;
    cont1_node->next = AST_NULL;

    ASTRef loop_ref = zero_cc_ast_alloc_node(&ast_arena, NODE_WHILE);
    ZeroCCASTNode *loop_node = zero_cc_ast_get(&ast_arena, loop_ref);
    loop_node->left = int_ret_expr;
    loop_node->right = block_ref;

    sem_analyze_node(&ctx, loop_node);
    assert(diags.count == err_count);

    /* Case 3f: for loop scope isolation for declarations in init */
    ASTRef for_decl_ref = zero_cc_ast_alloc_node(&ast_arena, NODE_DECL);
    ZeroCCASTNode *for_decl_node = zero_cc_ast_get(&ast_arena, for_decl_ref);
    for_decl_node->s.str = "for_i";
    for_decl_node->left = int_ret_expr;

    ASTRef for_ref = zero_cc_ast_alloc_node(&ast_arena, NODE_FOR);
    ZeroCCASTNode *for_node = zero_cc_ast_get(&ast_arena, for_ref);
    for_node->left = for_decl_ref;
    for_node->right = int_ret_expr;
    for_node->aux.step = AST_NULL;
    for_node->aux.alt = AST_NULL;

    sem_analyze_node(&ctx, for_node);
    assert(diags.count == err_count);

    /* for_i should not be visible outside the for loop */
    assert(scope_lookup(ctx.current_scope, "for_i") == NULL);

    /* ---------------------------------------------------------
     * Test 4: Switch condition & case/default validation
     * --------------------------------------------------------- */
    printf("Testing switch / case / default validation...\n");

    /* Case 4a: Case outside switch -> error diag */
    ASTRef case_ref = zero_cc_ast_alloc_node(&ast_arena, NODE_CASE);
    ZeroCCASTNode *case_node = zero_cc_ast_get(&ast_arena, case_ref);
    case_node->left = int_ret_expr;
    case_node->right = AST_NULL;

    sem_analyze_node(&ctx, case_node);
    assert(diags.count == err_count + 1);
    err_count = diags.count;

    /* Case 4b: Default outside switch -> error diag */
    ASTRef def_ref = zero_cc_ast_alloc_node(&ast_arena, NODE_DEFAULT);
    ZeroCCASTNode *def_node = zero_cc_ast_get(&ast_arena, def_ref);
    def_node->left = AST_NULL;

    sem_analyze_node(&ctx, def_node);
    assert(diags.count == err_count + 1);
    err_count = diags.count;

    /* Case 4c: Switch with float condition -> error diag */
    ASTRef sw_float_ref = zero_cc_ast_alloc_node(&ast_arena, NODE_SWITCH);
    ZeroCCASTNode *sw_float_node = zero_cc_ast_get(&ast_arena, sw_float_ref);
    ASTRef float_expr = zero_cc_ast_alloc_node(&ast_arena, NODE_FLOAT);
    ZeroCCASTNode *float_node = zero_cc_ast_get(&ast_arena, float_expr);
    float_node->float_val = 1.5;
    sw_float_node->left = float_expr;
    sw_float_node->right = AST_NULL;

    sem_analyze_node(&ctx, sw_float_node);
    assert(diags.count == err_count + 1);
    err_count = diags.count;

    /* Case 4d: Switch with int condition and nested case/default -> OK */
    ASTRef sw_valid_ref = zero_cc_ast_alloc_node(&ast_arena, NODE_SWITCH);
    ZeroCCASTNode *sw_valid_node = zero_cc_ast_get(&ast_arena, sw_valid_ref);
    sw_valid_node->left = int_ret_expr;

    ASTRef sw_block_ref = zero_cc_ast_alloc_node(&ast_arena, NODE_BLOCK);
    ZeroCCASTNode *sw_block_node = zero_cc_ast_get(&ast_arena, sw_block_ref);
    sw_block_node->left = case_ref;
    case_node->next = def_ref;
    def_node->next = AST_NULL;
    sw_valid_node->right = sw_block_ref;

    sem_analyze_node(&ctx, sw_valid_node);
    assert(diags.count == err_count);

    /* Clean up */
    sem_context_destroy(&ctx);
    zero_cc_ast_arena_free(&ast_arena);
    arena_destroy(perm);
    arena_destroy(tmp);

    printf("All statement validation tests PASSED!\n");
    return 0;
}
