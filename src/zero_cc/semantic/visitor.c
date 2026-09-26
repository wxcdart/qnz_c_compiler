#include "visitor.h"
#include "decl.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

bool sem_context_init(SemContext *ctx, Arena *perm, Arena *tmp, DiagBuf *diags, ZeroCCASTArena *ast_arena) {
    if (!ctx || !perm || !tmp || !diags) return false;
    ctx->perm_arena = perm;
    ctx->tmp_arena = tmp;
    ctx->diags = diags;
    ctx->current_scope = NULL;
    ctx->current_fn_ret_type = NULL;
    ctx->in_loop = false;
    ctx->in_switch = false;
    ctx->ast_arena = ast_arena;

    /* Enter global scope */
    if (!scope_enter(&ctx->current_scope, ctx->perm_arena)) {
        return false;
    }

    return true;
}

void sem_context_destroy(SemContext *ctx) {
    if (!ctx) return;
    while (ctx->current_scope) {
        scope_leave(&ctx->current_scope);
    }
}

void sem_node_error(SemContext *ctx, const ZeroCCASTNode *node, const char *fmt, ...) {
    if (!ctx || !ctx->diags) return;
    uint32_t line = node ? node->line : 0;
    uint32_t col = node ? node->col : 0;

    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    diag_report(ctx->diags, line, col, "%s", buf);
}

TypeInfo *node_type(SemContext *ctx, const ZeroCCASTNode *node) {
    if (!ctx || !node || node->aux.extra == 0) return NULL;
    /* Since index 0 is used as sentinel for unset, actual arena offset is (extra - 1) */
    size_t offset = (size_t)(node->aux.extra - 1);
    return (TypeInfo *)arena_index_to_ptr(ctx->perm_arena, offset);
}

void node_set_type(SemContext *ctx, ZeroCCASTNode *node, TypeInfo *type) {
    if (!ctx || !node || !type) return;
    size_t offset = arena_index_of(ctx->perm_arena, type);
    if (offset == (size_t)-1) return;
    node->aux.extra = (uint32_t)(offset + 1);
}

ZeroCCASTNode *sem_get_node(SemContext *ctx, ASTRef ref) {
    if (!ctx || !ctx->ast_arena || ref == AST_NULL) return NULL;
    return zero_cc_ast_get(ctx->ast_arena, ref);
}

TypeInfo *sem_analyze_ref(SemContext *ctx, ASTRef ref) {
    ZeroCCASTNode *n = sem_get_node(ctx, ref);
    if (!n) return NULL;
    return sem_analyze_node(ctx, n);
}

void sem_walk_node(SemContext *ctx, ZeroCCASTNode *node) {
    (void)sem_analyze_node(ctx, node);
}

