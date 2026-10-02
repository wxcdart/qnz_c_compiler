#include <stdio.h>
#include <assert.h>
#include <string.h>
#include "zero_cc/common/arena.h"
#include "zero_cc/parser/parser.h"
#include "zero_cc/semantic/type.h"
#include "zero_cc/semantic/scope.h"
#include "zero_cc/semantic/promote.h"
#include "zero_cc/semantic/diagnostics.h"
#include "zero_cc/semantic/visitor.h"
#include "zero_cc/semantic/decl.h"

int main(void) {
    Arena *perm = arena_create(1024 * 1024);
    Arena *tmp = arena_create(1024 * 1024);
    assert(perm != NULL && tmp != NULL);

    DiagBuf diags;
    diag_init(&diags, tmp);

    ZeroCCASTArena ast_arena;
    zero_cc_ast_arena_init(&ast_arena, 1024);

    SemContext ctx;
    bool ok = sem_context_init(&ctx, perm, tmp, &diags, &ast_arena);
    assert(ok);
    assert(ctx.current_scope != NULL);

    TypeInfo *t_int = type_int(perm);
    TypeInfo *t_double = type_double(perm);
    TypeInfo *t_int_ptr = type_pointer_to(perm, t_int);

    /* -------------------------------------------------------------
     * 1. Variable Declarations with Valid and Invalid Initializers
     * ------------------------------------------------------------- */
    printf("Testing variable declarations (valid and invalid initializers)...\n");
    {
        /* 1a. Variable declaration without initializer: int a; */
        ASTRef r_decl_a = zero_cc_ast_alloc_node(&ast_arena, NODE_DECL);
        ZeroCCASTNode *n_decl_a = zero_cc_ast_get(&ast_arena, r_decl_a);
        n_decl_a->s.str = "a";
        n_decl_a->s.len = 1;
        decl_node_set_type(&ctx, n_decl_a, t_int);

        TypeInfo *ty = decl_handle_var(&ctx, n_decl_a);
        assert(ty != NULL);
        assert(type_equal(ty, t_int));
        assert(!diag_has_errors(&diags));

        Symbol *sym_a = scope_lookup_current(ctx.current_scope, "a");
        assert(sym_a != NULL);
        assert(sym_a->kind == SYMBOL_VAR);
        assert(type_equal(sym_a->type, t_int));

        /* 1b. Variable declaration with valid initializer: int b = 42; */
        ASTRef r_init_42 = zero_cc_ast_alloc_node(&ast_arena, NODE_INT);
        ZeroCCASTNode *n_init_42 = zero_cc_ast_get(&ast_arena, r_init_42);
        n_init_42->int_val = 42;

        ASTRef r_decl_b = zero_cc_ast_alloc_node(&ast_arena, NODE_DECL);
        ZeroCCASTNode *n_decl_b = zero_cc_ast_get(&ast_arena, r_decl_b);
        n_decl_b->s.str = "b";
        n_decl_b->s.len = 1;
        n_decl_b->left = r_init_42;
        decl_node_set_type(&ctx, n_decl_b, t_int);

        ty = decl_handle_var(&ctx, n_decl_b);
        assert(ty != NULL);
        assert(type_equal(ty, t_int));
        assert(!diag_has_errors(&diags));

        Symbol *sym_b = scope_lookup_current(ctx.current_scope, "b");
        assert(sym_b != NULL);
        assert(sym_b->kind == SYMBOL_VAR);
        assert(type_equal(sym_b->type, t_int));

        /* 1c. Valid arithmetic conversion initializer: double c = 42; */
        ASTRef r_init_int = zero_cc_ast_alloc_node(&ast_arena, NODE_INT);
        ZeroCCASTNode *n_init_int = zero_cc_ast_get(&ast_arena, r_init_int);
        n_init_int->int_val = 42;

        ASTRef r_decl_c = zero_cc_ast_alloc_node(&ast_arena, NODE_DECL);
        ZeroCCASTNode *n_decl_c = zero_cc_ast_get(&ast_arena, r_decl_c);
        n_decl_c->s.str = "c";
        n_decl_c->s.len = 1;
        n_decl_c->left = r_init_int;
        decl_node_set_type(&ctx, n_decl_c, t_double);

        ty = decl_handle_var(&ctx, n_decl_c);
        assert(ty != NULL);
        assert(type_equal(ty, t_double));
        assert(!diag_has_errors(&diags));

        /* 1d. Incompatible initializer: int *p = 3.14 (double into pointer -> error) */
        size_t diags_before = diags.count;
        ASTRef r_init_float = zero_cc_ast_alloc_node(&ast_arena, NODE_FLOAT);
        ZeroCCASTNode *n_init_float = zero_cc_ast_get(&ast_arena, r_init_float);
        n_init_float->float_val = 3.14;

        ASTRef r_decl_p = zero_cc_ast_alloc_node(&ast_arena, NODE_DECL);
        ZeroCCASTNode *n_decl_p = zero_cc_ast_get(&ast_arena, r_decl_p);
        n_decl_p->s.str = "p";
        n_decl_p->s.len = 1;
        n_decl_p->left = r_init_float;
        decl_node_set_type(&ctx, n_decl_p, t_int_ptr);

        ty = decl_handle_var(&ctx, n_decl_p);
        assert(ty == NULL);
        assert(diags.count > diags_before);
        assert(diag_has_errors(&diags));
    }

    /* -------------------------------------------------------------
     * 2. Duplicate Symbol in Same Scope (Redefinition Diagnostic)
     * ------------------------------------------------------------- */
    printf("Testing duplicate symbol in same scope (redefinition diagnostic)...\n");
    {
        /* 'a' is already in ctx.current_scope from section 1 */
        size_t diags_before = diags.count;

        ASTRef r_dup_a = zero_cc_ast_alloc_node(&ast_arena, NODE_DECL);
        ZeroCCASTNode *n_dup_a = zero_cc_ast_get(&ast_arena, r_dup_a);
        n_dup_a->s.str = "a";
        n_dup_a->s.len = 1;
        decl_node_set_type(&ctx, n_dup_a, t_int);

        TypeInfo *ty = decl_handle_var(&ctx, n_dup_a);
        assert(ty == NULL);
        assert(diags.count > diags_before);

        /* Shadowing in an inner scope should succeed without diagnostic */
        Scope *inner = scope_enter(&ctx.current_scope, perm);
        assert(inner != NULL);
        diags_before = diags.count;

        ASTRef r_shadow_a = zero_cc_ast_alloc_node(&ast_arena, NODE_DECL);
        ZeroCCASTNode *n_shadow_a = zero_cc_ast_get(&ast_arena, r_shadow_a);
        n_shadow_a->s.str = "a";
        n_shadow_a->s.len = 1;
        decl_node_set_type(&ctx, n_shadow_a, t_double);

        ty = decl_handle_var(&ctx, n_shadow_a);
        assert(ty != NULL);
        assert(type_equal(ty, t_double));
        assert(diags.count == diags_before);

        Symbol *inner_sym = scope_lookup_current(ctx.current_scope, "a");
        assert(inner_sym != NULL);
        assert(type_equal(inner_sym->type, t_double));

        /* Pop inner scope */
        scope_leave(&ctx.current_scope);

        /* Outside inner scope, 'a' still has type int */
        Symbol *outer_sym = scope_lookup_current(ctx.current_scope, "a");
        assert(outer_sym != NULL);
        assert(type_equal(outer_sym->type, t_int));
    }

    /* -------------------------------------------------------------
     * 3. Typedef Registration and Lookup
     * ------------------------------------------------------------- */
    printf("Testing typedef registration and lookup...\n");
    {
        /* typedef int my_int_t; */
        ASTRef r_tdef = zero_cc_ast_alloc_node(&ast_arena, NODE_TYPEDEF_DECL);
        ZeroCCASTNode *n_tdef = zero_cc_ast_get(&ast_arena, r_tdef);
        n_tdef->s.str = "my_int_t";
        n_tdef->s.len = 8;
        decl_node_set_type(&ctx, n_tdef, t_int);

        TypeInfo *ty = decl_handle_typedef(&ctx, n_tdef);
        assert(ty != NULL);
        assert(type_equal(ty, t_int));

        Symbol *sym_td = scope_lookup(ctx.current_scope, "my_int_t");
        assert(sym_td != NULL);
        assert(sym_td->kind == SYMBOL_TYPEDEF);
        assert(type_equal(sym_td->type, t_int));

        /* Duplicate typedef in same scope produces diagnostic */
        size_t diags_before = diags.count;
        ASTRef r_tdef_dup = zero_cc_ast_alloc_node(&ast_arena, NODE_TYPEDEF_DECL);
        ZeroCCASTNode *n_tdef_dup = zero_cc_ast_get(&ast_arena, r_tdef_dup);
        n_tdef_dup->s.str = "my_int_t";
        n_tdef_dup->s.len = 8;
        decl_node_set_type(&ctx, n_tdef_dup, t_int);

        ty = decl_handle_typedef(&ctx, n_tdef_dup);
        assert(ty == NULL);
        assert(diags.count > diags_before);

        /* Typedef can be used to type a variable */
        ASTRef r_var_with_td = zero_cc_ast_alloc_node(&ast_arena, NODE_DECL);
        ZeroCCASTNode *n_var_with_td = zero_cc_ast_get(&ast_arena, r_var_with_td);
        n_var_with_td->s.str = "td_var";
        n_var_with_td->s.len = 6;
        decl_node_set_type(&ctx, n_var_with_td, sym_td->type);

        ty = decl_handle_var(&ctx, n_var_with_td);
        assert(ty != NULL);
        assert(type_equal(ty, t_int));
    }

    /* -------------------------------------------------------------
     * 4. Function Prototype and Definition Validation
     * ------------------------------------------------------------- */
    printf("Testing function prototype and definition validation...\n");
    {
        /* Function prototype: int add(int, int); */
        TypeInfo *params[2] = { t_int, t_int };
        TypeInfo *t_fn_add = type_function(perm, t_int, params, 2, false);

        ASTRef r_proto = zero_cc_ast_alloc_node(&ast_arena, NODE_DECL);
        ZeroCCASTNode *n_proto = zero_cc_ast_get(&ast_arena, r_proto);
        n_proto->s.str = "add";
        n_proto->s.len = 3;
        decl_node_set_type(&ctx, n_proto, t_fn_add);

        TypeInfo *ty = decl_handle_function_proto(&ctx, n_proto);
        assert(ty != NULL);
        assert(type_equal(ty, t_fn_add));

        Symbol *fn_sym = scope_lookup(ctx.current_scope, "add");
        assert(fn_sym != NULL);
        assert(fn_sym->kind == SYMBOL_FUNC);
        assert(type_equal(fn_sym->type, t_fn_add));

        /* Repeated identical prototype declaration is valid in C */
        size_t diags_before = diags.count;
        ty = decl_handle_function_proto(&ctx, n_proto);
        assert(ty != NULL);
        assert(diags.count == diags_before);

        /* Incompatible redeclaration (e.g. double add(int, int)) produces diag */
        TypeInfo *t_fn_incompat = type_function(perm, t_double, params, 2, false);
        ASTRef r_bad_proto = zero_cc_ast_alloc_node(&ast_arena, NODE_DECL);
        ZeroCCASTNode *n_bad_proto = zero_cc_ast_get(&ast_arena, r_bad_proto);
        n_bad_proto->s.str = "add";
        n_bad_proto->s.len = 3;
        decl_node_set_type(&ctx, n_bad_proto, t_fn_incompat);

        ty = decl_handle_function_proto(&ctx, n_bad_proto);
        assert(ty == NULL);
        assert(diags.count > diags_before);

        /* Function definition with body:
         * int compute(int x) { return x; }
         */
        TypeInfo *compute_params[1] = { t_int };
        TypeInfo *t_fn_compute = type_function(perm, t_int, compute_params, 1, false);

        /* Parameter node: int x */
        ASTRef r_param_x = zero_cc_ast_alloc_node(&ast_arena, NODE_DECL);
        ZeroCCASTNode *n_param_x = zero_cc_ast_get(&ast_arena, r_param_x);
        n_param_x->s.str = "x";
        n_param_x->s.len = 1;
        decl_node_set_type(&ctx, n_param_x, t_int);

        /* Return statement: return x; */
        ASTRef r_ret_ident = zero_cc_ast_alloc_node(&ast_arena, NODE_IDENT);
        ZeroCCASTNode *n_ret_ident = zero_cc_ast_get(&ast_arena, r_ret_ident);
        n_ret_ident->s.str = "x";
        n_ret_ident->s.len = 1;

        ASTRef r_ret_stmt = zero_cc_ast_alloc_node(&ast_arena, NODE_RETURN);
        ZeroCCASTNode *n_ret_stmt = zero_cc_ast_get(&ast_arena, r_ret_stmt);
        n_ret_stmt->left = r_ret_ident;

        /* Body block containing return statement */
        ASTRef r_body = zero_cc_ast_alloc_node(&ast_arena, NODE_BLOCK);
        ZeroCCASTNode *n_body = zero_cc_ast_get(&ast_arena, r_body);
        n_body->left = r_ret_stmt;

        /* Function definition node */
        ASTRef r_def = zero_cc_ast_alloc_node(&ast_arena, NODE_DECL);
        ZeroCCASTNode *n_def = zero_cc_ast_get(&ast_arena, r_def);
        n_def->s.str = "compute";
        n_def->s.len = 7;
        n_def->left = r_param_x;   /* parameters */
        n_def->right = r_body;     /* body */
        decl_node_set_type(&ctx, n_def, t_fn_compute);

        diags_before = diags.count;
        ty = decl_handle_function_proto(&ctx, n_def);
        assert(ty != NULL);
        assert(type_equal(ty, t_fn_compute));
        assert(diags.count == diags_before);

        /* Check that parameter 'x' is not leaked to global scope */
        assert(scope_lookup_current(ctx.current_scope, "x") == NULL);
        assert(ctx.current_fn_ret_type == NULL);
    }

    /* -------------------------------------------------------------
     * 5. Struct/Union/Enum Tag Registration
     * ------------------------------------------------------------- */
    printf("Testing struct/union/enum tag registration...\n");
    {
        TypeInfo *tag_ty = decl_handle_tag(&ctx, "Point", TYPE_KIND_STRUCT, 16, 8);
        assert(tag_ty != NULL);
        assert(tag_ty->kind == TYPE_KIND_STRUCT);
        assert(strcmp(tag_ty->details.record.tag, "Point") == 0);
        assert(tag_ty->details.record.size == 16);
        assert(tag_ty->details.record.align == 8);

        Symbol *tag_sym = scope_lookup(ctx.current_scope, "Point");
        assert(tag_sym != NULL);
        assert(tag_sym->kind == SYMBOL_TAG);
        assert(tag_sym->type == tag_ty);

        /* Redefinition of tag in same scope produces diagnostic */
        size_t diags_before = diags.count;
        TypeInfo *dup_tag = decl_handle_tag(&ctx, "Point", TYPE_KIND_STRUCT, 16, 8);
        assert(dup_tag == NULL);
        assert(diags.count > diags_before);
    }

    sem_context_destroy(&ctx);
    zero_cc_ast_arena_free(&ast_arena);
    arena_destroy(perm);
    arena_destroy(tmp);

    printf("All declaration semantic tests passed successfully!\n");
    return 0;
}
