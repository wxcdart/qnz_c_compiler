#include "zero_cc/semantic/visitor.h"
#include "zero_cc/semantic/stmt.h"
#include "zero_cc/semantic/decl.h"
#include "zero_cc/semantic/expr.h"
#include "zero_cc/types/types.h"
#include "zero_cc/jit/type_info.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

bool sem_context_init(SemContext *ctx, Arena *perm, Arena *tmp, DiagBuf *diags, ZeroCCASTArena *ast_arena, struct ZeroCCCompiler *compiler) {
    if (!ctx || !perm || !tmp || !diags) return false;
    ctx->perm_arena = perm;
    ctx->tmp_arena = tmp;
    ctx->diags = diags;
    ctx->current_scope = NULL;
    ctx->current_fn_ret_type = NULL;
    ctx->in_loop = false;
    ctx->in_switch = false;
    ctx->ast_arena = ast_arena;
    ctx->compiler = compiler;
    ctx->label_count = 0;
    ctx->label_cap = 0;
    ctx->labels = NULL;

    /* Enter global scope */
    if (!scope_enter(&ctx->current_scope, ctx->perm_arena)) {
        return false;
    }

    /* Add thread-local variables to global scope */
    if (compiler) {
        for (size_t i = 0; i < compiler->thread_local_count; i++) {
            const char *name = compiler->thread_locals[i].name;
            ZeroCCType *ty = &compiler->thread_locals[i].type;
            TypeInfo *ti = zero_cc_type_to_typeinfo(perm, *ty);
            if (ti) {
                Symbol *sym = scope_put_symbol(ctx->current_scope, name, SYMBOL_VAR, ti);
                if (sym) {
                    sym->flags |= (1 << 0); /* Mark as TLS */
                }
            }
        }
    }

    return true;
}

void sem_context_destroy(SemContext *ctx) {
    if (!ctx) return;
    while (ctx->current_scope) {
        scope_leave(&ctx->current_scope);
    }
}

/* ---------------- user labels (C99 6.8.4.3) ---------------- */

void sem_reset_labels(SemContext *ctx) {
    if (ctx) ctx->label_count = 0;
}

static void sem_register_label(SemContext *ctx, const char *name, size_t len) {
    if (!ctx || !name) return;
    if (len == 0) len = strlen(name);
    if (ctx->label_count >= ctx->label_cap) {
        size_t newcap = ctx->label_cap ? ctx->label_cap * 2 : 16;
        ctx->labels = (char **)arena_alloc(ctx->tmp_arena, newcap * sizeof(char *));
        ctx->label_cap = newcap;
    }
    char *copy = (char *)arena_alloc(ctx->tmp_arena, len + 1);
    if (!copy) return;
    memcpy(copy, name, len);
    copy[len] = '\0';
    ctx->labels[ctx->label_count++] = copy;
}

bool sem_find_label(SemContext *ctx, const char *name, size_t len) {
    if (!ctx || !name) return false;
    if (len == 0) len = strlen(name);
    for (size_t i = 0; i < ctx->label_count; i++) {
        size_t l = strlen(ctx->labels[i]);
        if (l == len && memcmp(ctx->labels[i], name, len) == 0) return true;
    }
    return false;
}

static void sem_collect_labels_impl(SemContext *ctx, ASTRef ref, int depth) {
    if (!ctx || !ctx->ast_arena || ref == AST_NULL || depth > 128) return;
    ZeroCCASTNode *n = zero_cc_ast_get(ctx->ast_arena, ref);
    if (!n) return;
    if (n->tag == NODE_LABEL) {
        sem_register_label(ctx, n->s.str, n->s_len);
    }
    /* Tag-aware child edges: aux.step/aux.alt only exist on NODE_FOR
     * (step, body) and NODE_IF / NODE_TERNARY (else).  On every other
     * tag they alias s.str / int_val in the payload union — following
     * them walks garbage thin-pointers (the label/goto name length ends
     * up in aux.step). */
    if (n->left != AST_NULL) sem_collect_labels_impl(ctx, n->left, depth + 1);
    if (n->right != AST_NULL) sem_collect_labels_impl(ctx, n->right, depth + 1);
    if (n->tag == NODE_FOR) {
        if (n->aux.step != AST_NULL) sem_collect_labels_impl(ctx, n->aux.step, depth + 1);
        if (n->aux.alt != AST_NULL) sem_collect_labels_impl(ctx, n->aux.alt, depth + 1);
    } else if (n->tag == NODE_IF || n->tag == NODE_TERNARY) {
        if (n->aux.alt != AST_NULL) sem_collect_labels_impl(ctx, n->aux.alt, depth + 1);
    }
    if (n->next != AST_NULL) sem_collect_labels_impl(ctx, n->next, depth + 1);
}

