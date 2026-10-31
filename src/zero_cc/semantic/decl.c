#include "zero_cc/semantic/decl.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

/* Forward declaration for expression handler when analyzing initializers */
TypeInfo *expr_handle_expression(SemContext *ctx, ZeroCCASTNode *node);
TypeInfo *sem_analyze_node(SemContext *ctx, ZeroCCASTNode *node);

/* Helper to report an error at an AST node location */
static void decl_error(SemContext *ctx, const ZeroCCASTNode *node, const char *fmt, ...) {
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

static ZeroCCASTNode *decl_get_node(SemContext *ctx, ASTRef ref) {
    if (!ctx || !ctx->ast_arena || ref == AST_NULL) return NULL;
    return zero_cc_ast_get(ctx->ast_arena, ref);
}

TypeInfo *decl_node_type(SemContext *ctx, const ZeroCCASTNode *node) {
    if (!ctx || !node || node->aux.extra == 0) return NULL;
    size_t offset = (size_t)(node->aux.extra - 1);
    return (TypeInfo *)arena_index_to_ptr(ctx->perm_arena, offset);
}

void decl_node_set_type(SemContext *ctx, ZeroCCASTNode *node, TypeInfo *type) {
    if (!ctx || !node || !type) return;
    size_t offset = arena_index_of(ctx->perm_arena, type);
    if (offset == (size_t)-1) return;
    node->aux.extra = (uint32_t)(offset + 1);
}

/* Helper to evaluate an expression or subnode */
static TypeInfo *decl_eval_node(SemContext *ctx, ZeroCCASTNode *node) {
    if (!ctx || !node) return NULL;
    /* Multi-error recovery: fall back to the visitor only when the expr
     * mechanism gave up WITHOUT reporting — if it already reported an error,
     * re-analyzing the same node through the visitor would double-report the
     * same fault (e.g. arity errors from both expr_handle_call and the
     * visitor's NODE_CALL check). */
    size_t diags_before = ctx->diags ? ctx->diags->count : 0;
    TypeInfo *ty = expr_handle_expression(ctx, node);
    if (!ty && ctx->diags && ctx->diags->count == diags_before) {
        ty = sem_analyze_node(ctx, node);
    }
    return ty;
}

TypeInfo *decl_handle_var(SemContext *ctx, ZeroCCASTNode *node) {
    if (!ctx || !node) return NULL;

    const char *name = node->s.str;
    if (!name || strlen(name) == 0) {
        decl_error(ctx, node, "variable declaration missing identifier name");
        return NULL;
    }

    /* Check for duplicate symbol in the current scope */
    Symbol *existing = scope_lookup_current(ctx->current_scope, name);
    if (existing) {
        decl_error(ctx, node, "redefinition of '%s'", name);
        return NULL;
    }

    /* Retrieve declared type (if annotated on node) */
    TypeInfo *var_t = decl_node_type(ctx, node);

    /* C99 6.7.9p11: the identifier is in scope within its own
     * initializer (`double b = b;` compiles; reading b is UB but the
     * name must resolve to the new object). Register the symbol before
     * evaluating the initializer when the declared type is known;
     * remove it again if the declaration is then rejected. */
    Symbol *sym = NULL;
    if (var_t && !type_is_void(var_t)) {
        sym = scope_put_symbol(ctx->current_scope, name, SYMBOL_VAR, var_t);
        if (!sym) {
            decl_error(ctx, node, "failed to insert symbol '%s' into scope", name);
            return NULL;
        }
        sym->node = node;
    }

    /* If initializer expression exists (in left branch) */
    TypeInfo *init_t = NULL;
    ZeroCCASTNode *init_node = NULL;
    if (node->left != AST_NULL) {
        init_node = decl_get_node(ctx, node->left);
        if (init_node) {
            init_t = decl_eval_node(ctx, init_node);
        }
    }

    /* If VLA dimension expression exists (in right branch) */
    if ((node->flags & NODE_DECL_IS_VLA) && node->right != AST_NULL) {
        ZeroCCASTNode *dim_node = decl_get_node(ctx, node->right);
        if (dim_node) {
            (void)decl_eval_node(ctx, dim_node);
        }
    }

    /* If no explicit type was specified, infer from initializer or default to int */
    if (!var_t) {
        var_t = init_t ? init_t : type_int(ctx->perm_arena);
    }

    /* Disallow void variables */
    if (type_is_void(var_t)) {
        if (sym) scope_remove_symbol(ctx->current_scope, name);
        decl_error(ctx, node, "variable '%s' declared with void type", name);
        return NULL;
    }

    /* Validate initializer compatibility if initializer exists */
    if (init_t) {
        /* C11 6.7.9p13: a string literal may initialize a char array
         * (char / unsigned char).  The literal is typed char* in the
         * semantic model, so the generic assign check would reject it. */
        bool str_lit_into_char_arr =
            (init_node && init_node->tag == NODE_STRING &&
             var_t && var_t->kind == TYPE_KIND_ARRAY &&
             var_t->details.array.base &&
             var_t->details.array.base->kind == TYPE_KIND_CHAR);
        if (!str_lit_into_char_arr && !type_compatible_assign(var_t, init_t)) {
            if (sym) scope_remove_symbol(ctx->current_scope, name);
            decl_error(ctx, node, "incompatible initializer type for variable '%s'", name);
            return NULL;
        }
    }

    /* Register symbol in current scope (already done above when the
     * declared type was known) */
    if (!sym) {
        sym = scope_put_symbol(ctx->current_scope, name, SYMBOL_VAR, var_t);
        if (!sym) {
            decl_error(ctx, node, "failed to insert symbol '%s' into scope", name);
            return NULL;
        }
    }
    sym->node = node;

    decl_node_set_type(ctx, node, var_t);
    return var_t;
}

TypeInfo *decl_handle_typedef(SemContext *ctx, ZeroCCASTNode *node) {
    if (!ctx || !node) return NULL;

    const char *name = node->s.str;
    if (!name || strlen(name) == 0) {
        decl_error(ctx, node, "typedef declaration missing identifier name");
        return NULL;
    }

    /* Check for duplicate in current scope */
    Symbol *existing = scope_lookup_current(ctx->current_scope, name);
    if (existing) {
        decl_error(ctx, node, "redefinition of typedef '%s'", name);
        return NULL;
    }

    /* Target type from node or inferred from right/left child or default to int */
    TypeInfo *target_t = decl_node_type(ctx, node);
    if (!target_t) {
        if (node->left != AST_NULL) {
            ZeroCCASTNode *target_node = decl_get_node(ctx, node->left);
            if (target_node) {
                target_t = decl_node_type(ctx, target_node);
                if (!target_t) target_t = decl_eval_node(ctx, target_node);
            }
        }
    }
    if (!target_t) {
        target_t = type_int(ctx->perm_arena);
    }

    Symbol *sym = scope_put_symbol(ctx->current_scope, name, SYMBOL_TYPEDEF, target_t);
    if (!sym) {
        decl_error(ctx, node, "failed to register typedef '%s'", name);
        return NULL;
    }
    sym->node = node;

    decl_node_set_type(ctx, node, target_t);
    return target_t;
}

TypeInfo *decl_handle_function_proto(SemContext *ctx, ZeroCCASTNode *node) {
    if (!ctx || !node) return NULL;

    const char *fn_name = node->s.str;
    if (!fn_name || strlen(fn_name) == 0) {
        decl_error(ctx, node, "function declaration missing function name");
        return NULL;
    }

    /* Get function type annotated on node */
    TypeInfo *fn_t = decl_node_type(ctx, node);
    if (!fn_t) {
        /* Default to function returning int */
        TypeInfo *ret_t = type_int(ctx->perm_arena);
        fn_t = type_function(ctx->perm_arena, ret_t, NULL, 0, false);
    }

    if (!type_is_function(fn_t)) {
        decl_error(ctx, node, "symbol '%s' declared with non-function type", fn_name);
        return NULL;
    }

    /* Check for previous declaration in scope */
    Symbol *prev = scope_lookup_current(ctx->current_scope, fn_name);
    if (prev) {
        if (prev->kind != SYMBOL_FUNC) {
            decl_error(ctx, node, "redefinition of '%s' as different kind of symbol", fn_name);
            return NULL;
        }
        /* Check prototype compatibility */
        if (!type_is_compatible(prev->type, fn_t)) {
            decl_error(ctx, node, "conflicting types for function '%s'", fn_name);
            return NULL;
        }
    } else {
        /* Insert into current scope */
        prev = scope_put_symbol(ctx->current_scope, fn_name, SYMBOL_FUNC, fn_t);
        if (!prev) {
            decl_error(ctx, node, "failed to register function '%s'", fn_name);
            return NULL;
        }
        prev->node = node;
    }

    decl_node_set_type(ctx, node, fn_t);

    /* If this node contains a function body (definition) */
    /* By convention, right child or aux.alt can hold the body statement/block */
    ASTRef body_ref = (node->right != AST_NULL) ? node->right : (ASTRef)node->aux.alt;
    if (body_ref != AST_NULL) {
        ZeroCCASTNode *body_node = decl_get_node(ctx, body_ref);
        if (body_node) {
            /* 1. Save and update current function return type */
            TypeInfo *prev_ret_t = ctx->current_fn_ret_type;
            ctx->current_fn_ret_type = fn_t->details.function.ret;

            /* 2. Push new function parameter scope */
            Scope *param_scope = scope_enter(&ctx->current_scope, ctx->perm_arena);
            (void)param_scope;

            /* 3. Insert parameters into scope if param declarations exist (in node->left chain) */
            ASTRef param_ref = node->left;
            size_t p_idx = 0;
            while (param_ref != AST_NULL) {
                ZeroCCASTNode *param_node = decl_get_node(ctx, param_ref);
                if (!param_node) break;

                const char *pname = param_node->s.str;
                TypeInfo *ptype = decl_node_type(ctx, param_node);
                if (!ptype && p_idx < fn_t->details.function.param_cnt) {
                    ptype = fn_t->details.function.params[p_idx];
                }
                if (!ptype) {
                    ptype = type_int(ctx->perm_arena);
                }

                if (pname && strlen(pname) > 0) {
                    if (scope_lookup_current(ctx->current_scope, pname)) {
                        decl_error(ctx, param_node, "redefinition of parameter '%s'", pname);
                    } else {
                        scope_put_symbol(ctx->current_scope, pname, SYMBOL_VAR, ptype);
                    }
                }

                p_idx++;
                param_ref = param_node->next;
            }

            /* 4. Process function body */
            if (body_node->tag == NODE_BLOCK) {
                /* Walk statements in block without double-entering scope */
                ASTRef stmt_ref = body_node->left;
                while (stmt_ref != AST_NULL) {
                    ZeroCCASTNode *stmt_node = decl_get_node(ctx, stmt_ref);
                    if (stmt_node) {
                        (void)sem_analyze_node(ctx, stmt_node);
                        stmt_ref = stmt_node->next;
                    } else {
                        break;
                    }
                }
            } else {
                (void)sem_analyze_node(ctx, body_node);
            }

            /* 5. Pop parameter scope and restore return type */
            scope_leave(&ctx->current_scope);
            ctx->current_fn_ret_type = prev_ret_t;
        }
    }

    return fn_t;
}

TypeInfo *decl_handle_tag(SemContext *ctx, const char *tag_name, TypeKind kind, size_t size, size_t align) {
    if (!ctx || !tag_name) return NULL;

    TypeInfo *tag_t = type_new(ctx->perm_arena);
    if (!tag_t) return NULL;

    tag_t->kind = kind;
    tag_t->modifiers = TYPE_MOD_NONE;
    tag_t->details.record.tag = tag_name;
    tag_t->details.record.size = size;
    tag_t->details.record.align = align ? align : 1;

    Symbol *existing = scope_lookup_current(ctx->current_scope, tag_name);
    if (existing && existing->kind == SYMBOL_TAG) {
        /* Already defined tag in same scope */
        decl_error(ctx, NULL, "redefinition of struct/union/enum tag '%s'", tag_name);
        return NULL;
    }

    Symbol *sym = scope_put_symbol(ctx->current_scope, tag_name, SYMBOL_TAG, tag_t);
    if (!sym) {
        decl_error(ctx, NULL, "failed to register tag '%s'", tag_name);
        return NULL;
    }

    return tag_t;
}

TypeInfo *decl_handle_declaration(SemContext *ctx, ZeroCCASTNode *node) {
    if (!ctx || !node) return NULL;

    switch (node->tag) {
        case NODE_DECL:
            return decl_handle_var(ctx, node);

        case NODE_TYPEDEF_DECL:
            return decl_handle_typedef(ctx, node);

        default:
            /* If it's a function declaration or custom node tag */
            return decl_handle_var(ctx, node);
    }
}
