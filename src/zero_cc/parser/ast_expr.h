#ifndef ZERO_CC_PARSER_AST_EXPR_H
#define ZERO_CC_PARSER_AST_EXPR_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "../types_lexer.h"
#include "../parser.h"
#include "../expressions.h"

/* Forward declaration for statement parsing */
ASTRef zero_cc_parse_stmt_ast(ZeroCCCompiler *c, ZeroCCASTArena *arena);

/* Forward declarations */
static bool is_type_specifier(ZeroCCCompiler *c, ZeroCCTokType t);
static ASTRef parse_expr_ast_internal(ZeroCCCompiler *c, ZeroCCASTArena *arena);

static ASTRef parse_primary_ast(ZeroCCCompiler *c, ZeroCCASTArena *arena) {
    if (c->token_pos >= c->token_count) return AST_NULL;
    ZeroCCToken *tok = &c->tokens[c->token_pos];

    if (tok->type == TK_NUM) {
        c->token_pos++;
        ASTRef ref = zero_cc_ast_alloc_node(arena, NODE_INT);
        ZeroCCASTNode *n = zero_cc_ast_get(arena, ref);
        n->line = (uint32_t)tok->line;
        n->col = (uint32_t)tok->col;
        n->int_val = tok->num_val;
        return ref;
    }

    if (tok->type == TK_FLOAT) {
        c->token_pos++;
        ASTRef ref = zero_cc_ast_alloc_node(arena, NODE_FLOAT);
        ZeroCCASTNode *n = zero_cc_ast_get(arena, ref);
        n->line = (uint32_t)tok->line;
        n->col = (uint32_t)tok->col;
        n->float_val = tok->float_val;
        return ref;
    }

    if (tok->type == TK_STRING) {
        c->token_pos++;
        ASTRef ref = zero_cc_ast_alloc_node(arena, NODE_STRING);
        ZeroCCASTNode *n = zero_cc_ast_get(arena, ref);
        n->line = (uint32_t)tok->line;
        n->col = (uint32_t)tok->col;
        const char *s = tok->allocated_str ? tok->allocated_str : tok->str_val;
        n->s.str = s;
        n->s.len = strlen(s);
        return ref;
    }

    if (tok->type == TK_NULLPTR) {
        c->token_pos++;
        ASTRef ref = zero_cc_ast_alloc_node(arena, NODE_NULLPTR);
        ZeroCCASTNode *n = zero_cc_ast_get(arena, ref);
        n->line = (uint32_t)tok->line;
        n->col = (uint32_t)tok->col;
        return ref;
    }

    if (tok->type == TK_TRUE || tok->type == TK_FALSE) {
        c->token_pos++;
        ASTRef ref = zero_cc_ast_alloc_node(arena, NODE_BOOL);
        ZeroCCASTNode *n = zero_cc_ast_get(arena, ref);
        n->line = (uint32_t)tok->line;
        n->col = (uint32_t)tok->col;
        n->int_val = (tok->type == TK_TRUE) ? 1 : 0;
        return ref;
    }

    if (tok->type == TK_IDENT) {
        c->token_pos++;
        ASTRef ref = zero_cc_ast_alloc_node(arena, NODE_IDENT);
        ZeroCCASTNode *n = zero_cc_ast_get(arena, ref);
        n->line = (uint32_t)tok->line;
        n->col = (uint32_t)tok->col;
        n->s.str = tok->str_val;
        n->s.len = strlen(tok->str_val);
        return ref;
    }

    /* C11 _Generic compile-time selection */
    if (tok->type == TK_GENERIC) {
        c->token_pos++;
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LPAREN) c->token_pos++;
        size_t ctrl_pos = c->token_pos;
        ZeroCCType ctrl_ty = zero_cc_type_of_equality(c, &ctrl_pos);
        c->token_pos = ctrl_pos;
        ctrl_ty = zero_cc_type_strip_qualifiers(ctrl_ty);
        if (ctrl_ty.tag == TYPE_ARRAY) {
            ctrl_ty = zero_cc_type_ptr(&ctrl_ty);
        }
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_COMMA) c->token_pos++;

        ASTRef selected_expr = AST_NULL;
        size_t default_expr_start = 0;
        bool matched = false;

        while (c->token_pos < c->token_count && c->tokens[c->token_pos].type != TK_RPAREN && c->tokens[c->token_pos].type != TK_EOF) {
            if (c->tokens[c->token_pos].type == TK_DEFAULT) {
                c->token_pos++;
                if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_COLON) c->token_pos++;
                default_expr_start = c->token_pos;
                /* Skip expression tokens until comma or rparen */
                int paren_depth = 0;
                while (c->token_pos < c->token_count && c->tokens[c->token_pos].type != TK_EOF) {
                    ZeroCCTokType tt = c->tokens[c->token_pos].type;
                    if (tt == TK_LPAREN) paren_depth++;
                    else if (tt == TK_RPAREN) {
                        if (paren_depth == 0) break;
                        paren_depth--;
                    } else if (tt == TK_COMMA && paren_depth == 0) {
                        break;
                    }
                    c->token_pos++;
                }
            } else {
                ZeroCCType assoc_ty = zero_cc_parse_type_name(c);
                if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_COLON) c->token_pos++;
                assoc_ty = zero_cc_type_strip_qualifiers(assoc_ty);
                if (!matched && zero_cc_types_compatible(ctrl_ty, assoc_ty)) {
                    matched = true;
                    selected_expr = parse_expr_ast_internal(c, arena);
                } else {
                    /* Skip expression */
                    int paren_depth = 0;
                    while (c->token_pos < c->token_count && c->tokens[c->token_pos].type != TK_EOF) {
                        ZeroCCTokType tt = c->tokens[c->token_pos].type;
                        if (tt == TK_LPAREN) paren_depth++;
                        else if (tt == TK_RPAREN) {
                            if (paren_depth == 0) break;
                            paren_depth--;
                        } else if (tt == TK_COMMA && paren_depth == 0) {
                            break;
                        }
                        c->token_pos++;
                    }
                }
            }
            if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_COMMA) c->token_pos++;
        }
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RPAREN) c->token_pos++;
        if (matched) return selected_expr;
        if (default_expr_start > 0) {
            c->token_pos = default_expr_start;
            return parse_expr_ast_internal(c, arena);
        }
        return AST_NULL;
    }

    if (tok->type == TK_LPAREN) {
        size_t cast_start = c->token_pos;
        if (c->token_pos + 1 < c->token_count && is_type_specifier(c, c->tokens[c->token_pos + 1].type)) {
            c->token_pos += 2; /* Skip '(' and type */
            while (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_STAR) c->token_pos++;
            /* Check array brackets: (type[]) */
            while (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LBRACKET) {
                while (c->token_pos < c->token_count && c->tokens[c->token_pos].type != TK_RBRACKET) {
                    c->token_pos++;
                }
                if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RBRACKET) c->token_pos++;
            }
            if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RPAREN) {
                c->token_pos++;
                if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LBRACE) {
                    c->token_pos++; /* skip '{' */
                    while (c->token_pos < c->token_count &&
                           c->tokens[c->token_pos].type == TK_DOT &&
                           c->token_pos + 1 < c->token_count &&
                           c->tokens[c->token_pos + 1].type == TK_IDENT) {
                        c->token_pos += 2;
                        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_ASSIGN) c->token_pos++;
                    }
                    ASTRef first_expr = parse_expr_ast_internal(c, arena);
                    int depth = 1;
                    while (depth > 0 && c->token_pos < c->token_count && c->tokens[c->token_pos].type != TK_EOF) {
                        if (c->tokens[c->token_pos].type == TK_LBRACE) depth++;
                        else if (c->tokens[c->token_pos].type == TK_RBRACE) depth--;
                        c->token_pos++;
                    }
                    ASTRef comp_ref = zero_cc_ast_alloc_node(arena, NODE_COMPOUND_LIT);
                    ZeroCCASTNode *cn = zero_cc_ast_get(arena, comp_ref);
                    cn->left = first_expr;
                    return comp_ref;
                }
            }
            c->token_pos = cast_start;
        }

        c->token_pos++;
        ASTRef inner = parse_expr_ast_internal(c, arena);
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RPAREN) {
            c->token_pos++;
        }
        return inner;
    }

    return AST_NULL;
}

