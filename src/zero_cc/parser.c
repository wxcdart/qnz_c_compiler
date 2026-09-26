#include "parser.h"
#include "expressions.h"
#include "statements.h"
#include <stdlib.h>
#include <string.h>

void zero_cc_ast_arena_init(ZeroCCASTArena *arena, size_t initial_cap) {
    if (!arena) return;
    arena->capacity = initial_cap ? initial_cap : 1024;
    arena->nodes = (ZeroCCASTNode *)malloc(arena->capacity * sizeof(ZeroCCASTNode));
    /* Reserve index 0 for AST_NULL */
    if (arena->nodes) {
        memset(&arena->nodes[0], 0, sizeof(ZeroCCASTNode));
        arena->count = 1;
    } else {
        arena->count = 0;
        arena->capacity = 0;
    }
}

void zero_cc_ast_arena_free(ZeroCCASTArena *arena) {
    if (!arena) return;
    if (arena->nodes) {
        free(arena->nodes);
        arena->nodes = NULL;
    }
    arena->count = 0;
    arena->capacity = 0;
}

void zero_cc_ast_arena_reset(ZeroCCASTArena *arena) {
    if (!arena) return;
    if (arena->nodes) {
        memset(&arena->nodes[0], 0, sizeof(ZeroCCASTNode));
        arena->count = 1; /* Keep index 0 as AST_NULL */
    } else {
        arena->count = 0;
    }
}

ASTRef zero_cc_ast_alloc_node(ZeroCCASTArena *arena, ZeroCCASTTag tag) {
    if (!arena) return AST_NULL;
    if (arena->count >= arena->capacity) {
        size_t new_cap = arena->capacity ? arena->capacity * 2 : 1024;
        ZeroCCASTNode *new_nodes = (ZeroCCASTNode *)realloc(arena->nodes, new_cap * sizeof(ZeroCCASTNode));
        if (!new_nodes) return AST_NULL;
        arena->nodes = new_nodes;
        arena->capacity = new_cap;
    }

    ASTRef ref = (ASTRef)arena->count++;
    ZeroCCASTNode *node = &arena->nodes[ref];
    memset(node, 0, sizeof(*node));
    node->tag = (uint16_t)tag;
    return ref;
}

/* =========================================================================
 *  Expression AST Builder
 * ========================================================================= */

static ASTRef parse_expr_ast_internal(ZeroCCCompiler *c, ZeroCCASTArena *arena);
static ASTRef parse_unary_ast(ZeroCCCompiler *c, ZeroCCASTArena *arena);
static bool is_type_specifier(ZeroCCCompiler *c, ZeroCCTokType t);

/* Primary Expressions: Literals, identifiers, (expr), function calls */
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

