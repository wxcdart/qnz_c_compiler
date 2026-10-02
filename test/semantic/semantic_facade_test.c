#include <stdio.h>
#include <assert.h>
#include <string.h>
#include "zero_cc/common/arena.h"
#include "zero_cc/parser.h"
#include "zero_cc/semantic.h"
#include "zero_cc/semantic/type.h"

int main(void) {
    printf("Starting semantic_facade_test...\n");

    Arena *perm = arena_create(1024 * 1024);
    Arena *tmp = arena_create(1024 * 1024);
    assert(perm != NULL && tmp != NULL);

    /* -------------------------------------------------------------
     * Test Case 1: Valid AST root
     * int x = 42;
     * Returns true, 0 diagnostics.
     * ------------------------------------------------------------- */
    {
        DiagBuf diags;
        diag_init(&diags, tmp);

        ZeroCCASTArena ast_arena;
        zero_cc_ast_arena_init(&ast_arena, 256);

        /* Node: 42 */
        ASTRef int_val_ref = zero_cc_ast_alloc_node(&ast_arena, NODE_INT);
        ZeroCCASTNode *int_val_node = zero_cc_ast_get(&ast_arena, int_val_ref);
        int_val_node->int_val = 42;
        int_val_node->line = 1;
        int_val_node->col = 9;

        /* Node: int x = 42; */
        ASTRef decl_ref = zero_cc_ast_alloc_node(&ast_arena, NODE_DECL);
        ZeroCCASTNode *decl_node = zero_cc_ast_get(&ast_arena, decl_ref);
        decl_node->s.str = "x";
        decl_node->s.len = 1;
        decl_node->left = int_val_ref;
        decl_node->line = 1;
        decl_node->col = 1;

        bool success = semantic_analyze(&ast_arena, decl_ref, perm, tmp, &diags);
        assert(success == true);
        assert(!diag_has_errors(&diags));
        assert(diags.count == 0);

        zero_cc_ast_arena_free(&ast_arena);
    }

    /* -------------------------------------------------------------
     * Test Case 2: Invalid AST root - Break outside loop / switch
     * break;
     * Returns false, diags has error, prints cleanly via semantic_print_diagnostics.
     * ------------------------------------------------------------- */
    {
        DiagBuf diags;
        diag_init(&diags, tmp);

        ZeroCCASTArena ast_arena;
        zero_cc_ast_arena_init(&ast_arena, 256);

        ASTRef break_ref = zero_cc_ast_alloc_node(&ast_arena, NODE_BREAK);
        ZeroCCASTNode *break_node = zero_cc_ast_get(&ast_arena, break_ref);
        break_node->line = 10;
        break_node->col = 5;

        bool success = semantic_analyze(&ast_arena, break_ref, perm, tmp, &diags);
        assert(success == false);
        assert(diag_has_errors(&diags));
        assert(diags.count > 0);

        /* Verify semantic_print_diagnostics works and prints formatted errors */
        printf("Expected diagnostic output for break outside loop:\n");
        semantic_print_diagnostics(&diags, stdout);

        zero_cc_ast_arena_free(&ast_arena);
    }

    /* -------------------------------------------------------------
     * Test Case 3: Invalid AST root - Incompatible assignment
     * int *p; p = 3.14;
     * Assignment of double into pointer.
     * Returns false, diags has error.
     * ------------------------------------------------------------- */
    {
        DiagBuf diags;
        diag_init(&diags, tmp);

        ZeroCCASTArena ast_arena;
        zero_cc_ast_arena_init(&ast_arena, 256);

        /* AST block with:
         * 1) int *p;
         * 2) p = 3.14;
         */
        TypeInfo *t_int = type_int(perm);
        TypeInfo *t_ptr = type_pointer_to(perm, t_int);

        /* Decl: int *p; */
        ASTRef decl_p_ref = zero_cc_ast_alloc_node(&ast_arena, NODE_DECL);
        ZeroCCASTNode *decl_p_node = zero_cc_ast_get(&ast_arena, decl_p_ref);
        decl_p_node->s.str = "p";
        decl_p_node->s.len = 1;
        decl_p_node->line = 20;
        decl_p_node->col = 1;
        /* Associate explicit type with decl node */
        size_t off = arena_index_of(perm, t_ptr);
        assert(off != (size_t)-1);
        decl_p_node->aux.extra = (uint32_t)(off + 1);

        /* Ident: p */
        ASTRef ident_p_ref = zero_cc_ast_alloc_node(&ast_arena, NODE_IDENT);
        ZeroCCASTNode *ident_p_node = zero_cc_ast_get(&ast_arena, ident_p_ref);
        ident_p_node->s.str = "p";
        ident_p_node->s.len = 1;
        ident_p_node->line = 21;
        ident_p_node->col = 1;

        /* Float: 3.14 */
        ASTRef float_ref = zero_cc_ast_alloc_node(&ast_arena, NODE_FLOAT);
        ZeroCCASTNode *float_node = zero_cc_ast_get(&ast_arena, float_ref);
        float_node->float_val = 3.14;
        float_node->line = 21;
        float_node->col = 5;

        /* Assign: p = 3.14 */
        ASTRef assign_ref = zero_cc_ast_alloc_node(&ast_arena, NODE_ASSIGN);
        ZeroCCASTNode *assign_node = zero_cc_ast_get(&ast_arena, assign_ref);
        assign_node->aux.op = '=';
        assign_node->left = ident_p_ref;
        assign_node->right = float_ref;
        assign_node->line = 21;
        assign_node->col = 3;

        /* Block containing decl_p followed by assign */
        ASTRef block_ref = zero_cc_ast_alloc_node(&ast_arena, NODE_BLOCK);
        ZeroCCASTNode *block_node = zero_cc_ast_get(&ast_arena, block_ref);
        block_node->left = decl_p_ref;
        decl_p_node->next = assign_ref;
        assign_node->next = AST_NULL;

        bool success = semantic_analyze(&ast_arena, block_ref, perm, tmp, &diags);
        assert(success == false);
        assert(diag_has_errors(&diags));
        assert(diags.count > 0);

        printf("Expected diagnostic output for incompatible assignment:\n");
        semantic_print_diagnostics(&diags, stdout);

        zero_cc_ast_arena_free(&ast_arena);
    }

    arena_destroy(tmp);
    arena_destroy(perm);

    printf("semantic_facade_test passed successfully!\n");
    return 0;
}