/* Postfix Expressions: a(params), a[i], a.mem, a->mem, a++, a-- */
static ASTRef parse_postfix_ast(ZeroCCCompiler *c, ZeroCCASTArena *arena) {
    ASTRef expr = parse_primary_ast(c, arena);
    if (expr == AST_NULL) return AST_NULL;

    while (c->token_pos < c->token_count) {
        ZeroCCTokType t = c->tokens[c->token_pos].type;

        /* Function Call: expr(arg1, arg2, ...) */
        if (t == TK_LPAREN) {
            c->token_pos++;
            ASTRef call_ref = zero_cc_ast_alloc_node(arena, NODE_CALL);
            ZeroCCASTNode *call_node = zero_cc_ast_get(arena, call_ref);
            call_node->left = expr;

            ASTRef last_arg = AST_NULL;
            while (c->token_pos < c->token_count && c->tokens[c->token_pos].type != TK_RPAREN) {
                ASTRef arg = parse_expr_ast_internal(c, arena);
                if (arg != AST_NULL) {
                    if (call_node->right == AST_NULL) {
                        call_node->right = arg;
                    } else if (last_arg != AST_NULL) {
                        ZeroCCASTNode *last_node = zero_cc_ast_get(arena, last_arg);
                        last_node->next = arg;
                    }
                    last_arg = arg;
                }
                if (c->tokens[c->token_pos].type == TK_COMMA) c->token_pos++;
            }
            if (c->tokens[c->token_pos].type == TK_RPAREN) c->token_pos++;
            expr = call_ref;
            continue;
        }

        /* Array Indexing: expr[index] */
        if (t == TK_LBRACKET) {
            c->token_pos++;
            ASTRef idx = parse_expr_ast_internal(c, arena);
            if (c->tokens[c->token_pos].type == TK_RBRACKET) c->token_pos++;
            ASTRef idx_ref = zero_cc_ast_alloc_node(arena, NODE_INDEX);
            ZeroCCASTNode *in = zero_cc_ast_get(arena, idx_ref);
            in->left = expr;
            in->right = idx;
            expr = idx_ref;
            continue;
        }

        /* Member Access: expr.mem or expr->mem */
        if (t == TK_DOT || t == TK_ARROW) {
            bool is_arrow = (t == TK_ARROW);
            c->token_pos++;
            if (c->tokens[c->token_pos].type == TK_IDENT) {
                ASTRef mem_ref = zero_cc_ast_alloc_node(arena, NODE_MEMBER);
                ZeroCCASTNode *mn = zero_cc_ast_get(arena, mem_ref);
                mn->left = expr;
                mn->flags = is_arrow ? 1 : 0;
                mn->s.str = c->tokens[c->token_pos].str_val;
                mn->s.len = strlen(c->tokens[c->token_pos].str_val);
                c->token_pos++;
                expr = mem_ref;
                continue;
            }
        }

        /* Postfix Increment / Decrement: expr++, expr-- */
        if (t == TK_INC || t == TK_DEC) {
            c->token_pos++;
            ASTRef un_ref = zero_cc_ast_alloc_node(arena, NODE_UNOP);
            ZeroCCASTNode *un = zero_cc_ast_get(arena, un_ref);
            un->left = expr;
            un->aux.op = (uint32_t)t;
            un->flags = 1; /* postfix flag */
            expr = un_ref;
            continue;
        }

        break;
    }
    return expr;
}