ASTRef zero_cc_parse_stmt_ast(ZeroCCCompiler *c, ZeroCCASTArena *arena) {
    if (!c || !arena || c->token_pos >= c->token_count) return AST_NULL;

    skip_c23_attributes(c);
    if (c->token_pos >= c->token_count) return AST_NULL;

    ZeroCCTokType t = c->tokens[c->token_pos].type;
    uint32_t line = (uint32_t)c->tokens[c->token_pos].line;
    uint32_t col = (uint32_t)c->tokens[c->token_pos].col;

    /* Empty statement */
    if (t == TK_SEMI) {
        c->token_pos++;
        return AST_NULL;
    }

    /* Compound Block: { stmt* } */
    if (t == TK_LBRACE) {
        c->token_pos++;
        ASTRef block_ref = zero_cc_ast_alloc_node(arena, NODE_BLOCK);
        ZeroCCASTNode *bn = zero_cc_ast_get(arena, block_ref);
        bn->line = line;
        bn->col = col;

        ASTRef head = AST_NULL;
        ASTRef tail = AST_NULL;

        while (c->token_pos < c->token_count &&
               c->tokens[c->token_pos].type != TK_RBRACE &&
               c->tokens[c->token_pos].type != TK_EOF) {
            ASTRef s = zero_cc_parse_stmt_ast(c, arena);
            if (s != AST_NULL) {
                if (head == AST_NULL) {
                    head = s;
                    tail = s;
                } else {
                    ZeroCCASTNode *tail_node = zero_cc_ast_get(arena, tail);
                    if (tail_node) {
                        tail_node->next = s;
                    }
                    tail = s;
                }
            }
        }
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RBRACE) {
            c->token_pos++;
        }
        bn = zero_cc_ast_get(arena, block_ref);
        if (bn) bn->left = head;
        return block_ref;
    }

    /* Return: return [expr]; */
    if (t == TK_RETURN) {
        c->token_pos++;
        ASTRef ret_ref = zero_cc_ast_alloc_node(arena, NODE_RETURN);
        ZeroCCASTNode *rn = zero_cc_ast_get(arena, ret_ref);
        rn->line = line;
        rn->col = col;

        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type != TK_SEMI) {
            rn->left = parse_expr_ast_internal(c, arena);
        }
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_SEMI) {
            c->token_pos++;
        }
        return ret_ref;
    }

    /* If statement: if (cond) then [else alt] */
    if (t == TK_IF) {
        c->token_pos++;
        ASTRef if_ref = zero_cc_ast_alloc_node(arena, NODE_IF);
        ZeroCCASTNode *in = zero_cc_ast_get(arena, if_ref);
        in->line = line;
        in->col = col;

        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LPAREN) {
            c->token_pos++;
        }
        ASTRef cond = parse_expr_ast_internal(c, arena);
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RPAREN) {
            c->token_pos++;
        }

        ASTRef then_stmt = zero_cc_parse_stmt_ast(c, arena);
        ASTRef else_stmt = AST_NULL;

        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_ELSE) {
            c->token_pos++;
            else_stmt = zero_cc_parse_stmt_ast(c, arena);
        }

        in = zero_cc_ast_get(arena, if_ref);
        if (in) {
            in->left = cond;
            in->right = then_stmt;
            in->aux.alt = else_stmt;
        }
        return if_ref;
    }

    /* While loop: while (cond) body */
    if (t == TK_WHILE) {
        c->token_pos++;
        ASTRef while_ref = zero_cc_ast_alloc_node(arena, NODE_WHILE);
        ZeroCCASTNode *wn = zero_cc_ast_get(arena, while_ref);
        wn->line = line;
        wn->col = col;

        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LPAREN) {
            c->token_pos++;
        }
        ASTRef cond = parse_expr_ast_internal(c, arena);
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RPAREN) {
            c->token_pos++;
        }

        ASTRef body = zero_cc_parse_stmt_ast(c, arena);
        wn = zero_cc_ast_get(arena, while_ref);
        if (wn) {
            wn->left = cond;
            wn->right = body;
        }
        return while_ref;
    }

    /* Do-While loop: do body while (cond); */
    if (t == TK_DO) {
        c->token_pos++;
        ASTRef do_ref = zero_cc_ast_alloc_node(arena, NODE_DO_WHILE);
        ZeroCCASTNode *dn = zero_cc_ast_get(arena, do_ref);
        dn->line = line;
        dn->col = col;

        ASTRef body = zero_cc_parse_stmt_ast(c, arena);
        ASTRef cond = AST_NULL;

        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_WHILE) {
            c->token_pos++;
            if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LPAREN) {
                c->token_pos++;
            }
            cond = parse_expr_ast_internal(c, arena);
            if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RPAREN) {
                c->token_pos++;
            }
            if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_SEMI) {
                c->token_pos++;
            }
        }
        dn = zero_cc_ast_get(arena, do_ref);
        if (dn) {
            dn->left = cond;
            dn->right = body;
        }
        return do_ref;
    }

    /* For loop: for (init; cond; step) body */
    if (t == TK_FOR) {
        c->token_pos++;
        ASTRef for_ref = zero_cc_ast_alloc_node(arena, NODE_FOR);
        ZeroCCASTNode *fn = zero_cc_ast_get(arena, for_ref);
        fn->line = line;
        fn->col = col;

        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LPAREN) {
            c->token_pos++;
        }

        /* 1. Init */
        ASTRef init_stmt = AST_NULL;
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type != TK_SEMI) {
            init_stmt = zero_cc_parse_stmt_ast(c, arena);
        } else if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_SEMI) {
            c->token_pos++;
        }

        /* 2. Cond */
        ASTRef cond_expr = AST_NULL;
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type != TK_SEMI) {
            cond_expr = parse_expr_ast_internal(c, arena);
        }
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_SEMI) {
            c->token_pos++;
        }

        /* 3. Step */
        ASTRef step_expr = AST_NULL;
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type != TK_RPAREN) {
            step_expr = parse_expr_ast_internal(c, arena);
        }
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RPAREN) {
            c->token_pos++;
        }

        /* 4. Body */
        ASTRef body_stmt = zero_cc_parse_stmt_ast(c, arena);

        fn = zero_cc_ast_get(arena, for_ref);
        if (fn) {
            fn->left = init_stmt;
            fn->right = cond_expr;
            fn->aux.step = step_expr;
            fn->aux.alt = body_stmt;
        }
        return for_ref;
    }

    /* Switch statement: switch (expr) body */
    if (t == TK_SWITCH) {
        c->token_pos++;
        ASTRef sw_ref = zero_cc_ast_alloc_node(arena, NODE_SWITCH);
        ZeroCCASTNode *sn = zero_cc_ast_get(arena, sw_ref);
        sn->line = line;
        sn->col = col;

        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LPAREN) {
            c->token_pos++;
        }
        ASTRef expr = parse_expr_ast_internal(c, arena);
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RPAREN) {
            c->token_pos++;
        }
        ASTRef body = zero_cc_parse_stmt_ast(c, arena);

        sn = zero_cc_ast_get(arena, sw_ref);
        if (sn) {
            sn->left = expr;
            sn->right = body;
        }
        return sw_ref;
    }

    /* Case statement: case const_expr: stmt */
    if (t == TK_CASE) {
        c->token_pos++;
        ASTRef case_ref = zero_cc_ast_alloc_node(arena, NODE_CASE);
        ZeroCCASTNode *cn = zero_cc_ast_get(arena, case_ref);
        cn->line = line;
        cn->col = col;

        ASTRef val = parse_expr_ast_internal(c, arena);
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_COLON) {
            c->token_pos++;
        }
        ASTRef stmt = AST_NULL;
        if (c->token_pos < c->token_count &&
            c->tokens[c->token_pos].type != TK_CASE &&
            c->tokens[c->token_pos].type != TK_DEFAULT &&
            c->tokens[c->token_pos].type != TK_RBRACE) {
            stmt = zero_cc_parse_stmt_ast(c, arena);
        }
        cn = zero_cc_ast_get(arena, case_ref);
        if (cn) {
            cn->left = val;
            cn->right = stmt;
        }
        return case_ref;
    }

    /* Default statement: default: stmt */
    if (t == TK_DEFAULT) {
        c->token_pos++;
        ASTRef def_ref = zero_cc_ast_alloc_node(arena, NODE_DEFAULT);
        ZeroCCASTNode *dn = zero_cc_ast_get(arena, def_ref);
        dn->line = line;
        dn->col = col;

        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_COLON) {
            c->token_pos++;
        }
        ASTRef stmt = AST_NULL;
        if (c->token_pos < c->token_count &&
            c->tokens[c->token_pos].type != TK_CASE &&
            c->tokens[c->token_pos].type != TK_DEFAULT &&
            c->tokens[c->token_pos].type != TK_RBRACE) {
            stmt = zero_cc_parse_stmt_ast(c, arena);
        }
        dn = zero_cc_ast_get(arena, def_ref);
        if (dn) {
            dn->left = stmt;
        }
        return def_ref;
    }

    /* Break */
    if (t == TK_BREAK) {
        c->token_pos++;
        ASTRef brk_ref = zero_cc_ast_alloc_node(arena, NODE_BREAK);
        ZeroCCASTNode *bn = zero_cc_ast_get(arena, brk_ref);
        bn->line = line;
        bn->col = col;
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_SEMI) {
            c->token_pos++;
        }
        return brk_ref;
    }

    /* Continue */
    if (t == TK_CONTINUE) {
        c->token_pos++;
        ASTRef cont_ref = zero_cc_ast_alloc_node(arena, NODE_CONTINUE);
        ZeroCCASTNode *cn = zero_cc_ast_get(arena, cont_ref);
        cn->line = line;
        cn->col = col;
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_SEMI) {
            c->token_pos++;
        }
        return cont_ref;
    }

    /* Static Assert: _Static_assert(cond, [msg]); */
    if (t == TK_STATIC_ASSERT) {
        c->token_pos++;
        ASTRef sa_ref = zero_cc_ast_alloc_node(arena, NODE_STATIC_ASSERT);
        ZeroCCASTNode *san = zero_cc_ast_get(arena, sa_ref);
        san->line = line;
        san->col = col;

        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LPAREN) {
            c->token_pos++;
        }
        ASTRef cond = parse_expr_ast_internal(c, arena);
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_COMMA) {
            c->token_pos++;
            if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_STRING) {
                c->token_pos++;
            }
        }
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RPAREN) {
            c->token_pos++;
        }
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_SEMI) {
            c->token_pos++;
        }
        san = zero_cc_ast_get(arena, sa_ref);
        if (san) san->left = cond;
        return sa_ref;
    }

    /* Variable Declaration or Typedef Declaration */
    /* Skip alignment & storage specifiers */
    while (c->token_pos < c->token_count &&
           (c->tokens[c->token_pos].type == TK_ALIGNAS ||
            c->tokens[c->token_pos].type == TK_CONSTEXPR ||
            c->tokens[c->token_pos].type == TK_CONST ||
            c->tokens[c->token_pos].type == TK_STATIC ||
            c->tokens[c->token_pos].type == TK_EXTERN ||
            c->tokens[c->token_pos].type == TK_THREAD_LOCAL ||
            c->tokens[c->token_pos].type == TK_REGISTER)) {
        if (c->tokens[c->token_pos].type == TK_ALIGNAS) {
            c->token_pos++;
            if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LPAREN) {
                c->token_pos++;
                (void)parse_expr_ast_internal(c, arena);
                if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RPAREN) {
                    c->token_pos++;
                }
            }
        } else {
            c->token_pos++;
        }
    }

    if (c->token_pos < c->token_count) {
        t = c->tokens[c->token_pos].type;
    }

    if (is_type_specifier(c, t)) {
        size_t type_start_pos = c->token_pos;
        ZeroCCTokType type_tok = t;
        c->token_pos++;
        /* Check _Atomic(T) */
        if (type_tok == TK_ATOMIC && c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LPAREN) {
            c->token_pos++;
            if (c->token_pos < c->token_count && is_type_specifier(c, c->tokens[c->token_pos].type)) {
                type_tok = c->tokens[c->token_pos].type;
                c->token_pos++;
            }
            if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RPAREN) {
                c->token_pos++;
            }
        }

        /* Pointer asterisks */
        bool is_ptr = false;
        while (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_STAR) {
            is_ptr = true;
            c->token_pos++;
        }

        /* Check for identifier name */
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_IDENT) {
            ZeroCCToken *var_tok = &c->tokens[c->token_pos];
            const char *vname = var_tok->allocated_str ? var_tok->allocated_str : var_tok->str_val;
            c->token_pos++;

            /* Register local variable in compiler symbol table */
            ZeroCCType decl_type;
            if (type_tok == TK_CHAR || type_tok == TK_BOOL_KW) decl_type = zero_cc_type_char();
            else if (type_tok == TK_SHORT) decl_type = zero_cc_type_int16();
            else if (type_tok == TK_INT) decl_type = zero_cc_type_int();
            else if (type_tok == TK_FLOAT_KW) decl_type = zero_cc_type_float();
            else if (type_tok == TK_DOUBLE_KW) decl_type = zero_cc_type_double();
            else if (type_tok == TK_VOID) decl_type = zero_cc_type_void();
            else {
                ZeroCCType *tdef = zero_cc_find_typedef(c, c->tokens[type_start_pos].str_val);
                decl_type = tdef ? *tdef : zero_cc_type_int64();
            }
            if (is_ptr) decl_type = zero_cc_type_ptr(&decl_type);
            zero_cc_add_local(c, vname, decl_type);

            ASTRef decl_ref = zero_cc_ast_alloc_node(arena, NODE_DECL);
            ZeroCCASTNode *dn = zero_cc_ast_get(arena, decl_ref);
            dn->line = line;
            dn->col = col;
            dn->s.str = vname;
            dn->s.len = strlen(vname);

            /* Optional initializer: = expr */
            if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_ASSIGN) {
                c->token_pos++;
                ASTRef init_val = parse_expr_ast_internal(c, arena);
                dn = zero_cc_ast_get(arena, decl_ref);
                if (dn) dn->left = init_val;
            }

            if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_SEMI) {
                c->token_pos++;
            }
            return decl_ref;
        }
    }

    /* General Expression Statement */
    ASTRef expr = parse_expr_ast_internal(c, arena);
    if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_SEMI) {
        c->token_pos++;
    }
    if (expr != AST_NULL) {
        ASTRef es_ref = zero_cc_ast_alloc_node(arena, NODE_EXPR_STMT);
        ZeroCCASTNode *esn = zero_cc_ast_get(arena, es_ref);
        esn->line = line;
        esn->col = col;
        esn->left = expr;
        return es_ref;
    }

    return AST_NULL;
}