void sem_collect_labels(SemContext *ctx, ASTRef root) {
    if (ctx) sem_reset_labels(ctx);
    sem_collect_labels_impl(ctx, root, 0);
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
            /* C99 6.4.4.1: the literal's type follows its suffix/value
             * (0x12345678ABCDEF0LLU is unsigned long long, not int) — this
             * drives usual-arithmetic-conversion widths for the whole
             * enclosing expression. */
            res = type_int_literal(ctx->perm_arena,
                                   (uint8_t)(node->flags & 7u), node->int_val);
            break;
        }
        case NODE_FLOAT: {
            if (node->aux.extra == 1) {
                res = type_float(ctx->perm_arena);
            } else {
                res = type_double(ctx->perm_arena);
            }
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
                           strncmp(ident_name, "__atomic_", 9) == 0 ||
                           strncmp(ident_name, "__zerocc_", 9) == 0) {
                    res = type_function(ctx->perm_arena, type_int(ctx->perm_arena), NULL, 0, true);
                } else {
                    /* "did you mean" against every visible symbol in the
                     * scope chain (vars, functions, typedefs, tags). */
                    const char *names[1024];
                    size_t n = scope_chain_names(ctx->current_scope, names, 1024);
                    const char *sugg = diag_best_match(ident_name, strlen(ident_name), names, n);
                    if (sugg) {
                        sem_node_error(ctx, node,
                            "use of undeclared identifier '%s'; did you mean '%s'?",
                            ident_name, sugg);
                    } else {
                        sem_node_error(ctx, node, "use of undeclared identifier '%s'", ident_name);
                    }
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
                    /* Pointer-pointer operations (except comparisons which are handled above).
                     * C11 6.5.6p9: pointer - pointer yields ptrdiff_t (long), not a pointer. */
                    if (op == '-' || op == TK_MINUS) {
                        res = type_long(ctx->perm_arena);
                    } else {
                        res = lt;
                    }
                } else if (type_is_pointer(lt)) {
                    res = lt;
                } else if (type_is_pointer(rt) && (op == '+' || op == TK_PLUS)) {
                    res = rt;
                } else if (op == TK_LOGAND || op == TK_LOGOR) {
                    res = type_int(ctx->perm_arena);
                } else if (op == TK_SHL || op == TK_SHR) {
                    res = type_promote(ctx->perm_arena, lt);
                } else {
                    res = type_common_arithmetic_type(ctx->perm_arena, lt, rt);
                    if (!res) {
                        if (lt->kind == TYPE_KIND_LONG || rt->kind == TYPE_KIND_LONG) {
                            res = type_long(ctx->perm_arena);
                        } else {
                            res = type_int(ctx->perm_arena);
                        }
                    }
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
            size_t arg_count = 0;
            ASTRef arg_ref = node->right;
            while (arg_ref != AST_NULL) {
                ZeroCCASTNode *arg_node = sem_get_node(ctx, arg_ref);
                if (arg_node) {
                    (void)sem_analyze_node(ctx, arg_node);
                    arg_count++;
                    arg_ref = arg_node->next;
                } else {
                    break;
                }
            }
            TypeInfo *proto_t = NULL;
            if (fn_t && type_is_function(fn_t)) {
                proto_t = fn_t;
                res = fn_t->details.function.ret;
            } else if (fn_t && type_is_pointer(fn_t) && fn_t->details.pointer.base &&
                       type_is_function(fn_t->details.pointer.base)) {
                proto_t = fn_t->details.pointer.base;
                res = proto_t->details.function.ret;
            }
            /* C99 6.7.5.3p6: when the callee has a prototype, the argument
             * count must match it (variadic: at least the named parameters).
             * No-prototype function types (empty-paren decls, implicit
             * declarations) place no constraint. */
            if (proto_t && proto_t->details.function.is_prototype) {
                size_t param_cnt = proto_t->details.function.param_cnt;
                ZeroCCASTNode *callee_node = sem_get_node(ctx, node->left);
                const char *callee_name = (callee_node && callee_node->tag == NODE_IDENT &&
                                           callee_node->s.str) ? callee_node->s.str : NULL;
                if (proto_t->details.function.is_variadic) {
                    if (arg_count < param_cnt) {
                        if (callee_name)
                            sem_node_error(ctx, node,
                                "too few arguments to call '%s': prototype requires at least %zu, %zu given",
                                callee_name, param_cnt, arg_count);
                        else
                            sem_node_error(ctx, node,
                                "too few arguments to call: prototype requires at least %zu, %zu given",
                                param_cnt, arg_count);
                    }
                } else if (arg_count != param_cnt) {
                    if (callee_name)
                        sem_node_error(ctx, node,
                            "argument count mismatch in call to '%s': prototype declares %zu parameter(s), %zu argument(s) given",
                            callee_name, param_cnt, arg_count);
                    else
                        sem_node_error(ctx, node,
                            "argument count mismatch in call: prototype declares %zu parameter(s), %zu argument(s) given",
                            param_cnt, arg_count);
                }
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
        case NODE_STMT_EXPR: {
            ZeroCCASTNode *block = sem_get_node(ctx, node->left);
            if (block) {
                stmt_handle_block(ctx, block);
            }
            TypeInfo *last_t = NULL;
            if (block) {
                ASTRef cur = block->left;
                while (cur != AST_NULL) {
                    ZeroCCASTNode *st = sem_get_node(ctx, cur);
                    if (!st) break;
                    if (st->next == AST_NULL) {
                        if (st->tag == NODE_EXPR_STMT && st->left != AST_NULL) {
                            last_t = expr_node_type(ctx, sem_get_node(ctx, st->left));
                        }
                    }
                    cur = st->next;
                }
            }
            res = last_t ? last_t : type_void(ctx->perm_arena);
            expr_node_set_type(ctx, node, res);
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
        case NODE_LABEL: {
            /* Register and keep traversing: the labeled statement must be
             * analyzed too (it was previously skipped entirely). */
            sem_register_label(ctx, node->s.str, node->s_len);
            if (node->left != AST_NULL) {
                (void)sem_analyze_ref(ctx, node->left);
            }
            break;
        }
        case NODE_GOTO: {
            /* C99 6.8.4.3p1: the label must be defined in the enclosing
             * function (labels are pre-collected over the whole body, so
             * forward gotos are validated). */
            if (node->s.str && !sem_find_label(ctx, node->s.str, node->s_len)) {
                sem_node_error(ctx, node, "label '%s' used but not defined", node->s.str);
            }
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