/* Unary Expressions: +a, -a, !a, ~a, *a, &a, ++a, --a */
static ASTRef parse_unary_ast(ZeroCCCompiler *c, ZeroCCASTArena *arena) {
    if (c->token_pos >= c->token_count) return AST_NULL;
    ZeroCCTokType t = c->tokens[c->token_pos].type;

    if (t == TK_PLUS || t == TK_MINUS || t == TK_BANG || t == TK_TILDE ||
        t == TK_STAR || t == TK_AMP || t == TK_INC || t == TK_DEC) {
        c->token_pos++;
        ASTRef operand = parse_unary_ast(c, arena);
        ASTRef un_ref = zero_cc_ast_alloc_node(arena, NODE_UNOP);
        ZeroCCASTNode *un = zero_cc_ast_get(arena, un_ref);
        un->left = operand;
        un->aux.op = (uint32_t)t;
        un->flags = 0; /* prefix */
        return un_ref;
    }

    return parse_postfix_ast(c, arena);
}

/* Multiplicative Expressions: *, /, % */
static ASTRef parse_multiplicative_ast(ZeroCCCompiler *c, ZeroCCASTArena *arena) {
    ASTRef left = parse_unary_ast(c, arena);
    while (c->token_pos < c->token_count) {
        ZeroCCTokType t = c->tokens[c->token_pos].type;
        if (t == TK_STAR || t == TK_SLASH || t == TK_PERCENT) {
            c->token_pos++;
            ASTRef right = parse_unary_ast(c, arena);
            ASTRef bin_ref = zero_cc_ast_alloc_node(arena, NODE_BINOP);
            ZeroCCASTNode *bn = zero_cc_ast_get(arena, bin_ref);
            bn->left = left;
            bn->right = right;
            bn->aux.op = (uint32_t)t;
            left = bin_ref;
        } else {
            break;
        }
    }
    return left;
}