/* =========================================================================
 *  AST Dumper / Pretty Printer
 * ========================================================================= */

static const char *op_to_str(uint32_t op) {
    switch (op) {
        case TK_PLUS: return "+";
        case TK_MINUS: return "-";
        case TK_STAR: return "*";
        case TK_SLASH: return "/";
        case TK_PERCENT: return "%";
        case TK_AMP: return "&";
        case TK_PIPE: return "|";
        case TK_CARET: return "^";
        case TK_SHL: return "<<";
        case TK_SHR: return ">>";
        case TK_EQ: return "==";
        case TK_NE: return "!=";
        case TK_LT: return "<";
        case TK_GT: return ">";
        case TK_LE: return "<=";
        case TK_GE: return ">=";
        case TK_LOGAND: return "&&";
        case TK_LOGOR: return "||";
        case TK_ASSIGN: return "=";
        case TK_ADD_ASSIGN: return "+=";
        case TK_SUB_ASSIGN: return "-=";
        case TK_MUL_ASSIGN: return "*=";
        case TK_DIV_ASSIGN: return "/=";
        case TK_MOD_ASSIGN: return "%=";
        case TK_AND_ASSIGN: return "&=";
        case TK_OR_ASSIGN: return "|=";
        case TK_XOR_ASSIGN: return "^=";
        case TK_SHL_ASSIGN: return "<<=";
        case TK_SHR_ASSIGN: return ">>=";
        case TK_BANG: return "!";
        case TK_TILDE: return "~";
        case TK_INC: return "++";
        case TK_DEC: return "--";
        default: return "?";
    }
}

