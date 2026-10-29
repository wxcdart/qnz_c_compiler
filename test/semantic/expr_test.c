#include <stdio.h>
#include <assert.h>
#include <string.h>
#include "zero_cc/common/arena.h"
#include "zero_cc/parser/parser.h"
#include "zero_cc/semantic/type.h"
#include "zero_cc/semantic/scope.h"
#include "zero_cc/semantic/promote.h"
#include "zero_cc/semantic/diagnostics.h"
#include "zero_cc/semantic/expr.h"

int main(void) {
    Arena *perm = arena_create(1024 * 1024);
    Arena *tmp = arena_create(1024 * 1024);
    assert(perm != NULL && tmp != NULL);

    DiagBuf diags;
    diag_init(&diags, tmp);

    ZeroCCASTArena ast_arena;
    zero_cc_ast_arena_init(&ast_arena, 1024);

    Scope *scope = NULL;
    scope_enter(&scope, perm);

    SemContext ctx;
    ctx.perm_arena = perm;
    ctx.tmp_arena = tmp;
    ctx.diags = &diags;
    ctx.current_scope = scope;
    ctx.current_fn_ret_type = NULL;
    ctx.in_loop = false;
    ctx.in_switch = false;
    ctx.ast_arena = &ast_arena;

    /* Base types */
    TypeInfo *t_int = type_int(perm);
    TypeInfo *t_double = type_double(perm);
    TypeInfo *t_bool = type_bool(perm);
    TypeInfo *t_char = type_char(perm);
    TypeInfo *t_char_ptr = type_pointer_to(perm, t_char);
    TypeInfo *t_void = type_void(perm);
    TypeInfo *t_void_ptr = type_pointer_to(perm, t_void);
    TypeInfo *t_int_ptr = type_pointer_to(perm, t_int);

    /* -------------------------------------------------------------
     * 1. Literal Handlers
     * ------------------------------------------------------------- */
    printf("Testing literals...\n");
    {
        /* NODE_INT */
        ASTRef r_int = zero_cc_ast_alloc_node(&ast_arena, NODE_INT);
        ZeroCCASTNode *n_int = zero_cc_ast_get(&ast_arena, r_int);
        n_int->int_val = 42;
        TypeInfo *ty = expr_handle_expression(&ctx, n_int);
        assert(ty != NULL);
        assert(type_equal(ty, t_int));
        assert(expr_node_type(&ctx, n_int) == ty);

        /* NODE_FLOAT */
        ASTRef r_float = zero_cc_ast_alloc_node(&ast_arena, NODE_FLOAT);
        ZeroCCASTNode *n_float = zero_cc_ast_get(&ast_arena, r_float);
        n_float->float_val = 3.14;
        ty = expr_handle_expression(&ctx, n_float);
        assert(ty != NULL);
        assert(type_equal(ty, t_double));
        assert(expr_node_type(&ctx, n_float) == ty);

        /* NODE_STRING */
        ASTRef r_str = zero_cc_ast_alloc_node(&ast_arena, NODE_STRING);
        ZeroCCASTNode *n_str = zero_cc_ast_get(&ast_arena, r_str);
        n_str->s.str = "hello";
        n_str->s_len = 5;
        ty = expr_handle_expression(&ctx, n_str);
        assert(ty != NULL);
        assert(type_equal(ty, t_char_ptr));
        assert(expr_node_type(&ctx, n_str) == ty);

        /* NODE_BOOL */
        ASTRef r_bool = zero_cc_ast_alloc_node(&ast_arena, NODE_BOOL);
        ZeroCCASTNode *n_bool = zero_cc_ast_get(&ast_arena, r_bool);
        n_bool->int_val = 1;
        ty = expr_handle_expression(&ctx, n_bool);
        assert(ty != NULL);
        assert(type_equal(ty, t_bool));
        assert(expr_node_type(&ctx, n_bool) == ty);

        /* NODE_NULLPTR */
        ASTRef r_null = zero_cc_ast_alloc_node(&ast_arena, NODE_NULLPTR);
        ZeroCCASTNode *n_null = zero_cc_ast_get(&ast_arena, r_null);
        ty = expr_handle_expression(&ctx, n_null);
        assert(ty != NULL);
        assert(type_equal(ty, t_void_ptr));
        assert(expr_node_type(&ctx, n_null) == ty);
    }

    /* -------------------------------------------------------------
     * 2. Identifier Lookup
     * ------------------------------------------------------------- */
    printf("Testing identifiers...\n");
    {
        /* Register variables in current scope */
        scope_put_symbol(scope, "foo", SYMBOL_VAR, t_int);
        scope_put_symbol(scope, "pi", SYMBOL_VAR, t_double);
        scope_put_symbol(scope, "ptr", SYMBOL_VAR, t_int_ptr);

        /* Lookup valid identifier "foo" */
        ASTRef r_foo = zero_cc_ast_alloc_node(&ast_arena, NODE_IDENT);
        ZeroCCASTNode *n_foo = zero_cc_ast_get(&ast_arena, r_foo);
        n_foo->s.str = "foo";
        n_foo->s_len = 3;
        TypeInfo *ty = expr_handle_expression(&ctx, n_foo);
        assert(ty != NULL);
        assert(type_equal(ty, t_int));
        assert(expr_node_type(&ctx, n_foo) == ty);

        /* Lookup undeclared identifier -> reports diagnostic error */
        size_t diags_before = diags.count;
        ASTRef r_bad = zero_cc_ast_alloc_node(&ast_arena, NODE_IDENT);
        ZeroCCASTNode *n_bad = zero_cc_ast_get(&ast_arena, r_bad);
        n_bad->s.str = "nonexistent_var";
        n_bad->s_len = 15;
        ty = expr_handle_expression(&ctx, n_bad);
        assert(ty == NULL);
        assert(diags.count > diags_before);
    }

    /* -------------------------------------------------------------
     * 3. Unary Operators
     * ------------------------------------------------------------- */
    printf("Testing unary operators...\n");
    {
        /* Address-of &foo (lvalue -> &int gives int*) */
        ASTRef r_foo = zero_cc_ast_alloc_node(&ast_arena, NODE_IDENT);
        ZeroCCASTNode *n_foo = zero_cc_ast_get(&ast_arena, r_foo);
        n_foo->s.str = "foo";
        n_foo->s_len = 3;

        ASTRef r_addr = zero_cc_ast_alloc_node(&ast_arena, NODE_UNOP);
        ZeroCCASTNode *n_addr = zero_cc_ast_get(&ast_arena, r_addr);
        n_addr->aux.op = EXPR_TK_AMP;
        n_addr->left = r_foo;
        TypeInfo *ty = expr_handle_expression(&ctx, n_addr);
        assert(ty != NULL);
        assert(type_equal(ty, t_int_ptr));

        /* Address-of on rvalue &42 -> error */
        size_t diags_before = diags.count;
        ASTRef r_lit = zero_cc_ast_alloc_node(&ast_arena, NODE_INT);
        ZeroCCASTNode *n_lit = zero_cc_ast_get(&ast_arena, r_lit);
        n_lit->int_val = 42;

        ASTRef r_bad_addr = zero_cc_ast_alloc_node(&ast_arena, NODE_UNOP);
        ZeroCCASTNode *n_bad_addr = zero_cc_ast_get(&ast_arena, r_bad_addr);
        n_bad_addr->aux.op = EXPR_TK_AMP;
        n_bad_addr->left = r_lit;
        ty = expr_handle_expression(&ctx, n_bad_addr);
        assert(ty == NULL);
        assert(diags.count > diags_before);

        /* Dereference *ptr (ptr has type int* -> dereference gives int) */
        ASTRef r_ptr = zero_cc_ast_alloc_node(&ast_arena, NODE_IDENT);
        ZeroCCASTNode *n_ptr = zero_cc_ast_get(&ast_arena, r_ptr);
        n_ptr->s.str = "ptr";
        n_ptr->s_len = 3;

        ASTRef r_deref = zero_cc_ast_alloc_node(&ast_arena, NODE_UNOP);
        ZeroCCASTNode *n_deref = zero_cc_ast_get(&ast_arena, r_deref);
        n_deref->aux.op = EXPR_TK_STAR;
        n_deref->left = r_ptr;
        ty = expr_handle_expression(&ctx, n_deref);
        assert(ty != NULL);
        assert(type_equal(ty, t_int));

        /* Dereference non-pointer *foo -> error */
        diags_before = diags.count;
        ASTRef r_bad_deref = zero_cc_ast_alloc_node(&ast_arena, NODE_UNOP);
        ZeroCCASTNode *n_bad_deref = zero_cc_ast_get(&ast_arena, r_bad_deref);
        n_bad_deref->aux.op = EXPR_TK_STAR;
        n_bad_deref->left = r_foo;
        ty = expr_handle_expression(&ctx, n_bad_deref);
        assert(ty == NULL);
        assert(diags.count > diags_before);

        /* Unary + and - on arithmetic */
        ASTRef r_neg = zero_cc_ast_alloc_node(&ast_arena, NODE_UNOP);
        ZeroCCASTNode *n_neg = zero_cc_ast_get(&ast_arena, r_neg);
        n_neg->aux.op = EXPR_TK_MINUS;
        n_neg->left = r_foo;
        ty = expr_handle_expression(&ctx, n_neg);
        assert(ty != NULL);
        assert(type_equal(ty, t_int));

        /* Logical NOT !foo -> int */
        ASTRef r_not = zero_cc_ast_alloc_node(&ast_arena, NODE_UNOP);
        ZeroCCASTNode *n_not = zero_cc_ast_get(&ast_arena, r_not);
        n_not->aux.op = EXPR_TK_BANG;
        n_not->left = r_foo;
        ty = expr_handle_expression(&ctx, n_not);
        assert(ty != NULL);
        assert(type_equal(ty, t_int));

        /* Bitwise NOT ~foo -> int */
        ASTRef r_bnot = zero_cc_ast_alloc_node(&ast_arena, NODE_UNOP);
        ZeroCCASTNode *n_bnot = zero_cc_ast_get(&ast_arena, r_bnot);
        n_bnot->aux.op = EXPR_TK_TILDE;
        n_bnot->left = r_foo;
        ty = expr_handle_expression(&ctx, n_bnot);
        assert(ty != NULL);
        assert(type_equal(ty, t_int));

        /* Increment ++foo on modifiable lvalue -> int */
        ASTRef r_inc = zero_cc_ast_alloc_node(&ast_arena, NODE_UNOP);
        ZeroCCASTNode *n_inc = zero_cc_ast_get(&ast_arena, r_inc);
        n_inc->aux.op = EXPR_TK_INC;
        n_inc->left = r_foo;
        ty = expr_handle_expression(&ctx, n_inc);
        assert(ty != NULL);
        assert(type_equal(ty, t_int));

        /* Increment on const variable -> error */
        TypeInfo *t_const_int = type_int(perm);
        t_const_int->modifiers |= TYPE_MOD_CONST;
        scope_put_symbol(scope, "c_val", SYMBOL_VAR, t_const_int);

        ASTRef r_cval = zero_cc_ast_alloc_node(&ast_arena, NODE_IDENT);
        ZeroCCASTNode *n_cval = zero_cc_ast_get(&ast_arena, r_cval);
        n_cval->s.str = "c_val";
        n_cval->s_len = 5;

        diags_before = diags.count;
        ASTRef r_bad_inc = zero_cc_ast_alloc_node(&ast_arena, NODE_UNOP);
        ZeroCCASTNode *n_bad_inc = zero_cc_ast_get(&ast_arena, r_bad_inc);
        n_bad_inc->aux.op = EXPR_TK_INC;
        n_bad_inc->left = r_cval;
        ty = expr_handle_expression(&ctx, n_bad_inc);
        assert(ty == NULL);
        assert(diags.count > diags_before);
    }

    /* -------------------------------------------------------------
     * 4. Binary Expressions
     * ------------------------------------------------------------- */
    printf("Testing binary expressions...\n");
    {
        /* Arithmetic: int + int -> int */
        ASTRef r_i1 = zero_cc_ast_alloc_node(&ast_arena, NODE_INT);
        zero_cc_ast_get(&ast_arena, r_i1)->int_val = 10;
        ASTRef r_i2 = zero_cc_ast_alloc_node(&ast_arena, NODE_INT);
        zero_cc_ast_get(&ast_arena, r_i2)->int_val = 20;

        ASTRef r_add = zero_cc_ast_alloc_node(&ast_arena, NODE_BINOP);
        ZeroCCASTNode *n_add = zero_cc_ast_get(&ast_arena, r_add);
        n_add->aux.op = '+';
        n_add->left = r_i1;
        n_add->right = r_i2;
        TypeInfo *ty = expr_handle_expression(&ctx, n_add);
        assert(ty != NULL);
        assert(type_equal(ty, t_int));

        /* Arithmetic promotion: int + double -> double */
        ASTRef r_d1 = zero_cc_ast_alloc_node(&ast_arena, NODE_FLOAT);
        zero_cc_ast_get(&ast_arena, r_d1)->float_val = 2.5;

        ASTRef r_add_d = zero_cc_ast_alloc_node(&ast_arena, NODE_BINOP);
        ZeroCCASTNode *n_add_d = zero_cc_ast_get(&ast_arena, r_add_d);
        n_add_d->aux.op = '+';
        n_add_d->left = r_i1;
        n_add_d->right = r_d1;
        ty = expr_handle_expression(&ctx, n_add_d);
        assert(ty != NULL);
        assert(type_equal(ty, t_double));

        /* Pointer arithmetic: int* + int -> int* */
        ASTRef r_ptr = zero_cc_ast_alloc_node(&ast_arena, NODE_IDENT);
        zero_cc_ast_get(&ast_arena, r_ptr)->s.str = "ptr";
        zero_cc_ast_get(&ast_arena, r_ptr)->s_len = 3;

        ASTRef r_ptr_add = zero_cc_ast_alloc_node(&ast_arena, NODE_BINOP);
        ZeroCCASTNode *n_ptr_add = zero_cc_ast_get(&ast_arena, r_ptr_add);
        n_ptr_add->aux.op = '+';
        n_ptr_add->left = r_ptr;
        n_ptr_add->right = r_i1;
        ty = expr_handle_expression(&ctx, n_ptr_add);
        assert(ty != NULL);
        assert(type_equal(ty, t_int_ptr));

        /* Pointer subtraction: int* - int* -> long (ptrdiff_t) */
        ASTRef r_ptr2 = zero_cc_ast_alloc_node(&ast_arena, NODE_IDENT);
        zero_cc_ast_get(&ast_arena, r_ptr2)->s.str = "ptr";
        zero_cc_ast_get(&ast_arena, r_ptr2)->s_len = 3;

        ASTRef r_ptr_diff = zero_cc_ast_alloc_node(&ast_arena, NODE_BINOP);
        ZeroCCASTNode *n_ptr_diff = zero_cc_ast_get(&ast_arena, r_ptr_diff);
        n_ptr_diff->aux.op = '-';
        n_ptr_diff->left = r_ptr;
        n_ptr_diff->right = r_ptr2;
        ty = expr_handle_expression(&ctx, n_ptr_diff);
        assert(ty != NULL);
        assert(type_equal(ty, type_long(perm)));

        /* Comparison: int < double -> int */
        ASTRef r_lt = zero_cc_ast_alloc_node(&ast_arena, NODE_BINOP);
        ZeroCCASTNode *n_lt = zero_cc_ast_get(&ast_arena, r_lt);
        n_lt->aux.op = '<';
        n_lt->left = r_i1;
        n_lt->right = r_d1;
        ty = expr_handle_expression(&ctx, n_lt);
        assert(ty != NULL);
        assert(type_equal(ty, t_int));

        /* Logical: int && int -> int */
        ASTRef r_and = zero_cc_ast_alloc_node(&ast_arena, NODE_BINOP);
        ZeroCCASTNode *n_and = zero_cc_ast_get(&ast_arena, r_and);
        n_and->aux.op = EXPR_TK_LOGAND;
        n_and->left = r_i1;
        n_and->right = r_i2;
        ty = expr_handle_expression(&ctx, n_and);
        assert(ty != NULL);
        assert(type_equal(ty, t_int));
    }

    /* -------------------------------------------------------------
     * 5. Assignment Expressions
     * ------------------------------------------------------------- */
    printf("Testing assignment expressions...\n");
    {
        /* Simple assignment foo = 100 */
        ASTRef r_foo = zero_cc_ast_alloc_node(&ast_arena, NODE_IDENT);
        zero_cc_ast_get(&ast_arena, r_foo)->s.str = "foo";
        zero_cc_ast_get(&ast_arena, r_foo)->s_len = 3;

        ASTRef r_val = zero_cc_ast_alloc_node(&ast_arena, NODE_INT);
        zero_cc_ast_get(&ast_arena, r_val)->int_val = 100;

        ASTRef r_assign = zero_cc_ast_alloc_node(&ast_arena, NODE_ASSIGN);
        ZeroCCASTNode *n_assign = zero_cc_ast_get(&ast_arena, r_assign);
        n_assign->aux.op = '=';
        n_assign->left = r_foo;
        n_assign->right = r_val;
        TypeInfo *ty = expr_handle_expression(&ctx, n_assign);
        assert(ty != NULL);
        assert(type_equal(ty, t_int));

        /* Assignment to non-lvalue: 42 = 10 -> error */
        size_t diags_before = diags.count;
        ASTRef r_bad_assign = zero_cc_ast_alloc_node(&ast_arena, NODE_ASSIGN);
        ZeroCCASTNode *n_bad_assign = zero_cc_ast_get(&ast_arena, r_bad_assign);
        n_bad_assign->aux.op = '=';
        n_bad_assign->left = r_val;
        n_bad_assign->right = r_val;
        ty = expr_handle_expression(&ctx, n_bad_assign);
        assert(ty == NULL);
        assert(diags.count > diags_before);

        /* Assignment to const variable -> error */
        ASTRef r_cval = zero_cc_ast_alloc_node(&ast_arena, NODE_IDENT);
        zero_cc_ast_get(&ast_arena, r_cval)->s.str = "c_val";
        zero_cc_ast_get(&ast_arena, r_cval)->s_len = 5;

        diags_before = diags.count;
        ASTRef r_const_assign = zero_cc_ast_alloc_node(&ast_arena, NODE_ASSIGN);
        ZeroCCASTNode *n_const_assign = zero_cc_ast_get(&ast_arena, r_const_assign);
        n_const_assign->aux.op = '=';
        n_const_assign->left = r_cval;
        n_const_assign->right = r_val;
        ty = expr_handle_expression(&ctx, n_const_assign);
        assert(ty == NULL);
        assert(diags.count > diags_before);

        /* Compound assignment: foo += 5 */
        ASTRef r_add_assign = zero_cc_ast_alloc_node(&ast_arena, NODE_ASSIGN);
        ZeroCCASTNode *n_add_assign = zero_cc_ast_get(&ast_arena, r_add_assign);
        n_add_assign->aux.op = EXPR_TK_ADD_ASSIGN;
        n_add_assign->left = r_foo;
        n_add_assign->right = r_val;
        ty = expr_handle_expression(&ctx, n_add_assign);
        assert(ty != NULL);
        assert(type_equal(ty, t_int));
    }

    /* -------------------------------------------------------------
     * 6. Function Calls
     * ------------------------------------------------------------- */
    printf("Testing function calls...\n");
    {
        /* Register function int my_add(int, double) — a declaration with a
         * parameter type list is a prototype (C99 6.7.5.3p9). */
        TypeInfo *params[2] = { t_int, t_double };
        TypeInfo *fn_t = type_function(perm, t_int, params, 2, false);
        fn_t->details.function.is_prototype = true;
        scope_put_symbol(scope, "my_add", SYMBOL_FUNC, fn_t);

        /* Valid call: my_add(10, 3.14) */
        ASTRef r_fn_id = zero_cc_ast_alloc_node(&ast_arena, NODE_IDENT);
        zero_cc_ast_get(&ast_arena, r_fn_id)->s.str = "my_add";
        zero_cc_ast_get(&ast_arena, r_fn_id)->s_len = 6;

        ASTRef r_arg1 = zero_cc_ast_alloc_node(&ast_arena, NODE_INT);
        zero_cc_ast_get(&ast_arena, r_arg1)->int_val = 10;
        ASTRef r_arg2 = zero_cc_ast_alloc_node(&ast_arena, NODE_FLOAT);
        zero_cc_ast_get(&ast_arena, r_arg2)->float_val = 3.14;
        zero_cc_ast_get(&ast_arena, r_arg1)->next = r_arg2;

        ASTRef r_call = zero_cc_ast_alloc_node(&ast_arena, NODE_CALL);
        ZeroCCASTNode *n_call = zero_cc_ast_get(&ast_arena, r_call);
        n_call->left = r_fn_id;
        n_call->right = r_arg1;
        TypeInfo *ty = expr_handle_expression(&ctx, n_call);
        assert(ty != NULL);
        assert(type_equal(ty, t_int));

        /* Argument count mismatch: my_add(10) -> error */
        size_t diags_before = diags.count;
        ASTRef r_bad_call = zero_cc_ast_alloc_node(&ast_arena, NODE_CALL);
        ZeroCCASTNode *n_bad_call = zero_cc_ast_get(&ast_arena, r_bad_call);
        zero_cc_ast_get(&ast_arena, r_arg1)->next = AST_NULL; // only 1 arg
        n_bad_call->left = r_fn_id;
        n_bad_call->right = r_arg1;
        ty = expr_handle_expression(&ctx, n_bad_call);
        assert(ty == NULL);
        assert(diags.count > diags_before);

        /* Incompatible argument type: pass int* to double param */
        diags_before = diags.count;
        ASTRef r_ptr_arg = zero_cc_ast_alloc_node(&ast_arena, NODE_IDENT);
        zero_cc_ast_get(&ast_arena, r_ptr_arg)->s.str = "ptr";
        zero_cc_ast_get(&ast_arena, r_ptr_arg)->s_len = 3;
        zero_cc_ast_get(&ast_arena, r_arg1)->next = r_ptr_arg;

        ASTRef r_bad_arg_call = zero_cc_ast_alloc_node(&ast_arena, NODE_CALL);
        ZeroCCASTNode *n_bad_arg_call = zero_cc_ast_get(&ast_arena, r_bad_arg_call);
        n_bad_arg_call->left = r_fn_id;
        n_bad_arg_call->right = r_arg1;
        ty = expr_handle_expression(&ctx, n_bad_arg_call);
        assert(ty == NULL);
        assert(diags.count > diags_before);
    }

    /* -------------------------------------------------------------
     * 7. Cast Expressions
     * ------------------------------------------------------------- */
    printf("Testing cast expressions...\n");
    {
        /* Cast double to int */
        ASTRef r_d = zero_cc_ast_alloc_node(&ast_arena, NODE_FLOAT);
        zero_cc_ast_get(&ast_arena, r_d)->float_val = 9.99;

        ASTRef r_cast = zero_cc_ast_alloc_node(&ast_arena, NODE_CAST);
        ZeroCCASTNode *n_cast = zero_cc_ast_get(&ast_arena, r_cast);
        n_cast->left = r_d;
        expr_node_set_type(&ctx, n_cast, t_int); /* target type */
        TypeInfo *ty = expr_handle_expression(&ctx, n_cast);
        assert(ty != NULL);
        assert(type_equal(ty, t_int));

        /* Invalid cast: pointer to double -> error */
        size_t diags_before = diags.count;
        ASTRef r_ptr = zero_cc_ast_alloc_node(&ast_arena, NODE_IDENT);
        zero_cc_ast_get(&ast_arena, r_ptr)->s.str = "ptr";
        zero_cc_ast_get(&ast_arena, r_ptr)->s_len = 3;

        ASTRef r_bad_cast = zero_cc_ast_alloc_node(&ast_arena, NODE_CAST);
        ZeroCCASTNode *n_bad_cast = zero_cc_ast_get(&ast_arena, r_bad_cast);
        n_bad_cast->left = r_ptr;
        expr_node_set_type(&ctx, n_bad_cast, t_double);
        ty = expr_handle_expression(&ctx, n_bad_cast);
        assert(ty == NULL);
        assert(diags.count > diags_before);
    }

    /* -------------------------------------------------------------
     * 8. Member Access, Index, Ternary
     * ------------------------------------------------------------- */
    printf("Testing member access, indexing, ternary...\n");
    {
        /* Array indexing: arr[0] where arr is int[10] */
        TypeInfo *t_arr = type_array_of(perm, t_int, 10);
        scope_put_symbol(scope, "arr", SYMBOL_VAR, t_arr);

        ASTRef r_arr = zero_cc_ast_alloc_node(&ast_arena, NODE_IDENT);
        zero_cc_ast_get(&ast_arena, r_arr)->s.str = "arr";
        zero_cc_ast_get(&ast_arena, r_arr)->s_len = 3;

        ASTRef r_idx = zero_cc_ast_alloc_node(&ast_arena, NODE_INT);
        zero_cc_ast_get(&ast_arena, r_idx)->int_val = 0;

        ASTRef r_sub = zero_cc_ast_alloc_node(&ast_arena, NODE_INDEX);
        ZeroCCASTNode *n_sub = zero_cc_ast_get(&ast_arena, r_sub);
        n_sub->left = r_arr;
        n_sub->right = r_idx;
        TypeInfo *ty = expr_handle_expression(&ctx, n_sub);
        assert(ty != NULL);
        assert(type_equal(ty, t_int));

        /* Ternary: cond ? then_expr : else_expr */
        ASTRef r_cond = zero_cc_ast_alloc_node(&ast_arena, NODE_INT);
        zero_cc_ast_get(&ast_arena, r_cond)->int_val = 1;
        ASTRef r_then = zero_cc_ast_alloc_node(&ast_arena, NODE_INT);
        zero_cc_ast_get(&ast_arena, r_then)->int_val = 10;
        ASTRef r_else = zero_cc_ast_alloc_node(&ast_arena, NODE_FLOAT);
        zero_cc_ast_get(&ast_arena, r_else)->float_val = 20.0;

        ASTRef r_tern = zero_cc_ast_alloc_node(&ast_arena, NODE_TERNARY);
        ZeroCCASTNode *n_tern = zero_cc_ast_get(&ast_arena, r_tern);
        n_tern->left = r_cond;
        n_tern->right = r_then;
        n_tern->aux.alt = r_else;
        ty = expr_handle_expression(&ctx, n_tern);
        assert(ty != NULL);
        assert(type_equal(ty, t_double)); /* common arithmetic between int and double */
    }

    /* Cleanup */
    scope_leave(&scope);
    zero_cc_ast_arena_free(&ast_arena);
    arena_destroy(tmp);
    arena_destroy(perm);

    printf("All expression semantic tests PASSED successfully!\n");
    return 0;
}