/* Additive Expressions: +, - */
static ASTRef parse_additive_ast(ZeroCCCompiler *c, ZeroCCASTArena *arena) {
    ASTRef left = parse_multiplicative_ast(c, arena);
    while (c->token_pos < c->token_count) {
        ZeroCCTokType t = c->tokens[c->token_pos].type;
        if (t == TK_PLUS || t == TK_MINUS) {
            c->token_pos++;
            ASTRef right = parse_multiplicative_ast(c, arena);
            ASTRef bin_ref = zero_cc_ast_alloc_node(arena, NODE_BINOP);
            ZeroCCASTNode *bn = zero_cc_ast_get(arena, bin_ref);
            bn->left = left;
            bn->right = right;
            bn->aux.op = (uint32_t)t;
            left = bin_ref;
        } else {
            break;
        }
    }
    return left;
}

/* Shift Expressions: <<, >> */
static ASTRef parse_shift_ast(ZeroCCCompiler *c, ZeroCCASTArena *arena) {
    ASTRef left = parse_additive_ast(c, arena);
    while (c->token_pos < c->token_count) {
        ZeroCCTokType t = c->tokens[c->token_pos].type;
        if (t == TK_SHL || t == TK_SHR) {
            c->token_pos++;
            ASTRef right = parse_additive_ast(c, arena);
            ASTRef bin_ref = zero_cc_ast_alloc_node(arena, NODE_BINOP);
            ZeroCCASTNode *bn = zero_cc_ast_get(arena, bin_ref);
            bn->left = left;
            bn->right = right;
            bn->aux.op = (uint32_t)t;
            left = bin_ref;
        } else {
            break;
        }
    }
    return left;
}

/* Relational Expressions: <, >, <=, >= */
static ASTRef parse_relational_ast(ZeroCCCompiler *c, ZeroCCASTArena *arena) {
    ASTRef left = parse_shift_ast(c, arena);
    while (c->token_pos < c->token_count) {
        ZeroCCTokType t = c->tokens[c->token_pos].type;
        if (t == TK_LT || t == TK_GT || t == TK_LE || t == TK_GE) {
            c->token_pos++;
            ASTRef right = parse_shift_ast(c, arena);
            ASTRef bin_ref = zero_cc_ast_alloc_node(arena, NODE_BINOP);
            ZeroCCASTNode *bn = zero_cc_ast_get(arena, bin_ref);
            bn->left = left;
            bn->right = right;
            bn->aux.op = (uint32_t)t;
            left = bin_ref;
        } else {
            break;
        }
    }
    return left;
}