void zero_cc_ast_dump(ZeroCCASTArena *arena, ASTRef root, int indent) {
    if (root == AST_NULL) return;
    ZeroCCASTNode *n = zero_cc_ast_get(arena, root);
    if (!n) return;

    for (int i = 0; i < indent; i++) printf("  ");

    switch (n->tag) {
        case NODE_INT:
            printf("IntLiteral: %ld\n", (long)n->int_val);
            break;
        case NODE_FLOAT:
            printf("FloatLiteral: %f\n", n->float_val);
            break;
        case NODE_STRING:
            printf("StringLiteral: \"%.*s\"\n", (int)n->s.len, n->s.str ? n->s.str : "");
            break;
        case NODE_IDENT:
            printf("Identifier: %.*s\n", (int)n->s.len, n->s.str ? n->s.str : "");
            break;
        case NODE_NULLPTR:
            printf("NullptrLiteral\n");
            break;
        case NODE_BOOL:
            printf("BoolLiteral: %s\n", n->int_val ? "true" : "false");
            break;
        case NODE_UNOP:
            printf("UnaryOp: %s %s\n", op_to_str(n->aux.op), n->flags ? "(postfix)" : "(prefix)");
            zero_cc_ast_dump(arena, n->left, indent + 1);
            break;
        case NODE_BINOP:
            printf("BinaryOp: %s\n", op_to_str(n->aux.op));
            zero_cc_ast_dump(arena, n->left, indent + 1);
            zero_cc_ast_dump(arena, n->right, indent + 1);
            break;
        case NODE_ASSIGN:
            printf("Assign: %s\n", op_to_str(n->aux.op));
            zero_cc_ast_dump(arena, n->left, indent + 1);
            zero_cc_ast_dump(arena, n->right, indent + 1);
            break;
        case NODE_TERNARY:
            printf("TernaryOp (? :)\n");
            zero_cc_ast_dump(arena, n->left, indent + 1);
            zero_cc_ast_dump(arena, n->right, indent + 1);
            zero_cc_ast_dump(arena, n->aux.alt, indent + 1);
            break;
        case NODE_MEMBER:
            printf("MemberAccess: %s%.*s\n", n->flags ? "->" : ".", (int)n->s.len, n->s.str ? n->s.str : "");
            zero_cc_ast_dump(arena, n->left, indent + 1);
            break;
        case NODE_INDEX:
            printf("ArrayIndex\n");
            zero_cc_ast_dump(arena, n->left, indent + 1);
            zero_cc_ast_dump(arena, n->right, indent + 1);
            break;
        case NODE_COMPOUND_LIT:
            printf("CompoundLiteral\n");
            zero_cc_ast_dump(arena, n->left, indent + 1);
            break;
        case NODE_CALL:
            printf("FunctionCall\n");
            zero_cc_ast_dump(arena, n->left, indent + 1);
            ASTRef arg = n->right;
            while (arg != AST_NULL) {
                zero_cc_ast_dump(arena, arg, indent + 2);
                ZeroCCASTNode *arg_node = zero_cc_ast_get(arena, arg);
                arg = arg_node ? arg_node->next : AST_NULL;
            }
            break;
        case NODE_BLOCK:
            printf("Block\n");
            ASTRef stmt = n->left;
            while (stmt != AST_NULL) {
                zero_cc_ast_dump(arena, stmt, indent + 1);
                ZeroCCASTNode *stmt_node = zero_cc_ast_get(arena, stmt);
                stmt = stmt_node ? stmt_node->next : AST_NULL;
            }
            break;
        case NODE_EXPR_STMT:
            printf("ExprStmt\n");
            zero_cc_ast_dump(arena, n->left, indent + 1);
            break;
        case NODE_IF:
            printf("IfStmt\n");
            for (int i = 0; i <= indent; i++) printf("  ");
            printf(".cond:\n");
            zero_cc_ast_dump(arena, n->left, indent + 2);
            for (int i = 0; i <= indent; i++) printf("  ");
            printf(".then:\n");
            zero_cc_ast_dump(arena, n->right, indent + 2);
            if (n->aux.alt != AST_NULL) {
                for (int i = 0; i <= indent; i++) printf("  ");
                printf(".else:\n");
                zero_cc_ast_dump(arena, n->aux.alt, indent + 2);
            }
            break;
        case NODE_WHILE:
            printf("WhileStmt\n");
            for (int i = 0; i <= indent; i++) printf("  ");
            printf(".cond:\n");
            zero_cc_ast_dump(arena, n->left, indent + 2);
            for (int i = 0; i <= indent; i++) printf("  ");
            printf(".body:\n");
            zero_cc_ast_dump(arena, n->right, indent + 2);
            break;
        case NODE_DO_WHILE:
            printf("DoWhileStmt\n");
            for (int i = 0; i <= indent; i++) printf("  ");
            printf(".body:\n");
            zero_cc_ast_dump(arena, n->right, indent + 2);
            for (int i = 0; i <= indent; i++) printf("  ");
            printf(".cond:\n");
            zero_cc_ast_dump(arena, n->left, indent + 2);
            break;
        case NODE_FOR:
            printf("ForStmt\n");
            if (n->left != AST_NULL) {
                for (int i = 0; i <= indent; i++) printf("  ");
                printf(".init:\n");
                zero_cc_ast_dump(arena, n->left, indent + 2);
            }
            if (n->right != AST_NULL) {
                for (int i = 0; i <= indent; i++) printf("  ");
                printf(".cond:\n");
                zero_cc_ast_dump(arena, n->right, indent + 2);
            }
            if (n->aux.step != AST_NULL) {
                for (int i = 0; i <= indent; i++) printf("  ");
                printf(".step:\n");
                zero_cc_ast_dump(arena, n->aux.step, indent + 2);
            }
            if (n->aux.alt != AST_NULL) {
                for (int i = 0; i <= indent; i++) printf("  ");
                printf(".body:\n");
                zero_cc_ast_dump(arena, n->aux.alt, indent + 2);
            }
            break;
        case NODE_SWITCH:
            printf("SwitchStmt\n");
            for (int i = 0; i <= indent; i++) printf("  ");
            printf(".expr:\n");
            zero_cc_ast_dump(arena, n->left, indent + 2);
            for (int i = 0; i <= indent; i++) printf("  ");
            printf(".body:\n");
            zero_cc_ast_dump(arena, n->right, indent + 2);
            break;
        case NODE_CASE:
            printf("CaseStmt\n");
            zero_cc_ast_dump(arena, n->left, indent + 1);
            if (n->right != AST_NULL) {
                zero_cc_ast_dump(arena, n->right, indent + 1);
            }
            break;
        case NODE_DEFAULT:
            printf("DefaultStmt\n");
            if (n->left != AST_NULL) {
                zero_cc_ast_dump(arena, n->left, indent + 1);
            }
            break;
        case NODE_BREAK:
            printf("BreakStmt\n");
            break;
        case NODE_CONTINUE:
            printf("ContinueStmt\n");
            break;
        case NODE_RETURN:
            printf("ReturnStmt\n");
            if (n->left != AST_NULL) {
                zero_cc_ast_dump(arena, n->left, indent + 1);
            }
            break;
        case NODE_DECL:
            printf("Decl: %.*s\n", (int)n->s.len, n->s.str ? n->s.str : "");
            if (n->left != AST_NULL) {
                for (int i = 0; i <= indent; i++) printf("  ");
                printf(".init:\n");
                zero_cc_ast_dump(arena, n->left, indent + 2);
            }
            break;
        case NODE_STATIC_ASSERT:
            printf("StaticAssert\n");
            zero_cc_ast_dump(arena, n->left, indent + 1);
            break;
        default:
            printf("UnknownNode (tag=%d)\n", n->tag);
            break;
    }
}

/* ======================================================================
 *  AST Lowering to AMD64 Machine Code
 * ======================================================================
 */

static void lower_expr(ZeroCCCompiler *c, ZeroCCASTArena *arena, ASTRef ref);

#define MAX_EPILOGUE_PATCHES 256
static size_t epilogue_patches[MAX_EPILOGUE_PATCHES];
static size_t epilogue_patch_count = 0;

/* Loop control flow support */
#define MAX_LOOP_NESTING 16
#define MAX_LOOP_PATCHES 64
static size_t break_patch_pos[MAX_LOOP_NESTING][MAX_LOOP_PATCHES];
static int break_patch_count[MAX_LOOP_NESTING];
static size_t continue_patch_pos[MAX_LOOP_NESTING][MAX_LOOP_PATCHES];
static int continue_patch_count[MAX_LOOP_NESTING];
static size_t loop_start_pos[MAX_LOOP_NESTING];
/* static size_t loop_end_placeholder_pos[MAX_LOOP_NESTING]; */
static int loop_depth = 0;

