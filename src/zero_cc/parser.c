#include "parser.h"
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

    if (tok->type == TK_LPAREN) {
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
        default:
            printf("UnknownNode (tag=%d)\n", n->tag);
            break;
    }
}
