#include <stdio.h>
#include <assert.h>
#include <string.h>
#include "zero_cc/common/arena.h"
#include "zero_cc/parser.h"
#include "zero_cc/semantic/type.h"
#include "zero_cc/semantic/scope.h"
#include "zero_cc/semantic/diagnostics.h"
#include "zero_cc/semantic/visitor.h"

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
    assert(ctx.perm_arena == perm);
    assert(ctx.tmp_arena == tmp);
    assert(ctx.diags == &diags);
    assert(ctx.current_scope != NULL);
    assert(ctx.current_fn_ret_type == NULL);
    assert(!ctx.in_loop);
    assert(!ctx.in_switch);

    /* 1. Literal Nodes & node_type / node_set_type checking */
    ASTRef int_node_ref = zero_cc_ast_alloc_node(&ast_arena, NODE_INT);
    ZeroCCASTNode *int_node = zero_cc_ast_get(&ast_arena, int_node_ref);
    int_node->int_val = 42;

    TypeInfo *ti = sem_analyze_node(&ctx, int_node);
    assert(ti != NULL);
    assert(type_is_integer(ti));
    assert(node_type(&ctx, int_node) == ti);

    ASTRef float_node_ref = zero_cc_ast_alloc_node(&ast_arena, NODE_FLOAT);
    ZeroCCASTNode *float_node = zero_cc_ast_get(&ast_arena, float_node_ref);
    float_node->float_val = 3.14;
    TypeInfo *tf = sem_analyze_node(&ctx, float_node);
    assert(tf != NULL);
    assert(type_is_floating(tf));
    assert(node_type(&ctx, float_node) == tf);

    /* 2. Binary Op (int + float -> double) */
    ASTRef bin_node_ref = zero_cc_ast_alloc_node(&ast_arena, NODE_BINOP);
    ZeroCCASTNode *bin_node = zero_cc_ast_get(&ast_arena, bin_node_ref);
    bin_node->aux.op = '+';
    bin_node->left = int_node_ref;
    bin_node->right = float_node_ref;

    TypeInfo *tb = sem_analyze_node(&ctx, bin_node);
    assert(tb != NULL);
    assert(tb->kind == TYPE_KIND_DOUBLE);
    assert(node_type(&ctx, bin_node) == tb);

    /* 3. Declarations and Identifiers */
    ASTRef decl_ref = zero_cc_ast_alloc_node(&ast_arena, NODE_DECL);
    ZeroCCASTNode *decl_node = zero_cc_ast_get(&ast_arena, decl_ref);
    decl_node->s.str = "x";
    decl_node->left = int_node_ref; // init with int

    TypeInfo *tdecl = sem_analyze_node(&ctx, decl_node);
    assert(tdecl != NULL);
    assert(type_is_integer(tdecl));
    assert(scope_lookup(ctx.current_scope, "x") != NULL);

    /* Read identifier 'x' */
    ASTRef ident_ref = zero_cc_ast_alloc_node(&ast_arena, NODE_IDENT);
    ZeroCCASTNode *ident_node = zero_cc_ast_get(&ast_arena, ident_ref);
    ident_node->s.str = "x";

    TypeInfo *tident = sem_analyze_node(&ctx, ident_node);
    assert(tident != NULL);
    assert(type_is_integer(tident));

    /* Undeclared identifier should emit diagnostic */
    size_t err_count_before = diags.count;
    ASTRef unknown_ref = zero_cc_ast_alloc_node(&ast_arena, NODE_IDENT);
    ZeroCCASTNode *unknown_node = zero_cc_ast_get(&ast_arena, unknown_ref);
    unknown_node->s.str = "undeclared_var";
    unknown_node->line = 10;
    unknown_node->col = 5;

    TypeInfo *tunk = sem_analyze_node(&ctx, unknown_node);
    assert(tunk == NULL);
    assert(diags.count == err_count_before + 1);
    assert(diag_has_errors(&diags));

    /* 4. Scoping in Blocks */
    ASTRef block_ref = zero_cc_ast_alloc_node(&ast_arena, NODE_BLOCK);
    ZeroCCASTNode *block_node = zero_cc_ast_get(&ast_arena, block_ref);

    ASTRef inner_decl_ref = zero_cc_ast_alloc_node(&ast_arena, NODE_DECL);
    ZeroCCASTNode *inner_decl = zero_cc_ast_get(&ast_arena, inner_decl_ref);
    inner_decl->s.str = "inner_y";
    inner_decl->left = float_node_ref;

    block_node->left = inner_decl_ref;

    sem_walk_node(&ctx, block_node);
    /* After block exit, inner_y should not be visible in global scope */
    assert(scope_lookup(ctx.current_scope, "inner_y") == NULL);

    /* 5. Control Flow Validation (break outside loop/switch) */
    ASTRef break_ref = zero_cc_ast_alloc_node(&ast_arena, NODE_BREAK);
    ZeroCCASTNode *break_node = zero_cc_ast_get(&ast_arena, break_ref);
    break_node->line = 20;
    break_node->col = 1;

    size_t prev_diags = diags.count;
    sem_analyze_node(&ctx, break_node);
    assert(diags.count == prev_diags + 1);

    /* Break inside while loop should succeed without new error */
    ASTRef while_ref = zero_cc_ast_alloc_node(&ast_arena, NODE_WHILE);
    ZeroCCASTNode *while_node = zero_cc_ast_get(&ast_arena, while_ref);
    while_node->left = int_node_ref;  // cond
    while_node->right = break_ref;    // body

    prev_diags = diags.count;
    sem_analyze_node(&ctx, while_node);
    assert(diags.count == prev_diags);

    /* 6. Cleanup */
    sem_context_destroy(&ctx);
    zero_cc_ast_arena_free(&ast_arena);
    arena_destroy(tmp);
    arena_destroy(perm);

    printf("visitor_test passed!\n");
    return 0;
}
