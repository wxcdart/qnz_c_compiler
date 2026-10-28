#include "zero_cc/semantic/expr.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "zero_cc/semantic/stmt.h"
#include "zero_cc/types/token.h"

/* Token type constants for binary operators */
#define TOK_SHL         TK_SHL
#define TOK_SHR         TK_SHR

/* Helper to report an error at an AST node location */
static void expr_error(SemContext *ctx, const ZeroCCASTNode *node, const char *fmt, ...) {
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

static ZeroCCASTNode *expr_get_node(SemContext *ctx, ASTRef ref) {
    if (!ctx || !ctx->ast_arena || ref == AST_NULL) return NULL;
    return zero_cc_ast_get(ctx->ast_arena, ref);
}

TypeInfo *expr_node_type(SemContext *ctx, const ZeroCCASTNode *node) {
    if (!ctx || !node || node->aux.extra == 0) return NULL;
    size_t offset = (size_t)(node->aux.extra - 1);
    return (TypeInfo *)arena_index_to_ptr(ctx->perm_arena, offset);
}

void expr_node_set_type(SemContext *ctx, ZeroCCASTNode *node, TypeInfo *type) {
    if (!ctx || !node || !type) return;
    size_t offset = arena_index_of(ctx->perm_arena, type);
    if (offset == (size_t)-1) return;
    node->aux.extra = (uint32_t)(offset + 1);
}

/* Helper to check if an expression is an lvalue */
static bool expr_is_lvalue(SemContext *ctx, const ZeroCCASTNode *node) {
    if (!node) return false;
    switch (node->tag) {
        case NODE_IDENT:
            return true;
        case NODE_MEMBER:
            return true;
        case NODE_INDEX:
            return true;
        case NODE_COMPOUND_LIT:
            return true;
        case NODE_UNOP:
            /* Dereference *p is an lvalue */
            if (node->aux.op == '*' || node->aux.op == EXPR_TK_STAR) {
                return true;
            }
            return false;
        default:
            (void)ctx;
            return false;
    }
}

TypeInfo *expr_handle_literal(SemContext *ctx, ZeroCCASTNode *node) {
    if (!ctx || !node) return NULL;
    TypeInfo *res = NULL;

    switch (node->tag) {
        case NODE_INT:
            res = type_int(ctx->perm_arena);
            break;
        case NODE_FLOAT:
            res = type_double(ctx->perm_arena);
            break;
        case NODE_STRING: {
            TypeInfo *char_t = type_char(ctx->perm_arena);
            res = type_pointer_to(ctx->perm_arena, char_t);
            break;
        }
        case NODE_BOOL:
            res = type_bool(ctx->perm_arena);
            break;
        case NODE_NULLPTR: {
            TypeInfo *v = type_void(ctx->perm_arena);
            res = type_pointer_to(ctx->perm_arena, v);
            break;
        }
        default:
            expr_error(ctx, node, "unknown literal node tag %u", node->tag);
            return NULL;
    }

    if (res) {
        expr_node_set_type(ctx, node, res);
    }
    return res;
}

TypeInfo *expr_handle_identifier(SemContext *ctx, ZeroCCASTNode *node) {
    if (!ctx || !node) return NULL;

    const char *name = node->s.str;
    if (!name) {
        expr_error(ctx, node, "identifier node missing symbol name");
        return NULL;
    }

    Symbol *sym = scope_lookup(ctx->current_scope, name);
    if (!sym) {
        if (strcmp(name, "__func__") == 0) {
            /* __func__ is implicitly declared as static const char[] */
            TypeInfo *res = type_pointer_to(ctx->perm_arena, type_char(ctx->perm_arena));
            expr_node_set_type(ctx, node, res);
            return res;
        }
        if (strncmp(name, "__builtin_", 10) == 0 ||
            strncmp(name, "__sync_", 7) == 0 ||
            strncmp(name, "__atomic_", 9) == 0 ||
            strncmp(name, "__zerocc_", 9) == 0) {
            TypeInfo *res = type_function(ctx->perm_arena, type_int(ctx->perm_arena), NULL, 0, true);
            expr_node_set_type(ctx, node, res);
            return res;
        }
        /* "did you mean" against every visible symbol in the scope chain
         * (same policy as the visitor's NODE_IDENT check). */
        const char *names[1024];
        size_t n = scope_chain_names(ctx->current_scope, names, 1024);
        const char *sugg = diag_best_match(name, strlen(name), names, n);
        if (sugg) {
            expr_error(ctx, node,
                       "use of undeclared identifier '%s'; did you mean '%s'?",
                       name, sugg);
        } else {
            expr_error(ctx, node, "use of undeclared identifier '%s'", name);
        }
        return NULL;
    }

    TypeInfo *res = sym->type;
    if (res) {
        expr_node_set_type(ctx, node, res);
    }
    return res;
}

TypeInfo *expr_handle_unary(SemContext *ctx, ZeroCCASTNode *node) {
    if (!ctx || !node) return NULL;

    ZeroCCASTNode *operand = expr_get_node(ctx, node->left);
    if (!operand) {
        expr_error(ctx, node, "unary operator missing operand");
        return NULL;
    }

    TypeInfo *op_type = expr_handle_expression(ctx, operand);
    if (!op_type) {
        return NULL;
    }

    uint32_t op = node->aux.op;
    TypeInfo *res = NULL;

    switch (op) {
        case '&':
        case TK_AMP: {
            /* Address-of operator (&): operand must be an lvalue or function */
            if (!expr_is_lvalue(ctx, operand) && !type_is_function(op_type)) {
                expr_error(ctx, node, "cannot take the address of an rvalue");
                return NULL;
            }
            res = type_pointer_to(ctx->perm_arena, op_type);
            break;
        }
        case '*':
        case TK_STAR: {
            /* Dereference operator (*): operand must be a pointer or array */
            if (type_is_pointer(op_type)) {
                res = op_type->details.pointer.base;
            } else if (type_is_array(op_type)) {
                res = op_type->details.array.base;
            } else {
                expr_error(ctx, node, "indirection requires pointer operand");
                return NULL;
            }
            break;
        }
        case '+':
        case TK_PLUS:
        case '-':
        case TK_MINUS: {
            /* Unary plus and minus: operand must be arithmetic */
            if (!type_is_arithmetic(op_type)) {
                expr_error(ctx, node, "unary arithmetic operator requires arithmetic operand");
                return NULL;
            }
            res = type_promote(ctx->perm_arena, op_type);
            break;
        }
        case '!':
        case TK_BANG: {
            /* Logical NOT: operand must be scalar (arithmetic or pointer) */
            if (!type_is_arithmetic(op_type) && !type_is_pointer(op_type)) {
                expr_error(ctx, node, "logical NOT requires scalar operand");
                return NULL;
            }
            res = type_int(ctx->perm_arena);
            break;
        }
        case '~':
        case TK_TILDE: {
            /* Bitwise NOT: operand must be integer */
            if (!type_is_integer(op_type)) {
                expr_error(ctx, node, "bitwise NOT requires integer operand");
                return NULL;
            }
            res = type_promote(ctx->perm_arena, op_type);
            break;
        }
        case TK_INC:
        case TK_DEC: {
            /* Increment and decrement (prefix and postfix): operand must be modifiable lvalue */
            if (!expr_is_lvalue(ctx, operand)) {
                expr_error(ctx, node, "expression is not assignable");
                return NULL;
            }
            if ((op_type->modifiers & TYPE_MOD_CONST) != 0) {
                expr_error(ctx, node, "cannot modify const-qualified operand");
                return NULL;
            }
            if (!type_is_arithmetic(op_type) && !type_is_pointer(op_type)) {
                expr_error(ctx, node, "increment/decrement operand must be arithmetic or pointer");
                return NULL;
            }
            res = op_type;
            break;
        }
        default:
            expr_error(ctx, node, "unsupported unary operator %u", op);
            return NULL;
    }

    if (res) {
        expr_node_set_type(ctx, node, res);
    }
    return res;
}

TypeInfo *expr_handle_binary(SemContext *ctx, ZeroCCASTNode *node) {
    if (!ctx || !node) return NULL;

    ZeroCCASTNode *lhs = expr_get_node(ctx, node->left);
    ZeroCCASTNode *rhs = expr_get_node(ctx, node->right);
    if (!lhs || !rhs) {
        expr_error(ctx, node, "binary operator missing operand");
        return NULL;
    }

    TypeInfo *lt = expr_handle_expression(ctx, lhs);
    TypeInfo *rt = expr_handle_expression(ctx, rhs);
    if (!lt || !rt) {
        return NULL;
    }

    uint32_t op = node->aux.op;
    TypeInfo *res = promote_binary(op, lt, rt, ctx->perm_arena, ctx->diags);
    if (!res) {
        expr_error(ctx, node, "invalid operands to binary operator %u", op);
        return NULL;
    }

    expr_node_set_type(ctx, node, res);
    return res;
}

TypeInfo *expr_handle_assignment(SemContext *ctx, ZeroCCASTNode *node) {
    if (!ctx || !node) return NULL;

    ZeroCCASTNode *lhs = expr_get_node(ctx, node->left);
    ZeroCCASTNode *rhs = expr_get_node(ctx, node->right);
    if (!lhs || !rhs) {
        expr_error(ctx, node, "assignment operator missing operand");
        return NULL;
    }

    TypeInfo *lt = expr_handle_expression(ctx, lhs);
    TypeInfo *rt = expr_handle_expression(ctx, rhs);
    if (!lt || !rt) {
        return NULL;
    }

    /* Left operand must be an lvalue */
    if (!expr_is_lvalue(ctx, lhs)) {
        expr_error(ctx, node, "expression is not assignable");
        return NULL;
    }

    /* Left operand must not be const */
    if ((lt->modifiers & TYPE_MOD_CONST) != 0) {
        expr_error(ctx, node, "cannot assign to const-qualified variable");
        return NULL;
    }

    uint32_t op = node->aux.op;

    if (op == EXPR_TK_ASSIGN || op == '=') {
        /* Simple assignment = */
        if (!type_compatible_assign(lt, rt)) {
            expr_error(ctx, node, "incompatible types in assignment");
            return NULL;
        }
    } else {
        /* Compound assignment: +=, -=, *=, etc. */
        uint32_t underlying_op = 0;
        switch (op) {
            case TK_ADD_ASSIGN: underlying_op = '+'; break;
            case TK_SUB_ASSIGN: underlying_op = '-'; break;
            case TK_MUL_ASSIGN: underlying_op = '*'; break;
            case TK_DIV_ASSIGN: underlying_op = '/'; break;
            case TK_MOD_ASSIGN: underlying_op = '%'; break;
            case TK_AND_ASSIGN: underlying_op = '&'; break;
            case TK_OR_ASSIGN:  underlying_op = '|'; break;
            case TK_XOR_ASSIGN: underlying_op = '^'; break;
            case TK_SHL_ASSIGN: underlying_op = TOK_SHL; break;
            case TK_SHR_ASSIGN: underlying_op = TOK_SHR; break;
            default:
                expr_error(ctx, node, "unknown compound assignment operator %u", op);
                return NULL;
        }

        TypeInfo *op_res = promote_binary(underlying_op, lt, rt, ctx->perm_arena, ctx->diags);
        if (!op_res) {
            expr_error(ctx, node, "invalid operands to compound assignment %u", op);
            return NULL;
        }

        if (!type_compatible_assign(lt, op_res)) {
            expr_error(ctx, node, "incompatible types in compound assignment");
            return NULL;
        }
    }

    expr_node_set_type(ctx, node, lt);
    return lt;
}

TypeInfo *expr_handle_call(SemContext *ctx, ZeroCCASTNode *node) {
    if (!ctx || !node) return NULL;

    ZeroCCASTNode *callee = expr_get_node(ctx, node->left);
    if (!callee) {
        expr_error(ctx, node, "function call missing callee");
        return NULL;
    }

    TypeInfo *callee_t = expr_handle_expression(ctx, callee);
    if (!callee_t) {
        return NULL;
    }

    /* Callee must be a function or pointer to function */
    TypeInfo *fn_t = NULL;
    if (type_is_function(callee_t)) {
        fn_t = callee_t;
    } else if (type_is_pointer(callee_t) && callee_t->details.pointer.base &&
               type_is_function(callee_t->details.pointer.base)) {
        fn_t = callee_t->details.pointer.base;
    } else {
        expr_error(ctx, node, "called object is not a function or function pointer");
        return NULL;
    }

    /* Analyze and count arguments */
    size_t arg_count = 0;
    ASTRef arg_ref = node->right;
    while (arg_ref != AST_NULL) {
        ZeroCCASTNode *arg_node = expr_get_node(ctx, arg_ref);
        if (!arg_node) break;

        TypeInfo *arg_t = expr_handle_expression(ctx, arg_node);
        if (!arg_t) {
            return NULL;
        }

        /* Check parameter type match if within declared parameter count */
        if (arg_count < fn_t->details.function.param_cnt) {
            TypeInfo *param_t = fn_t->details.function.params[arg_count];
            if (!type_compatible_assign(param_t, arg_t)) {
                expr_error(ctx, arg_node, "passing incompatible type for argument %zu of function call", arg_count + 1);
                return NULL;
            }
        }

        arg_count++;
        arg_ref = arg_node->next;
    }

    /* Verify argument count — only against prototype types; no-prototype
     * declarations (empty parens, implicit) constrain nothing (C99 6.7.5.3). */
    if (fn_t->details.function.is_prototype) {
        if (fn_t->details.function.is_variadic) {
            if (arg_count < fn_t->details.function.param_cnt) {
                expr_error(ctx, node, "too few arguments to function call (expected at least %zu, have %zu)",
                           fn_t->details.function.param_cnt, arg_count);
                return NULL;
            }
        } else {
            if (arg_count != fn_t->details.function.param_cnt) {
                expr_error(ctx, node, "argument count mismatch in function call (expected %zu, have %zu)",
                           fn_t->details.function.param_cnt, arg_count);
                return NULL;
            }
        }
    }

    TypeInfo *res = fn_t->details.function.ret;
    if (!res) {
        res = type_void(ctx->perm_arena);
    }

    expr_node_set_type(ctx, node, res);
    return res;
}

TypeInfo *expr_handle_cast(SemContext *ctx, ZeroCCASTNode *node) {
    if (!ctx || !node) return NULL;

    ZeroCCASTNode *expr = expr_get_node(ctx, node->left);
    if (!expr) {
        expr_error(ctx, node, "cast expression missing operand");
        return NULL;
    }

    TypeInfo *src_t = expr_handle_expression(ctx, expr);
    if (!src_t) {
        return NULL;
    }

    /* Target type is retrieved from node or default to int */
    TypeInfo *dst_t = expr_node_type(ctx, node);
    if (!dst_t) {
        dst_t = type_int(ctx->perm_arena);
    }

    if (!type_can_cast(src_t, dst_t)) {
        expr_error(ctx, node, "cannot cast expression of given type to target type");
        return NULL;
    }

    expr_node_set_type(ctx, node, dst_t);
    return dst_t;
}

TypeInfo *expr_handle_va_arg(SemContext *ctx, ZeroCCASTNode *node) {
    if (!ctx || !node) return NULL;

    ZeroCCASTNode *ap_expr = expr_get_node(ctx, node->left);
    if (!ap_expr) {
        expr_error(ctx, node, "va_arg missing va_list argument");
        return NULL;
    }

    /* First argument must be a va_list (pointer type) */
    TypeInfo *ap_t = expr_handle_expression(ctx, ap_expr);
    if (!ap_t) {
        return NULL;
    }

    /* Accept pointer, array, or typedef that resolves to pointer */
    /* For now, accept any type since va_list can be typedef'd */
    /* A proper implementation would check if it's typedef'd to a pointer */

    /* Get the target type from aux.step (parsed TypeInfo*) or aux.extra (type tag) */
    TypeInfo *target_t = NULL;
    if (node->aux.extra == 0 && node->aux.step > 0) {
        target_t = (TypeInfo *)arena_index_to_ptr(ctx->perm_arena, (size_t)(node->aux.step - 1));
    }
    if (!target_t) {
        switch (node->aux.extra) {
            case 1: target_t = type_int(ctx->perm_arena); break;    /* int */
            case 2: target_t = type_char(ctx->perm_arena); break;   /* char */
            case 3: target_t = type_double(ctx->perm_arena); break; /* double */
            case 4: target_t = type_float(ctx->perm_arena); break;  /* float */
            default: target_t = type_int(ctx->perm_arena); break;   /* default to int */
        }
    }

    /* The result type is the target type */
    expr_node_set_type(ctx, node, target_t);
    return target_t;
}

TypeInfo *expr_handle_va_start(SemContext *ctx, ZeroCCASTNode *node) {
    if (!ctx || !node) return NULL;

    ZeroCCASTNode *ap_expr = expr_get_node(ctx, node->left);
    if (!ap_expr) {
        expr_error(ctx, node, "va_start missing va_list argument");
        return NULL;
    }

    /* First argument must be a va_list (pointer type) */
    TypeInfo *ap_t = expr_handle_expression(ctx, ap_expr);
    if (!ap_t) {
        return NULL;
    }

    /* Second argument is the last fixed parameter (used for context, not value) */
    ZeroCCASTNode *last_expr = expr_get_node(ctx, node->right);
    if (!last_expr) {
        expr_error(ctx, node, "va_start missing last parameter");
        return NULL;
    }

    /* The result type is void (it's a statement expression) */
    expr_node_set_type(ctx, node, type_void(ctx->perm_arena));
    return type_void(ctx->perm_arena);
}

TypeInfo *expr_handle_member(SemContext *ctx, ZeroCCASTNode *node) {
    if (!ctx || !node) return NULL;

    ZeroCCASTNode *target = expr_get_node(ctx, node->left);
    if (!target) {
        expr_error(ctx, node, "member access missing target");
        return NULL;
    }

    TypeInfo *target_t = expr_handle_expression(ctx, target);
    if (!target_t) {
        return NULL;
    }

    bool is_arrow = (node->flags != 0);

    /* If arrow -> target must be pointer to struct/union */
    TypeInfo *struct_t = NULL;
    if (is_arrow) {
        if (!type_is_pointer(target_t) || !target_t->details.pointer.base) {
            expr_error(ctx, node, "member reference type is not a pointer");
            return NULL;
        }
        struct_t = target_t->details.pointer.base;
    } else {
        struct_t = target_t;
    }

    if (struct_t->kind != TYPE_KIND_STRUCT && struct_t->kind != TYPE_KIND_UNION) {
        expr_error(ctx, node, "member reference base is not a structure or union");
        return NULL;
    }

    /* Field lookup from tag / struct definition in symbol table */
    TypeInfo *member_t = NULL;
    /* s.str is clobbered by the aux.alt offset store below; keep the name in
     * member_name so re-analysis of the same node still sees it. */
    if (!node->member_name) node->member_name = node->s.str;
    const char *member_name = node->member_name;
    size_t member_offset = 0;

    if (struct_t->details.record.tag) {
        Symbol *sym = scope_lookup(ctx->current_scope, struct_t->details.record.tag);
        if (sym && sym->type) {
            member_t = sym->type;
        }
    }

    /* Try to find field by name in struct fields */
    if (struct_t->details.record.fields && member_name) {
        struct StructField *fld = type_find_field_info(struct_t, member_name);
        if (fld) {
            member_t = fld->type;
            member_offset = fld->offset;
            if (fld->is_bitfield) {
                node->flags |= NODE_MEMBER_IS_BITFIELD;
                node->aux.step = ((uint32_t)fld->bit_offset) |
                                 (((uint32_t)fld->bit_width) << 8) |
                                 (((uint32_t)(fld->is_signed ? 1 : 0)) << 16);
                /* C11 6.3.1.1p2: integer promotions apply to bit-fields:
                 * if int can represent all values, promote to int;
                 * otherwise if unsigned int can represent all values, promote to unsigned int. */
                if (fld->bit_width < 32 || (fld->bit_width == 32 && fld->is_signed)) {
                    member_t = type_int(ctx->perm_arena);
                } else if (fld->bit_width == 32 && !fld->is_signed) {
                    member_t = type_uint(ctx->perm_arena);
                }
            }
        } else {
            /* A field table exists but has no such member: C99 6.5.2.1p3
             * constraint violation.  (Previously silently defaulted to
             * int, so `p.y2 = 2` on a field-less struct compiled and the
             * write went to offset 0 — a miscompile.) */
            const char *tag_or_struct = struct_t->details.record.tag ?
                                        struct_t->details.record.tag : "struct";
            const char *names[128];
            size_t nn = 0;
            for (size_t fi = 0; fi < struct_t->details.record.field_count && nn < 128; fi++) {
                if (struct_t->details.record.fields[fi].name) names[nn++] = struct_t->details.record.fields[fi].name;
            }
            const char *sugg = diag_best_match(member_name, strlen(member_name), names, nn);
            if (sugg) {
                expr_error(ctx, node, "'%s' has no member named '%s'; did you mean '%s'?",
                           tag_or_struct, member_name, sugg);
            } else {
                expr_error(ctx, node, "'%s' has no member named '%s'",
                           tag_or_struct, member_name);
            }
            return NULL;
        }
    }

    if (!member_t) {
        /* No field table recorded (incomplete/late struct info): keep the
         * legacy lenient default rather than a false positive. */
        member_t = type_int(ctx->perm_arena);
    }

    if (struct_t && (struct_t->modifiers & TYPE_MOD_CONST)) {
        if (member_t && (member_t->modifiers & TYPE_MOD_CONST) == 0) {
            TypeInfo *qualified_member = type_new(ctx->perm_arena);
            *qualified_member = *member_t;
            qualified_member->modifiers |= TYPE_MOD_CONST;
            member_t = qualified_member;
        }
    }
    if (struct_t && (struct_t->modifiers & TYPE_MOD_VOLATILE)) {
        if (member_t && (member_t->modifiers & TYPE_MOD_VOLATILE) == 0) {
            TypeInfo *qualified_member = type_new(ctx->perm_arena);
            *qualified_member = *member_t;
            qualified_member->modifiers |= TYPE_MOD_VOLATILE;
            member_t = qualified_member;
        }
    }

    /* Store member offset in node->aux.alt for codegen (aux.extra used for type info) */
    node->aux.alt = (uint32_t)member_offset;

    expr_node_set_type(ctx, node, member_t);
    return member_t;
}

TypeInfo *expr_handle_index(SemContext *ctx, ZeroCCASTNode *node) {
    if (!ctx || !node) return NULL;

    ZeroCCASTNode *arr = expr_get_node(ctx, node->left);
    ZeroCCASTNode *idx = expr_get_node(ctx, node->right);
    if (!arr || !idx) {
        expr_error(ctx, node, "array subscript missing operand");
        return NULL;
    }

    TypeInfo *arr_t = expr_handle_expression(ctx, arr);
    TypeInfo *idx_t = expr_handle_expression(ctx, idx);
    if (!arr_t || !idx_t) {
        return NULL;
    }

    /* In C, one operand must be pointer/array, the other integer */
    TypeInfo *ptr_side = NULL;
    TypeInfo *idx_side = NULL;

    if (type_is_pointer(arr_t) || type_is_array(arr_t)) {
        ptr_side = arr_t;
        idx_side = idx_t;
    } else if (type_is_pointer(idx_t) || type_is_array(idx_t)) {
        ptr_side = idx_t;
        idx_side = arr_t;
    } else {
        expr_error(ctx, node, "subscripted value is not an array or pointer");
        return NULL;
    }

    if (!type_is_integer(idx_side)) {
        expr_error(ctx, node, "array subscript is not an integer");
        return NULL;
    }

    TypeInfo *elem_t = NULL;
    if (type_is_pointer(ptr_side)) {
        elem_t = ptr_side->details.pointer.base;
    } else {
        elem_t = ptr_side->details.array.base;
    }

    if (!elem_t) {
        elem_t = type_int(ctx->perm_arena);
    }

    expr_node_set_type(ctx, node, elem_t);
    return elem_t;
}

TypeInfo *expr_handle_ternary(SemContext *ctx, ZeroCCASTNode *node) {
    if (!ctx || !node) return NULL;

    ZeroCCASTNode *cond = expr_get_node(ctx, node->left);
    ZeroCCASTNode *then_n = expr_get_node(ctx, node->right);
    ZeroCCASTNode *else_n = expr_get_node(ctx, node->aux.alt);

    if (!cond || !then_n || !else_n) {
        expr_error(ctx, node, "ternary operator missing operand");
        return NULL;
    }

    TypeInfo *cond_t = expr_handle_expression(ctx, cond);
    TypeInfo *then_t = expr_handle_expression(ctx, then_n);
    TypeInfo *else_t = expr_handle_expression(ctx, else_n);

    if (!cond_t || !then_t || !else_t) {
        return NULL;
    }

    if (!type_is_arithmetic(cond_t) && !type_is_pointer(cond_t)) {
        expr_error(ctx, node, "conditional expression condition must be scalar");
        return NULL;
    }

    /* If both are arithmetic, usual arithmetic conversions */
    TypeInfo *res = NULL;
    if (type_is_arithmetic(then_t) && type_is_arithmetic(else_t)) {
        res = type_common_arithmetic_type(ctx->perm_arena, then_t, else_t);
    } else if (type_is_compatible(then_t, else_t)) {
        res = then_t;
    } else if (type_is_pointer(then_t) && type_is_pointer(else_t)) {
        /* If one is void* */
        if (type_is_void(then_t->details.pointer.base) || type_is_void(else_t->details.pointer.base)) {
            res = type_pointer_to(ctx->perm_arena, type_void(ctx->perm_arena));
        } else {
            res = then_t;
        }
    } else {
        res = then_t;
    }

    if (!res) {
        res = then_t;
    }

    expr_node_set_type(ctx, node, res);
    return res;
}

TypeInfo *expr_handle_expression(SemContext *ctx, ZeroCCASTNode *node) {
    if (!ctx || !node) return NULL;

    switch (node->tag) {
        /* Literals & Primary */
        case NODE_INT:
        case NODE_FLOAT:
        case NODE_STRING:
        case NODE_BOOL:
        case NODE_NULLPTR:
            return expr_handle_literal(ctx, node);

        case NODE_IDENT:
            return expr_handle_identifier(ctx, node);

        /* Operators */
        case NODE_UNOP:
            return expr_handle_unary(ctx, node);

        case NODE_BINOP:
            return expr_handle_binary(ctx, node);

        case NODE_ASSIGN:
            return expr_handle_assignment(ctx, node);

        case NODE_CALL:
            return expr_handle_call(ctx, node);

        case NODE_CAST:
            return expr_handle_cast(ctx, node);

        case NODE_VA_START:
            return expr_handle_va_start(ctx, node);

        case NODE_VA_ARG:
            return expr_handle_va_arg(ctx, node);

        case NODE_MEMBER:
            return expr_handle_member(ctx, node);

        case NODE_INDEX:
            return expr_handle_index(ctx, node);

        case NODE_TERNARY:
            return expr_handle_ternary(ctx, node);

        case NODE_COMMA: {
            ZeroCCASTNode *left_node = expr_get_node(ctx, node->left);
            if (left_node) (void)expr_handle_expression(ctx, left_node);
            ZeroCCASTNode *right_node = expr_get_node(ctx, node->right);
            TypeInfo *res = right_node ? expr_handle_expression(ctx, right_node) : NULL;
            expr_node_set_type(ctx, node, res);
            return res;
        }

        case NODE_COMPOUND_LIT: {
            ASTRef cur_ref = node->left;
            while (cur_ref != AST_NULL) {
                ZeroCCASTNode *elem_node = expr_get_node(ctx, cur_ref);
                if (elem_node) {
                    (void)expr_handle_expression(ctx, elem_node);
                    cur_ref = elem_node->next;
                } else {
                    break;
                }
            }
            return expr_node_type(ctx, node);
        }

        case NODE_STMT_EXPR: {
            ZeroCCASTNode *block = expr_get_node(ctx, node->left);
            if (block) {
                stmt_handle_block(ctx, block);
            }
            TypeInfo *last_t = NULL;
            if (block) {
                ASTRef cur = block->left;
                while (cur != AST_NULL) {
                    ZeroCCASTNode *st = expr_get_node(ctx, cur);
                    if (!st) break;
                    if (st->next == AST_NULL) {
                        if (st->tag == NODE_EXPR_STMT && st->left != AST_NULL) {
                            last_t = expr_node_type(ctx, expr_get_node(ctx, st->left));
                        }
                    }
                    cur = st->next;
                }
            }
            TypeInfo *res = last_t ? last_t : type_void(ctx->perm_arena);
            expr_node_set_type(ctx, node, res);
            return res;
        }

        default:
            /* Other expressions or statements */
            return expr_node_type(ctx, node);
    }
}
