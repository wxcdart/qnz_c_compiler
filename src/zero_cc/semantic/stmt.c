#include "zero_cc/semantic/stmt.h"
#include "zero_cc/semantic/promote.h"

static bool is_scalar_type(const TypeInfo *t) {
    if (!t) return false;
    return type_is_arithmetic(t) || type_is_pointer(t);
}

bool stmt_handle_block(SemContext *ctx, ZeroCCASTNode *node) {
    if (!ctx || !node) return false;

    /* Push new inner scope for block */
    Scope *inner = scope_enter(&ctx->current_scope, ctx->perm_arena);
    if (!inner) return false;

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

    /* Pop inner scope */
    scope_leave(&ctx->current_scope);
    return true;
}

bool stmt_handle_if(SemContext *ctx, ZeroCCASTNode *node) {
    if (!ctx || !node) return false;

    /* Condition must be scalar (arithmetic or pointer, NOT struct/union/void) */
    TypeInfo *cond_t = sem_analyze_ref(ctx, node->left);
    if (cond_t) {
        if (!is_scalar_type(cond_t)) {
            sem_node_error(ctx, node, "controlling expression of if statement must have scalar type");
        }
    }

    /* Then branch */
    (void)sem_analyze_ref(ctx, node->right);

    /* Else branch (optional) */
    if (node->aux.alt != AST_NULL) {
        (void)sem_analyze_ref(ctx, node->aux.alt);
    }

    return true;
}

bool stmt_handle_while(SemContext *ctx, ZeroCCASTNode *node) {
    if (!ctx || !node) return false;

    /* Condition must be scalar */
    TypeInfo *cond_t = sem_analyze_ref(ctx, node->left);
    if (cond_t) {
        if (!is_scalar_type(cond_t)) {
            sem_node_error(ctx, node, "controlling expression of while statement must have scalar type");
        }
    }

    bool prev_in_loop = ctx->in_loop;
    ctx->in_loop = true;

    /* Body */
    (void)sem_analyze_ref(ctx, node->right);

    ctx->in_loop = prev_in_loop;
    return true;
}

bool stmt_handle_do_while(SemContext *ctx, ZeroCCASTNode *node) {
    if (!ctx || !node) return false;

    bool prev_in_loop = ctx->in_loop;
    ctx->in_loop = true;

    /* Body */
    (void)sem_analyze_ref(ctx, node->right);

    ctx->in_loop = prev_in_loop;

    /* Condition must be scalar */
    TypeInfo *cond_t = sem_analyze_ref(ctx, node->left);
    if (cond_t) {
        if (!is_scalar_type(cond_t)) {
            sem_node_error(ctx, node, "controlling expression of do-while statement must have scalar type");
        }
    }

    return true;
}

bool stmt_handle_for(SemContext *ctx, ZeroCCASTNode *node) {
    if (!ctx || !node) return false;

    bool prev_in_loop = ctx->in_loop;

    /*
     * For loops: initialize new scope if for-init declares variables or for general for-loop lexical scope.
     * C99/C11 6.8.5.3: The declaration part of a for statement defines a block scope.
     * Entering scope here ensures declarations in init are scoped to the for loop.
     */
    Scope *for_scope = scope_enter(&ctx->current_scope, ctx->perm_arena);
    if (!for_scope) return false;

    /* Init */
    if (node->left != AST_NULL) {
        (void)sem_analyze_ref(ctx, node->left);
    }

    /* Condition: if present, must be scalar */
    if (node->right != AST_NULL) {
        TypeInfo *cond_t = sem_analyze_ref(ctx, node->right);
        if (cond_t && !is_scalar_type(cond_t)) {
            sem_node_error(ctx, node, "controlling expression of for statement must have scalar type");
        }
    }

    /* Step */
    if (node->aux.step != AST_NULL) {
        (void)sem_analyze_ref(ctx, node->aux.step);
    }

    /* Body */
    ctx->in_loop = true;
    if (node->aux.alt != AST_NULL) {
        (void)sem_analyze_ref(ctx, node->aux.alt);
    }

    /* Cleanup */
    scope_leave(&ctx->current_scope);
    ctx->in_loop = prev_in_loop;

    return true;
}

bool stmt_handle_switch(SemContext *ctx, ZeroCCASTNode *node) {
    if (!ctx || !node) return false;

    /* Switch condition must be integer type */
    TypeInfo *cond_t = sem_analyze_ref(ctx, node->left);
    if (cond_t) {
        if (!type_is_integer(cond_t)) {
            sem_node_error(ctx, node, "switch condition must have integer type");
        }
    }

    bool prev_in_switch = ctx->in_switch;
    ctx->in_switch = true;

    /* Body */
    (void)sem_analyze_ref(ctx, node->right);

    ctx->in_switch = prev_in_switch;
    return true;
}

bool stmt_handle_case(SemContext *ctx, ZeroCCASTNode *node) {
    if (!ctx || !node) return false;

    if (!ctx->in_switch) {
        sem_node_error(ctx, node, "'case' statement not in switch statement");
    }

    /* Case value */
    if (node->left != AST_NULL) {
        TypeInfo *case_t = sem_analyze_ref(ctx, node->left);
        if (case_t && !type_is_integer(case_t)) {
            sem_node_error(ctx, node, "case label expression must have integer type");
        }
    }

    /* Case statement body */
    if (node->right != AST_NULL) {
        (void)sem_analyze_ref(ctx, node->right);
    }

    return true;
}

bool stmt_handle_default(SemContext *ctx, ZeroCCASTNode *node) {
    if (!ctx || !node) return false;

    if (!ctx->in_switch) {
        sem_node_error(ctx, node, "'default' statement not in switch statement");
    }

    if (node->left != AST_NULL) {
        (void)sem_analyze_ref(ctx, node->left);
    }

    return true;
}

bool stmt_handle_break(SemContext *ctx, ZeroCCASTNode *node) {
    if (!ctx || !node) return false;

    if (!ctx->in_loop && !ctx->in_switch) {
        sem_node_error(ctx, node, "'break' statement not in loop or switch statement");
        return false;
    }

    return true;
}

bool stmt_handle_continue(SemContext *ctx, ZeroCCASTNode *node) {
    if (!ctx || !node) return false;

    if (!ctx->in_loop) {
        sem_node_error(ctx, node, "'continue' statement not in loop statement");
        return false;
    }

    return true;
}

bool stmt_handle_return(SemContext *ctx, ZeroCCASTNode *node) {
    if (!ctx || !node) return false;

    if (node->left != AST_NULL) {
        /* Expression returned */
        TypeInfo *ret_t = sem_analyze_ref(ctx, node->left);
        if (ctx->current_fn_ret_type) {
            if (type_is_void(ctx->current_fn_ret_type)) {
                sem_node_error(ctx, node, "void function should not return a value");
            } else if (ret_t) {
                if (!type_compatible_assign(ctx->current_fn_ret_type, ret_t)) {
                    sem_node_error(ctx, node, "incompatible return type in return statement");
                }
            }
        }
    } else {
        /* No expression returned */
        if (ctx->current_fn_ret_type && !type_is_void(ctx->current_fn_ret_type)) {
            sem_node_error(ctx, node, "non-void function should return a value");
        }
    }

    return true;
}
