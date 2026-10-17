#include "visitor.h"
#include "stmt.h"
#include "decl.h"
#include "expr.h"
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
                } else if (strcmp(ident_name, "__func__") == 0) {
                    /* __func__ is implicitly declared as char[] (pointer to function name) */
                    res = type_pointer_to(ctx->perm_arena, type_char(ctx->perm_arena));
                } else if (strncmp(ident_name, "__builtin_", 10) == 0 ||
                           strncmp(ident_name, "__sync_", 7) == 0 ||
                           strncmp(ident_name, "__atomic_", 9) == 0) {
                    res = type_function(ctx->perm_arena, type_int(ctx->perm_arena), NULL, 0, true);
                } else {
                    sem_node_error(ctx, node, "use of undeclared identifier '%s'", ident_name);
                }
            }
            break;
        }

        /* Operators / Expressions */
        case NODE_UNOP: {
            TypeInfo *operand_type = sem_analyze_ref(ctx, node->left);
            if (operand_type) {
                uint32_t op = node->aux.op;
                if (op == '&' || op == TK_AMP) {
                    res = type_pointer_to(ctx->perm_arena, operand_type);
                } else if (op == '*' || op == TK_STAR) {
                    if (type_is_pointer(operand_type)) {
                        res = operand_type->details.pointer.base;
                    } else if (type_is_array(operand_type)) {
                        res = operand_type->details.array.base;
                    } else {
                        res = operand_type;
                    }
                } else if (op == '!' || op == TK_BANG) {
                    res = type_int(ctx->perm_arena);
                } else if (op == TK_INC || op == TK_DEC) {
                    if ((operand_type->modifiers & TYPE_MOD_CONST) != 0) {
                        sem_node_error(ctx, node, "cannot modify const-qualified operand");
                    }
                    res = operand_type;
                } else {
                    res = operand_type;
                }
            }
            break;
        }
        case NODE_BINOP: {
            TypeInfo *lt = sem_analyze_ref(ctx, node->left);
            TypeInfo *rt = sem_analyze_ref(ctx, node->right);
            if (lt && rt) {
                uint32_t op = node->aux.op;
                /* Comparison operators (<, <=, >, >=, ==, !=) always return int */
                bool is_comparison = (op == '<' || op == '>' || op == TK_LT || op == TK_GT ||
                                      op == TK_LE || op == TK_GE || op == TK_EQ || op == TK_NE);
                if (is_comparison) {
                    res = type_int(ctx->perm_arena);
                } else if (lt->kind == TYPE_KIND_DOUBLE || rt->kind == TYPE_KIND_DOUBLE) {
                    res = type_double(ctx->perm_arena);
                } else if (lt->kind == TYPE_KIND_FLOAT || rt->kind == TYPE_KIND_FLOAT) {
                    res = type_float(ctx->perm_arena);
                } else if (type_is_pointer(lt) && type_is_pointer(rt)) {
                    /* Pointer-pointer operations (except comparisons which are handled above) */
                    res = lt;
                } else if (type_is_pointer(lt)) {
                    res = lt;
                } else if (type_is_pointer(rt) && op == '+') {
                    res = rt;
                } else if (lt->kind == TYPE_KIND_LONG || rt->kind == TYPE_KIND_LONG) {
                    res = type_long(ctx->perm_arena);
                } else {
                    res = type_int(ctx->perm_arena);
                }
            }
            break;
        }
        case NODE_ASSIGN: {
            TypeInfo *lt = sem_analyze_ref(ctx, node->left);
            TypeInfo *rt = sem_analyze_ref(ctx, node->right);
            if (lt && rt) {
                if ((lt->modifiers & TYPE_MOD_CONST) != 0) {
                    sem_node_error(ctx, node, "cannot assign to const-qualified variable");
                }
                if (!type_compatible_assign(lt, rt)) {
                    sem_node_error(ctx, node, "incompatible types in assignment");
                }
            }
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
        case NODE_COMMA: {
            (void)sem_analyze_ref(ctx, node->left);
            res = sem_analyze_ref(ctx, node->right);
            break;
        }
        case NODE_CAST: {
            (void)sem_analyze_ref(ctx, node->left);
            /* Preserve cast target type annotated by parser or default to int */
            TypeInfo *dst_t = expr_node_type(ctx, node);
            res = dst_t ? dst_t : type_int(ctx->perm_arena);
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
            } else if (fn_t && type_is_pointer(fn_t) && fn_t->details.pointer.base &&
                       type_is_function(fn_t->details.pointer.base)) {
                res = fn_t->details.pointer.base->details.function.ret;
            }
            expr_node_set_type(ctx, node, res);
            break;
        }
        case NODE_COMPOUND_LIT: {
            ASTRef cur_ref = node->left;
            while (cur_ref != AST_NULL) {
                (void)sem_analyze_ref(ctx, cur_ref);
                ZeroCCASTNode *elem = zero_cc_ast_get(ctx->ast_arena, cur_ref);
                cur_ref = elem ? elem->next : AST_NULL;
            }
            res = expr_node_type(ctx, node);
            break;
        }
        case NODE_MEMBER: {
            res = expr_handle_member(ctx, node);
            break;
        }
        case NODE_VA_START: {
            res = expr_handle_va_start(ctx, node);
            break;
        }
        case NODE_VA_ARG: {
            res = expr_handle_va_arg(ctx, node);
            break;
        }

        /* Statements */
        case NODE_EXPR_STMT: {
            (void)sem_analyze_ref(ctx, node->left);
            break;
        }
        case NODE_BLOCK: {
            stmt_handle_block(ctx, node);
            break;
        }
        case NODE_IF: {
            stmt_handle_if(ctx, node);
            break;
        }
        case NODE_WHILE: {
            stmt_handle_while(ctx, node);
            break;
        }
        case NODE_DO_WHILE: {
            stmt_handle_do_while(ctx, node);
            break;
        }
        case NODE_FOR: {
            stmt_handle_for(ctx, node);
            break;
        }
        case NODE_SWITCH: {
            stmt_handle_switch(ctx, node);
            break;
        }
        case NODE_CASE: {
            stmt_handle_case(ctx, node);
            break;
        }
        case NODE_DEFAULT: {
            stmt_handle_default(ctx, node);
            break;
        }
        case NODE_BREAK: {
            stmt_handle_break(ctx, node);
            break;
        }
        case NODE_CONTINUE: {
            stmt_handle_continue(ctx, node);
            break;
        }
        case NODE_RETURN: {
            stmt_handle_return(ctx, node);
            break;
        }
        case NODE_DECL: {
            /* Check if this is a function definition: has body (right != AST_NULL) and not a VLA */
            if (!(node->flags & NODE_DECL_IS_VLA) && node->right != AST_NULL) {
                /* Function definition */
                res = decl_handle_function_proto(ctx, node);
            } else if (node->flags & NODE_FLAG_FUNC_PROTO) {
                /* Function prototype (no body, has param list) */
                res = decl_handle_function_proto(ctx, node);
            } else {
                /* Variable declaration */
                res = decl_handle_var(ctx, node);
            }
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