static void lower_node(ZeroCCCompiler *c, ZeroCCASTArena *arena, ASTRef ref) {
    if (ref == AST_NULL) return;
    ZeroCCASTNode *node = zero_cc_ast_get(arena, ref);
    if (!node) return;
    switch (node->tag) {
        case NODE_BLOCK: {
            ASTRef stmt = node->left;
            while (stmt != AST_NULL) {
                lower_node(c, arena, stmt);
                ZeroCCASTNode *snode = zero_cc_ast_get(arena, stmt);
                stmt = snode ? snode->next : AST_NULL;
            }
            break;
        }
        case NODE_EXPR_STMT:
            lower_expr(c, arena, node->left);
            break;
        case NODE_DECL: {
            // Variable declaration handled during parsing; initializer if present.
            if (node->left != AST_NULL) {
                lower_expr(c, arena, node->left);
                // Store result (in rax) to local variable.
                const char *var = node->s.str;
                ZeroCCSym *sym = zero_cc_find_local(c, var);
                if (sym) {
                    zero_cc_emit_u8(&c->text_section, 0x48);
                    zero_cc_emit_u8(&c->text_section, 0x89);
                    zero_cc_emit_u8(&c->text_section, 0x85);
                    zero_cc_emit_u32(&c->text_section, (uint32_t)(-sym->stack_offset));
                }
            }
            break;
        }
        case NODE_RETURN: {
            if (node->left != AST_NULL) {
                lower_expr(c, arena, node->left);
            }
            // Emit a jump to the function epilogue label.
            // Placeholder 5‑byte relative jump: E9 <rel32>
            zero_cc_emit_u8(&c->text_section, 0xE9);
            size_t patch_pos = c->text_section.length;
            zero_cc_emit_u32(&c->text_section, 0);
            if (epilogue_patch_count < MAX_EPILOGUE_PATCHES) {
                epilogue_patches[epilogue_patch_count++] = patch_pos;
            }
            break;
        }
        case NODE_IF: {
            // Evaluate condition -> result in rax.
            lower_expr(c, arena, node->left);
            // test rax, rax
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0x85);
            zero_cc_emit_u8(&c->text_section, 0xC0);
            // je to else (or end if no else).
            zero_cc_emit_u8(&c->text_section, 0x0F);
            zero_cc_emit_u8(&c->text_section, 0x84);
            uint32_t else_offset_pos = c->text_section.length;
            zero_cc_emit_u32(&c->text_section, 0);

            // then block
            lower_node(c, arena, node->right);
            // jmp end
            zero_cc_emit_u8(&c->text_section, 0xE9);
            uint32_t end_jump_pos = c->text_section.length;
            zero_cc_emit_u32(&c->text_section, 0);

            // else label position – patch.
            size_t else_pos = c->text_section.length;
            uint32_t rel_else = (uint32_t)(else_pos - (else_offset_pos + 4));
            memcpy(c->text_section.data + else_offset_pos, &rel_else, 4);
            if (node->aux.alt != AST_NULL) {
                lower_node(c, arena, node->aux.alt);
            }
            // end label position – patch.
            size_t end_pos = c->text_section.length;
            uint32_t rel_end = (uint32_t)(end_pos - (end_jump_pos + 4));
            memcpy(c->text_section.data + end_jump_pos, &rel_end, 4);
            break;
        }
        case NODE_WHILE: {
            // Enter loop: push depth, init patch lists
            loop_start_pos[loop_depth] = c->text_section.length;
            break_patch_count[loop_depth] = 0;
            continue_patch_count[loop_depth] = 0;
            loop_depth++;

            // condition
            lower_expr(c, arena, node->left);
            // test rax, rax
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0x85);
            zero_cc_emit_u8(&c->text_section, 0xC0);
            // je end
            zero_cc_emit_u8(&c->text_section, 0x0F);
            zero_cc_emit_u8(&c->text_section, 0x84);
            uint32_t end_offset_pos = c->text_section.length;
            zero_cc_emit_u32(&c->text_section, 0);
            // body
            lower_node(c, arena, node->right);
            // jmp start (E9 <rel32>)
            uint32_t back_rel = (uint32_t)(loop_start_pos[loop_depth-1] - (c->text_section.length + 5));
            zero_cc_emit_u8(&c->text_section, 0xE9);
            zero_cc_emit_u32(&c->text_section, back_rel);
            // end label
            size_t end_pos = c->text_section.length;
            uint32_t rel_end = (uint32_t)(end_pos - (end_offset_pos + 4));
            memcpy(c->text_section.data + end_offset_pos, &rel_end, 4);
            // patch break jumps to end of loop
            for (int i = 0; i < break_patch_count[loop_depth-1]; i++) {
                size_t patch = break_patch_pos[loop_depth-1][i];
                uint32_t rel = (uint32_t)(end_pos - (patch + 4));
                memcpy(c->text_section.data + patch, &rel, 4);
            }
            // patch continue jumps to loop start
            for (int i = 0; i < continue_patch_count[loop_depth-1]; i++) {
                size_t patch = continue_patch_pos[loop_depth-1][i];
                uint32_t rel = (uint32_t)(loop_start_pos[loop_depth-1] - (patch + 5));
                memcpy(c->text_section.data + patch, &rel, 4);
            }
            // pop stack
            loop_depth--;
            break;
        }
        case NODE_BREAK: {
            // Emit placeholder jump; patch later to loop end
            if (loop_depth <= 0) {
                // No enclosing loop – ignore or handle error
                break;
            }
            zero_cc_emit_u8(&c->text_section, 0xE9);
            size_t patch = c->text_section.length;
            zero_cc_emit_u32(&c->text_section, 0);
            break_patch_pos[loop_depth-1][break_patch_count[loop_depth-1]++] = patch;
            break;
        }
        case NODE_CONTINUE: {
            // Emit placeholder jump; patch later to loop start
            if (loop_depth <= 0) {
                break;
            }
            zero_cc_emit_u8(&c->text_section, 0xE9);
            size_t patch = c->text_section.length;
            zero_cc_emit_u32(&c->text_section, 0);
            continue_patch_pos[loop_depth-1][continue_patch_count[loop_depth-1]++] = patch;
            break;
        }
        case NODE_SWITCH: {
            // Evaluate switch expression
            lower_expr(c, arena, node->left);
            // Move switch value to rbx
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0x89);
            zero_cc_emit_u8(&c->text_section, 0xD8); // mov rbx, rax
            // Walk through case/default linked list
            ASTRef stmt = node->right;
            // removed unused default_body_pos
            while (stmt != AST_NULL) {
                ZeroCCASTNode *case_node = zero_cc_ast_get(arena, stmt);
                if (!case_node) break;
                if (case_node->tag == NODE_CASE) {
                    // Evaluate case constant
                    lower_expr(c, arena, case_node->left);
                    // cmp rbx, rax
                    zero_cc_emit_u8(&c->text_section, 0x48);
                    zero_cc_emit_u8(&c->text_section, 0x39);
                    zero_cc_emit_u8(&c->text_section, 0xD8);
                    // je placeholder
                    zero_cc_emit_u8(&c->text_section, 0x0F);
                    zero_cc_emit_u8(&c->text_section, 0x84);
                    size_t je_patch = c->text_section.length;
                    zero_cc_emit_u32(&c->text_section, 0);
                    // case body start
                    size_t case_body_start = c->text_section.length;
                    uint32_t rel = (uint32_t)(case_body_start - (je_patch + 4));
                    memcpy(c->text_section.data + je_patch, &rel, 4);
                    if (case_node->right != AST_NULL) {
                        lower_node(c, arena, case_node->right);
                    }
                } else if (case_node->tag == NODE_DEFAULT) {
                    // default case body already emitted
                    if (case_node->right != AST_NULL) {
                        lower_node(c, arena, case_node->right);
                    }
                }
                stmt = case_node->next;
            }
            // execution falls through to default if present
            break;
        }
        case NODE_FOR: {
            if (node->left != AST_NULL) lower_node(c, arena, node->left);
            size_t start_pos = c->text_section.length;
            // condition
            if (node->right != AST_NULL) {
                lower_expr(c, arena, node->right);
                // test rax, rax
                zero_cc_emit_u8(&c->text_section, 0x48);
                zero_cc_emit_u8(&c->text_section, 0x85);
                zero_cc_emit_u8(&c->text_section, 0xC0);
                // je end
                zero_cc_emit_u8(&c->text_section, 0x0F);
                zero_cc_emit_u8(&c->text_section, 0x84);
                uint32_t end_offset_pos = c->text_section.length;
                zero_cc_emit_u32(&c->text_section, 0);
                // body
                if (node->aux.alt != AST_NULL) lower_node(c, arena, node->aux.alt);
                // step
                if (node->aux.step != AST_NULL) lower_node(c, arena, node->aux.step);
                // jmp start
                uint32_t back_rel = (uint32_t)(start_pos - (c->text_section.length + 5));
                zero_cc_emit_u8(&c->text_section, 0xE9);
                zero_cc_emit_u32(&c->text_section, back_rel);
                // end label patch
                size_t end_pos = c->text_section.length;
                uint32_t rel_end = (uint32_t)(end_pos - (end_offset_pos + 4));
                memcpy(c->text_section.data + end_offset_pos, &rel_end, 4);
            } else {
                // No condition -> infinite loop.
                if (node->aux.alt != AST_NULL) lower_node(c, arena, node->aux.alt);
                if (node->aux.step != AST_NULL) lower_node(c, arena, node->aux.step);
                // jmp start
                uint32_t back_rel = (uint32_t)(start_pos - (c->text_section.length + 5));
                zero_cc_emit_u8(&c->text_section, 0xE9);
                zero_cc_emit_u32(&c->text_section, back_rel);
            }
            break;
        }

    }
}