TypeInfo *sem_analyze_node(SemContext *ctx, ZeroCCASTNode *node) {
    if (!ctx || !node) return NULL;

    TypeInfo *res = NULL;

    switch (node->tag) {
        /* Literals & Primary */
        case NODE_INT: {
            res = type_int(ctx->perm_arena);
            break;
        }
        case NODE_FLOAT: {
            res = type_double(ctx->perm_arena);
            break;
        }
        case NODE_STRING: {
            TypeInfo *char_t = type_char(ctx->perm_arena);
            res = type_pointer_to(ctx->perm_arena, char_t);
            break;
        }
        case NODE_BOOL: {
            res = type_bool(ctx->perm_arena);
            break;
        }
        case NODE_NULLPTR: {
            TypeInfo *v = type_void(ctx->perm_arena);
            res = type_pointer_to(ctx->perm_arena, v);
            break;
        }
        case NODE_IDENT: {
            const char *ident_name = node->s.str;
            if (ident_name) {
                Symbol *sym = scope_lookup(ctx->current_scope, ident_name);
                if (sym) {
                    res = sym->type;
                } else {
                    sem_node_error(ctx, node, "use of undeclared identifier '%s'", ident_name);
                }
            }
            break;
        }

        /* Operators / Expressions */
        case NODE_UNOP: {
            TypeInfo *operand_type = sem_analyze_ref(ctx, node->left);
            res = operand_type;
            break;
        }
        case NODE_BINOP: {
            TypeInfo *lt = sem_analyze_ref(ctx, node->left);
            TypeInfo *rt = sem_analyze_ref(ctx, node->right);
            if (lt && rt) {
                /* If either is double, result is double */
                if (lt->kind == TYPE_DOUBLE || rt->kind == TYPE_DOUBLE) {
                    res = type_double(ctx->perm_arena);
                } else if (lt->kind == TYPE_FLOAT || rt->kind == TYPE_FLOAT) {
                    res = type_float(ctx->perm_arena);
                } else if (type_is_pointer(lt)) {
                    res = lt;
                } else if (type_is_pointer(rt) && node->aux.op == '+') {
                    res = rt;
                } else if (lt->kind == TYPE_LONG || rt->kind == TYPE_LONG) {
                    res = type_long(ctx->perm_arena);
                } else {
                    res = type_int(ctx->perm_arena);
                }
            }
            break;
        }
        case NODE_ASSIGN: {
            TypeInfo *lt = sem_analyze_ref(ctx, node->left);
            (void)sem_analyze_ref(ctx, node->right);
            res = lt;
            break;
        }
        case NODE_TERNARY: {
            (void)sem_analyze_ref(ctx, node->left);
            TypeInfo *then_t = sem_analyze_ref(ctx, node->right);
            TypeInfo *else_t = sem_analyze_ref(ctx, node->aux.alt);
            res = then_t ? then_t : else_t;
            break;
        }
        case NODE_CAST: {
            (void)sem_analyze_ref(ctx, node->left);
            /* Cast target type will be set by full semantic passes */
            res = type_int(ctx->perm_arena);
            break;
        }
        case NODE_INDEX: {
            TypeInfo *arr_t = sem_analyze_ref(ctx, node->left);
            (void)sem_analyze_ref(ctx, node->right);
            if (arr_t) {
                if (type_is_pointer(arr_t)) {
                    res = arr_t->details.pointer.base;
                } else if (type_is_array(arr_t)) {
                    res = arr_t->details.array.base;
                }
            }
            break;
        }
        case NODE_CALL: {
            TypeInfo *fn_t = sem_analyze_ref(ctx, node->left);
            /* Walk arguments */
            ASTRef arg_ref = node->right;
            while (arg_ref != AST_NULL) {
                ZeroCCASTNode *arg_node = sem_get_node(ctx, arg_ref);
                if (arg_node) {
                    (void)sem_analyze_node(ctx, arg_node);
                    arg_ref = arg_node->next;
                } else {
                    break;
                }
            }
            if (fn_t && type_is_function(fn_t)) {
                res = fn_t->details.function.ret;
            }
            break;
        }
        case NODE_COMPOUND_LIT: {
            (void)sem_analyze_ref(ctx, node->left);
            break;
        }
        case NODE_MEMBER: {
            (void)sem_analyze_ref(ctx, node->left);
            break;
        }

        /* Statements */
        case NODE_EXPR_STMT: {
            (void)sem_analyze_ref(ctx, node->left);
            break;
        }
        case NODE_BLOCK: {
            /* Create new inner scope for block */
            Scope *inner = scope_enter(&ctx->current_scope, ctx->perm_arena);
            (void)inner;

            ASTRef stmt_ref = node->left;
            while (stmt_ref != AST_NULL) {
                ZeroCCASTNode *stmt_node = sem_get_node(ctx, stmt_ref);
                if (stmt_node) {
                    (void)sem_analyze_node(ctx, stmt_node);
                    stmt_ref = stmt_node->next;
                } else {
                    break;
                }
            }

            scope_leave(&ctx->current_scope);
            break;
        }
        case NODE_IF: {
            (void)sem_analyze_ref(ctx, node->left);
            (void)sem_analyze_ref(ctx, node->right);
            if (node->aux.alt != AST_NULL) {
                (void)sem_analyze_ref(ctx, node->aux.alt);
            }
            break;
        }
        case NODE_WHILE:
        case NODE_DO_WHILE: {
            bool prev_in_loop = ctx->in_loop;
            ctx->in_loop = true;

            (void)sem_analyze_ref(ctx, node->left);
            (void)sem_analyze_ref(ctx, node->right);

            ctx->in_loop = prev_in_loop;
            break;
        }
        case NODE_FOR: {
            bool prev_in_loop = ctx->in_loop;
            ctx->in_loop = true;

            /* for-loop can introduce a scope for declarations in init */
            Scope *for_scope = scope_enter(&ctx->current_scope, ctx->perm_arena);
            (void)for_scope;

            if (node->left != AST_NULL) sem_analyze_ref(ctx, node->left);
            if (node->right != AST_NULL) sem_analyze_ref(ctx, node->right);
            if (node->aux.step != AST_NULL) sem_analyze_ref(ctx, node->aux.step);
            if (node->aux.alt != AST_NULL) sem_analyze_ref(ctx, node->aux.alt);

            scope_leave(&ctx->current_scope);
            ctx->in_loop = prev_in_loop;
            break;
        }
        case NODE_SWITCH: {
            bool prev_in_switch = ctx->in_switch;
            ctx->in_switch = true;

            (void)sem_analyze_ref(ctx, node->left);
            (void)sem_analyze_ref(ctx, node->right);

            ctx->in_switch = prev_in_switch;
            break;
        }
        case NODE_CASE: {
            if (!ctx->in_switch) {
                sem_node_error(ctx, node, "'case' statement not in switch statement");
            }
            (void)sem_analyze_ref(ctx, node->left);
            (void)sem_analyze_ref(ctx, node->right);
            break;
        }
        case NODE_DEFAULT: {
            if (!ctx->in_switch) {
                sem_node_error(ctx, node, "'default' statement not in switch statement");
            }
            (void)sem_analyze_ref(ctx, node->left);
            break;
        }
        case NODE_BREAK: {
            if (!ctx->in_loop && !ctx->in_switch) {
                sem_node_error(ctx, node, "'break' statement not in loop or switch statement");
            }
            break;
        }
        case NODE_CONTINUE: {
            if (!ctx->in_loop) {
                sem_node_error(ctx, node, "'continue' statement not in loop statement");
            }
            break;
        }
        case NODE_RETURN: {
            if (node->left != AST_NULL) {
                TypeInfo *ret_t = sem_analyze_ref(ctx, node->left);
                if (ctx->current_fn_ret_type && ret_t) {
                    /* If function returns void but return expression is present */
                    if (type_is_void(ctx->current_fn_ret_type) && !type_is_void(ret_t)) {
                        sem_node_error(ctx, node, "void function should not return a value");
                    }
                }
            } else {
                if (ctx->current_fn_ret_type && !type_is_void(ctx->current_fn_ret_type)) {
                    sem_node_error(ctx, node, "non-void function should return a value");
                }
            }
            break;
        }
        case NODE_DECL: {
            res = decl_handle_var(ctx, node);
            break;
        }
        case NODE_TYPEDEF_DECL: {
            res = decl_handle_typedef(ctx, node);
            break;
        }
        default:
            break;
    }

    if (res) {
        node_set_type(ctx, node, res);
    }

    return res;
}