/* Equality Expressions: ==, != */
static ASTRef parse_equality_ast(ZeroCCCompiler *c, ZeroCCASTArena *arena) {
    ASTRef left = parse_relational_ast(c, arena);
    while (c->token_pos < c->token_count) {
        ZeroCCTokType t = c->tokens[c->token_pos].type;
        if (t == TK_EQ || t == TK_NE) {
            c->token_pos++;
            ASTRef right = parse_relational_ast(c, arena);
            ASTRef bin_ref = zero_cc_ast_alloc_node(arena, NODE_BINOP);
            ZeroCCASTNode *bn = zero_cc_ast_get(arena, bin_ref);
            bn->left = left;
            bn->right = right;
            bn->aux.op = (uint32_t)t;
            left = bin_ref;
        } else {
            break;
        }
    }
    return left;
}

/* Bitwise AND: & */
static ASTRef parse_bit_and_ast(ZeroCCCompiler *c, ZeroCCASTArena *arena) {
    ASTRef left = parse_equality_ast(c, arena);
    while (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_AMP) {
        c->token_pos++;
        ASTRef right = parse_equality_ast(c, arena);
        ASTRef bin_ref = zero_cc_ast_alloc_node(arena, NODE_BINOP);
        ZeroCCASTNode *bn = zero_cc_ast_get(arena, bin_ref);
        bn->left = left;
        bn->right = right;
        bn->aux.op = TK_AMP;
        left = bin_ref;
    }
    return left;
}

/* Bitwise XOR: ^ */
static ASTRef parse_bit_xor_ast(ZeroCCCompiler *c, ZeroCCASTArena *arena) {
    ASTRef left = parse_bit_and_ast(c, arena);
    while (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_CARET) {
        c->token_pos++;
        ASTRef right = parse_bit_and_ast(c, arena);
        ASTRef bin_ref = zero_cc_ast_alloc_node(arena, NODE_BINOP);
        ZeroCCASTNode *bn = zero_cc_ast_get(arena, bin_ref);
        bn->left = left;
        bn->right = right;
        bn->aux.op = TK_CARET;
        left = bin_ref;
    }
    return left;
}

/* Bitwise OR: | */
static ASTRef parse_bit_or_ast(ZeroCCCompiler *c, ZeroCCASTArena *arena) {
    ASTRef left = parse_bit_xor_ast(c, arena);
    while (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_PIPE) {
        c->token_pos++;
        ASTRef right = parse_bit_xor_ast(c, arena);
        ASTRef bin_ref = zero_cc_ast_alloc_node(arena, NODE_BINOP);
        ZeroCCASTNode *bn = zero_cc_ast_get(arena, bin_ref);
        bn->left = left;
        bn->right = right;
        bn->aux.op = TK_PIPE;
        left = bin_ref;
    }
    return left;
}

/* Logical AND: && */
static ASTRef parse_logical_and_ast(ZeroCCCompiler *c, ZeroCCASTArena *arena) {
    ASTRef left = parse_bit_or_ast(c, arena);
    while (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LOGAND) {
        c->token_pos++;
        ASTRef right = parse_bit_or_ast(c, arena);
        ASTRef bin_ref = zero_cc_ast_alloc_node(arena, NODE_BINOP);
        ZeroCCASTNode *bn = zero_cc_ast_get(arena, bin_ref);
        bn->left = left;
        bn->right = right;
        bn->aux.op = TK_LOGAND;
        left = bin_ref;
    }
    return left;
}

/* Logical OR: || */
static ASTRef parse_logical_or_ast(ZeroCCCompiler *c, ZeroCCASTArena *arena) {
    ASTRef left = parse_logical_and_ast(c, arena);
    while (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LOGOR) {
        c->token_pos++;
        ASTRef right = parse_logical_and_ast(c, arena);
        ASTRef bin_ref = zero_cc_ast_alloc_node(arena, NODE_BINOP);
        ZeroCCASTNode *bn = zero_cc_ast_get(arena, bin_ref);
        bn->left = left;
        bn->right = right;
        bn->aux.op = TK_LOGOR;
        left = bin_ref;
    }
    return left;
}