static void lower_expr(ZeroCCCompiler *c, ZeroCCASTArena *arena, ASTRef ref) {
    if (ref == AST_NULL) return;
    ZeroCCASTNode *node = zero_cc_ast_get(arena, ref);
    if (!node) return;
    switch (node->tag) {
        case NODE_INT:
            // mov rax, imm64
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0xB8);
            zero_cc_emit_u64(&c->text_section, (uint64_t)node->int_val);
            break;
        case NODE_BOOL:
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0xB8);
            zero_cc_emit_u64(&c->text_section, (uint64_t)node->int_val);
            break;
        case NODE_NULLPTR:
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0x31);
            zero_cc_emit_u8(&c->text_section, 0xC0); // xor rax, rax
            break;
        case NODE_IDENT: {
            const char *var = node->s.str;
            ZeroCCSym *sym = zero_cc_find_local(c, var);
            if (sym) {
                // mov rax, [rbp - offset]
                zero_cc_emit_u8(&c->text_section, 0x48);
                zero_cc_emit_u8(&c->text_section, 0x8B);
                zero_cc_emit_u8(&c->text_section, 0x85);
                zero_cc_emit_u32(&c->text_section, (uint32_t)(-sym->stack_offset));
            }
            break;
        }
        case NODE_CALL: {
            // Evaluate arguments left to right and push them to stack
            ASTRef arg = node->right;
            int arg_count = 0;
            while (arg != AST_NULL) {
                lower_expr(c, arena, arg);
                zero_cc_emit_u8(&c->text_section, 0x50); // push rax
                arg_count++;
                ZeroCCASTNode *arg_node = zero_cc_ast_get(arena, arg);
                arg = arg_node ? arg_node->next : AST_NULL;
            }

            // Pop into AMD64 argument registers
            static const uint8_t arg_regs[6][3] = {
                {0x48, 0x89, 0xC7}, /* mov rdi, rax */
                {0x48, 0x89, 0xC6}, /* mov rsi, rax */
                {0x48, 0x89, 0xC2}, /* mov rdx, rax */
                {0x48, 0x89, 0xC1}, /* mov rcx, rax */
                {0x49, 0x89, 0xC0}, /* mov r8,  rax */
                {0x49, 0x89, 0xC1}  /* mov r9,  rax */
            };
            int reg_args = (arg_count > 6) ? 6 : arg_count;
            for (int i = reg_args - 1; i >= 0; i--) {
                zero_cc_emit_u8(&c->text_section, 0x58); /* pop rax */
                zero_cc_emit_u8(&c->text_section, arg_regs[i][0]);
                zero_cc_emit_u8(&c->text_section, arg_regs[i][1]);
                zero_cc_emit_u8(&c->text_section, arg_regs[i][2]);
            }
            // Clear al for ABI variadic compatibility
            zero_cc_emit_u8(&c->text_section, 0x31);
            zero_cc_emit_u8(&c->text_section, 0xC0);

            // Callee is left child
            ZeroCCASTNode *callee = zero_cc_ast_get(arena, node->left);
            if (callee && callee->tag == NODE_IDENT) {
                const char *name = callee->s.str;
                ZeroCCSym *sym = zero_cc_find_local(c, name);
                if (sym) {
                    // Indirect call: mov r11, [rbp - offset]; call r11
                    zero_cc_emit_u8(&c->text_section, 0x4C);
                    zero_cc_emit_u8(&c->text_section, 0x8B);
                    zero_cc_emit_u8(&c->text_section, 0x9D);
                    zero_cc_emit_u32(&c->text_section, (uint32_t)(-sym->stack_offset));
                    zero_cc_emit_u8(&c->text_section, 0x41);
                    zero_cc_emit_u8(&c->text_section, 0xFF);
                    zero_cc_emit_u8(&c->text_section, 0xD3);
                } else {
                    ZeroCCFunction *fn = zero_cc_find_function(c, name);
                    void *host_fn = zero_cc_resolve_symbol(name);
                    if (fn) {
                        zero_cc_emit_u8(&c->text_section, 0xE8);
                        size_t patch_pos = c->text_section.length;
                        int32_t rel = (int32_t)(fn->text_offset - (patch_pos + 4));
                        zero_cc_emit_u32(&c->text_section, (uint32_t)rel);
                        if (c->reloc_count < 512) {
                            ZeroCCReloc *r = &c->relocs[c->reloc_count++];
                            r->text_patch_offset = patch_pos;
                            snprintf(r->target_symbol, sizeof(r->target_symbol), "%s", name);
                            r->is_call = true;
                        }
                    } else if (host_fn) {
                        zero_cc_emit_u8(&c->text_section, 0x49);
                        zero_cc_emit_u8(&c->text_section, 0xBB);
                        zero_cc_emit_u64(&c->text_section, (uint64_t)host_fn);
                        zero_cc_emit_u8(&c->text_section, 0x41);
                        zero_cc_emit_u8(&c->text_section, 0xFF);
                        zero_cc_emit_u8(&c->text_section, 0xD3);
                    } else {
                        zero_cc_emit_u8(&c->text_section, 0xE8);
                        size_t patch_pos = c->text_section.length;
                        zero_cc_emit_u32(&c->text_section, 0);
                        if (c->reloc_count < 512) {
                            ZeroCCReloc *r = &c->relocs[c->reloc_count++];
                            r->text_patch_offset = patch_pos;
                            snprintf(r->target_symbol, sizeof(r->target_symbol), "%s", name);
                            r->is_call = true;
                        }
                    }
                }
            }
            break;
        }
        case NODE_COMPOUND_LIT:
            // Compound literal: evaluate first initializer expression
            if (node->left != AST_NULL) {
                lower_expr(c, arena, node->left);
            }
            break;
        case NODE_ASSIGN: {
            // Right-hand side evaluated into rax
            lower_expr(c, arena, node->right);
            // Left-hand side variable target
            ZeroCCASTNode *target = zero_cc_ast_get(arena, node->left);
            if (target && target->tag == NODE_IDENT) {
                const char *var = target->s.str;
                ZeroCCSym *sym = zero_cc_find_local(c, var);
                if (sym) {
                    // mov [rbp - offset], rax
                    zero_cc_emit_u8(&c->text_section, 0x48);
                    zero_cc_emit_u8(&c->text_section, 0x89);
                    zero_cc_emit_u8(&c->text_section, 0x85);
                    zero_cc_emit_u32(&c->text_section, (uint32_t)(-sym->stack_offset));
                }
            }
            break;
        }
        case NODE_BINOP: {
            // Evaluate left operand, push to stack.
            lower_expr(c, arena, node->left);
            zero_cc_emit_u8(&c->text_section, 0x50); // push rax
            // Evaluate right operand.
            lower_expr(c, arena, node->right);
            zero_cc_emit_u8(&c->text_section, 0x59); // pop rcx (left operand)
            // Left is now in rcx, right is in rax.
            // Perform operation based on opcode.
            switch (node->aux.op) {
                case TK_PLUS:
                    // add rax, rcx
                    zero_cc_emit_u8(&c->text_section, 0x48);
                    zero_cc_emit_u8(&c->text_section, 0x01);
                    zero_cc_emit_u8(&c->text_section, 0xC8);
                    break;
                case TK_MINUS:
                    // sub rcx, rax ; mov rax, rcx
                    zero_cc_emit_u8(&c->text_section, 0x48);
                    zero_cc_emit_u8(&c->text_section, 0x29);
                    zero_cc_emit_u8(&c->text_section, 0xC1);
                    zero_cc_emit_u8(&c->text_section, 0x48);
                    zero_cc_emit_u8(&c->text_section, 0x89);
                    zero_cc_emit_u8(&c->text_section, 0xC8);
                    break;
                case TK_STAR:
                    // imul rax, rcx
                    zero_cc_emit_u8(&c->text_section, 0x48);
                    zero_cc_emit_u8(&c->text_section, 0x0F);
                    zero_cc_emit_u8(&c->text_section, 0xAF);
                    zero_cc_emit_u8(&c->text_section, 0xC1);
                    break;
                case TK_SLASH:
                    // rcx (left) / rax (right) -> quotient in rax
                    zero_cc_emit_u8(&c->text_section, 0x53);       // push rbx
                    zero_cc_emit_u8(&c->text_section, 0x48);
                    zero_cc_emit_u8(&c->text_section, 0x89);
                    zero_cc_emit_u8(&c->text_section, 0xC3);       // mov rbx, rax
                    zero_cc_emit_u8(&c->text_section, 0x48);
                    zero_cc_emit_u8(&c->text_section, 0x89);
                    zero_cc_emit_u8(&c->text_section, 0xC8);       // mov rax, rcx
                    zero_cc_emit_u8(&c->text_section, 0x48);
                    zero_cc_emit_u8(&c->text_section, 0x99);       // cqo (sign extend rax into rdx:rax)
                    zero_cc_emit_u8(&c->text_section, 0x48);
                    zero_cc_emit_u8(&c->text_section, 0xF7);
                    zero_cc_emit_u8(&c->text_section, 0xFB);       // idiv rbx
                    zero_cc_emit_u8(&c->text_section, 0x5B);       // pop rbx
                    break;
                case TK_PERCENT:
                    // rcx (left) % rax (right) -> remainder in rdx, move to rax
                    zero_cc_emit_u8(&c->text_section, 0x53);       // push rbx
                    zero_cc_emit_u8(&c->text_section, 0x48);
                    zero_cc_emit_u8(&c->text_section, 0x89);
                    zero_cc_emit_u8(&c->text_section, 0xC3);       // mov rbx, rax
                    zero_cc_emit_u8(&c->text_section, 0x48);
                    zero_cc_emit_u8(&c->text_section, 0x89);
                    zero_cc_emit_u8(&c->text_section, 0xC8);       // mov rax, rcx
                    zero_cc_emit_u8(&c->text_section, 0x48);
                    zero_cc_emit_u8(&c->text_section, 0x99);       // cqo
                    zero_cc_emit_u8(&c->text_section, 0x48);
                    zero_cc_emit_u8(&c->text_section, 0xF7);
                    zero_cc_emit_u8(&c->text_section, 0xFB);       // idiv rbx
                    zero_cc_emit_u8(&c->text_section, 0x48);
                    zero_cc_emit_u8(&c->text_section, 0x89);
                    zero_cc_emit_u8(&c->text_section, 0xD0);       // mov rax, rdx
                    zero_cc_emit_u8(&c->text_section, 0x5B);       // pop rbx
                    break;
                case TK_AMP:
                    // and rax, rcx
                    zero_cc_emit_u8(&c->text_section, 0x48);
                    zero_cc_emit_u8(&c->text_section, 0x21);
                    zero_cc_emit_u8(&c->text_section, 0xC8);
                    break;
                case TK_PIPE:
                    // or rax, rcx
                    zero_cc_emit_u8(&c->text_section, 0x48);
                    zero_cc_emit_u8(&c->text_section, 0x09);
                    zero_cc_emit_u8(&c->text_section, 0xC8);
                    break;
                case TK_CARET:
                    // xor rax, rcx
                    zero_cc_emit_u8(&c->text_section, 0x48);
                    zero_cc_emit_u8(&c->text_section, 0x31);
                    zero_cc_emit_u8(&c->text_section, 0xC8);
                    break;
                case TK_SHL:
                    // rcx (left) << rax (right: count in cl)
                    // xchg rax, rcx ; shl rax, cl
                    zero_cc_emit_u8(&c->text_section, 0x48);
                    zero_cc_emit_u8(&c->text_section, 0x87);
                    zero_cc_emit_u8(&c->text_section, 0xC8);       // xchg rax, rcx
                    zero_cc_emit_u8(&c->text_section, 0x48);
                    zero_cc_emit_u8(&c->text_section, 0xD3);
                    zero_cc_emit_u8(&c->text_section, 0xE0);       // shl rax, cl
                    break;
                case TK_SHR:
                    // rcx (left) >> rax (right: count in cl)
                    // xchg rax, rcx ; sar rax, cl
                    zero_cc_emit_u8(&c->text_section, 0x48);
                    zero_cc_emit_u8(&c->text_section, 0x87);
                    zero_cc_emit_u8(&c->text_section, 0xC8);       // xchg rax, rcx
                    zero_cc_emit_u8(&c->text_section, 0x48);
                    zero_cc_emit_u8(&c->text_section, 0xD3);
                    zero_cc_emit_u8(&c->text_section, 0xF8);       // sar rax, cl
                    break;
                case TK_EQ:
                case TK_NE:
                case TK_LT:
                case TK_GT:
                case TK_LE:
                case TK_GE:
                    // cmp rcx, rax (compare left with right)
                    zero_cc_emit_u8(&c->text_section, 0x48);
                    zero_cc_emit_u8(&c->text_section, 0x39);
                    zero_cc_emit_u8(&c->text_section, 0xC1);
                    // setcc al
                    zero_cc_emit_u8(&c->text_section, 0x0F);
                    if (node->aux.op == TK_EQ) zero_cc_emit_u8(&c->text_section, 0x94);      // sete al
                    else if (node->aux.op == TK_NE) zero_cc_emit_u8(&c->text_section, 0x95); // setne al
                    else if (node->aux.op == TK_LT) zero_cc_emit_u8(&c->text_section, 0x9C); // setl al
                    else if (node->aux.op == TK_GE) zero_cc_emit_u8(&c->text_section, 0x9D); // setge al
                    else if (node->aux.op == TK_LE) zero_cc_emit_u8(&c->text_section, 0x9E); // setle al
                    else if (node->aux.op == TK_GT) zero_cc_emit_u8(&c->text_section, 0x9F); // setg al
                    zero_cc_emit_u8(&c->text_section, 0xC0); // ModR/M for AL register
                    // movzx rax, al
                    zero_cc_emit_u8(&c->text_section, 0x48);
                    zero_cc_emit_u8(&c->text_section, 0x0F);
                    zero_cc_emit_u8(&c->text_section, 0xB6);
                    zero_cc_emit_u8(&c->text_section, 0xC0);
                    break;
                default:
                    // Unsupported binary operator – emit zero.
                    zero_cc_emit_u8(&c->text_section, 0x48);
                    zero_cc_emit_u8(&c->text_section, 0xB8);
                    zero_cc_emit_u64(&c->text_section, 0);
                    break;
            }
            break;
        }
        case NODE_UNOP: {
            if (node->aux.op == TK_AMP) {
                // Address-of operator &var
                ZeroCCASTNode *operand = zero_cc_ast_get(arena, node->left);
                if (operand && operand->tag == NODE_IDENT) {
                    const char *var = operand->s.str;
                    ZeroCCSym *sym = zero_cc_find_local(c, var);
                    if (sym) {
                        // lea rax, [rbp - offset]
                        zero_cc_emit_u8(&c->text_section, 0x48);
                        zero_cc_emit_u8(&c->text_section, 0x8D);
                        zero_cc_emit_u8(&c->text_section, 0x85);
                        zero_cc_emit_u32(&c->text_section, (uint32_t)(-sym->stack_offset));
                    }
                }
            } else if (node->aux.op == TK_STAR) {
                // Dereference operator *ptr
                lower_expr(c, arena, node->left);
                // mov rax, [rax]
                zero_cc_emit_u8(&c->text_section, 0x48);
                zero_cc_emit_u8(&c->text_section, 0x8B);
                zero_cc_emit_u8(&c->text_section, 0x00);
            } else if (node->aux.op == TK_MINUS) {
                // Negation -x
                lower_expr(c, arena, node->left);
                // neg rax
                zero_cc_emit_u8(&c->text_section, 0x48);
                zero_cc_emit_u8(&c->text_section, 0xF7);
                zero_cc_emit_u8(&c->text_section, 0xD8);
            } else if (node->aux.op == TK_BANG) {
                // Logical NOT !x
                lower_expr(c, arena, node->left);
                // test rax, rax; sete al; movzx rax, al
                zero_cc_emit_u8(&c->text_section, 0x48);
                zero_cc_emit_u8(&c->text_section, 0x85);
                zero_cc_emit_u8(&c->text_section, 0xC0);
                zero_cc_emit_u8(&c->text_section, 0x0F);
                zero_cc_emit_u8(&c->text_section, 0x94);
                zero_cc_emit_u8(&c->text_section, 0xC0);
                zero_cc_emit_u8(&c->text_section, 0x48);
                zero_cc_emit_u8(&c->text_section, 0x0F);
                zero_cc_emit_u8(&c->text_section, 0xB6);
                zero_cc_emit_u8(&c->text_section, 0xC0);
            } else if (node->aux.op == TK_TILDE) {
                // Bitwise NOT ~x
                lower_expr(c, arena, node->left);
                // not rax
                zero_cc_emit_u8(&c->text_section, 0x48);
                zero_cc_emit_u8(&c->text_section, 0xF7);
                zero_cc_emit_u8(&c->text_section, 0xD0);
            } else if (node->flags == 0 && (node->aux.op == TK_INC || node->aux.op == TK_DEC)) {
                // Handle prefix ++/-- on identifiers.
                ZeroCCASTNode *operand = zero_cc_ast_get(arena, node->left);
                if (operand && operand->tag == NODE_IDENT) {
                    const char *var = operand->s.str;
                    ZeroCCSym *sym = zero_cc_find_local(c, var);
                    if (sym) {
                        // Load current value.
                        zero_cc_emit_u8(&c->text_section, 0x48);
                        zero_cc_emit_u8(&c->text_section, 0x8B);
                        zero_cc_emit_u8(&c->text_section, 0x85);
                        zero_cc_emit_u32(&c->text_section, (uint32_t)(-sym->stack_offset));
                        // inc or dec rax.
                        if (node->aux.op == TK_INC) {
                            zero_cc_emit_u8(&c->text_section, 0x48);
                            zero_cc_emit_u8(&c->text_section, 0xFF);
                            zero_cc_emit_u8(&c->text_section, 0xC0); // inc rax
                        } else {
                            zero_cc_emit_u8(&c->text_section, 0x48);
                            zero_cc_emit_u8(&c->text_section, 0xFF);
                            zero_cc_emit_u8(&c->text_section, 0xC8); // dec rax
                        }
                        // Store back.
                        zero_cc_emit_u8(&c->text_section, 0x48);
                        zero_cc_emit_u8(&c->text_section, 0x89);
                        zero_cc_emit_u8(&c->text_section, 0x85);
                        zero_cc_emit_u32(&c->text_section, (uint32_t)(-sym->stack_offset));
                    }
                }
            }
            break;
        }
        default:
            // Emit zero for other expression types (placeholder).
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0xB8);
            zero_cc_emit_u64(&c->text_section, 0);
            break;
    }
}

void zero_cc_lower_ast(ZeroCCCompiler *c, ZeroCCASTArena *arena, ASTRef root) {
    epilogue_patch_count = 0;
    lower_node(c, arena, root);
    // Backpatch all return epilogue jumps to the current offset (which will immediately precede epilogue)
    size_t epilogue_target = c->text_section.length;
    for (size_t i = 0; i < epilogue_patch_count; i++) {
        size_t patch_pos = epilogue_patches[i];
        int32_t rel = (int32_t)(epilogue_target - (patch_pos + 4));
        memcpy(c->text_section.data + patch_pos, &rel, 4);
    }
    epilogue_patch_count = 0;
}