/* Ternary Expression: cond ? then_expr : else_expr */
static ASTRef parse_ternary_ast(ZeroCCCompiler *c, ZeroCCASTArena *arena) {
    ASTRef cond = parse_logical_or_ast(c, arena);
    if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_QUESTION) {
        c->token_pos++;
        ASTRef then_expr = parse_expr_ast_internal(c, arena);
        if (c->tokens[c->token_pos].type == TK_COLON) c->token_pos++;
        ASTRef else_expr = parse_ternary_ast(c, arena);

        ASTRef tern_ref = zero_cc_ast_alloc_node(arena, NODE_TERNARY);
        ZeroCCASTNode *tn = zero_cc_ast_get(arena, tern_ref);
        tn->left = cond;
        tn->right = then_expr;
        tn->aux.alt = else_expr;
        return tern_ref;
    }
    return cond;
}

/* Assignment Expressions: target = val or compound assign */
static bool is_assign_op(ZeroCCTokType t) {
    return (t == TK_ASSIGN || t == TK_ADD_ASSIGN || t == TK_SUB_ASSIGN ||
            t == TK_MUL_ASSIGN || t == TK_DIV_ASSIGN || t == TK_MOD_ASSIGN ||
            t == TK_AND_ASSIGN || t == TK_OR_ASSIGN || t == TK_XOR_ASSIGN ||
            t == TK_SHL_ASSIGN || t == TK_SHR_ASSIGN);
}

static ASTRef parse_expr_ast_internal(ZeroCCCompiler *c, ZeroCCASTArena *arena) {
    ASTRef left = parse_ternary_ast(c, arena);
    if (c->token_pos < c->token_count && is_assign_op(c->tokens[c->token_pos].type)) {
        ZeroCCTokType op = c->tokens[c->token_pos].type;
        c->token_pos++;
        ASTRef right = parse_expr_ast_internal(c, arena);
        ASTRef assign_ref = zero_cc_ast_alloc_node(arena, NODE_ASSIGN);
        ZeroCCASTNode *an = zero_cc_ast_get(arena, assign_ref);
        an->left = left;
        an->right = right;
        an->aux.op = (uint32_t)op;
        return assign_ref;
    }
    return left;
}

/* Public entry point for Expression AST parsing */
ASTRef zero_cc_parse_expr_ast(ZeroCCCompiler *c, ZeroCCASTArena *arena) {
    if (!c || !arena) return AST_NULL;
    return parse_expr_ast_internal(c, arena);
}

/* =========================================================================
 *  Statement AST Builder
 * ========================================================================= */

static void skip_c23_attributes(ZeroCCCompiler *c) {
    while (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LDBRACKET) {
        c->token_pos++;
        int depth = 1;
        while (c->token_pos < c->token_count && c->tokens[c->token_pos].type != TK_EOF && depth > 0) {
            if (c->tokens[c->token_pos].type == TK_LDBRACKET) depth++;
            else if (c->tokens[c->token_pos].type == TK_RDBRACKET) depth--;
            c->token_pos++;
        }
    }
}

static bool is_type_specifier(ZeroCCCompiler *c, ZeroCCTokType t) {
    if (t == TK_INT || t == TK_CHAR || t == TK_VOID || t == TK_STRUCT ||
        t == TK_FLOAT_KW || t == TK_DOUBLE_KW || t == TK_BOOL_KW ||
        t == TK_SIGNED || t == TK_UNSIGNED || t == TK_SHORT || t == TK_LONG ||
        t == TK_AUTO || t == TK_ATOMIC) {
        return true;
    }
    if (t == TK_IDENT && c->token_pos < c->token_count) {
        const char *tname = c->tokens[c->token_pos].allocated_str ? 
                            c->tokens[c->token_pos].allocated_str : c->tokens[c->token_pos].str_val;
        if (zero_cc_find_typedef(c, tname) != NULL) {
            return true;
        }
    }
    return false;
}


#endif /* ZERO_CC_PARSER_AST_EXPR_H */
