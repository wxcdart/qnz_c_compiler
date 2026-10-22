#ifndef ZERO_CC_PARSER_AST_EXPR_H
#define ZERO_CC_PARSER_AST_EXPR_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "../types/types.h"
#include "../types/global_vars.h"
#include "parser.h"
#include "const_eval.h"
#include "../jit/type_info.h"

/* Forward declaration for statement parsing */
ASTRef zero_cc_parse_stmt_ast(ZeroCCCompiler *c, ZeroCCASTArena *arena, SemContext *ctx);

/* Helper: Check if token is a primary expression */
static inline bool is_primary_token(ZeroCCTokType t) {
    return (t == TK_NUM || t == TK_CHAR_LIT || t == TK_FLOAT || t == TK_STRING ||
            t == TK_NULLPTR || t == TK_TRUE || t == TK_FALSE ||
            t == TK_IDENT || t == TK_GENERIC || t == TK_LPAREN);
}

/* Helper: Check if token is an assignment operator */
static inline bool is_assign_op(ZeroCCTokType t) {
    return (t == TK_ASSIGN || t == TK_ADD_ASSIGN || t == TK_SUB_ASSIGN ||
            t == TK_MUL_ASSIGN || t == TK_DIV_ASSIGN || t == TK_MOD_ASSIGN ||
            t == TK_AND_ASSIGN || t == TK_OR_ASSIGN || t == TK_XOR_ASSIGN ||
            t == TK_SHL_ASSIGN || t == TK_SHR_ASSIGN);
}

/* Forward declarations */
static bool is_type_specifier(ZeroCCCompiler *c, ZeroCCTokType t);
static bool is_type_specifier_at(ZeroCCCompiler *c, size_t pos, ZeroCCTokType t);
static ASTRef parse_expr_ast_internal(ZeroCCCompiler *c, ZeroCCASTArena *arena);
static ASTRef parse_assignment_ast(ZeroCCCompiler *c, ZeroCCASTArena *arena);
static ASTRef parse_ternary_ast(ZeroCCCompiler *c, ZeroCCASTArena *arena);
static ASTRef parse_unary_ast(ZeroCCCompiler *c, ZeroCCASTArena *arena);

static ASTRef parse_brace_init_list_ast(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCType lit_type, size_t *out_elem_count) {
    if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LBRACE) {
        c->token_pos++; /* consume '{' */
    }
    ASTRef head_expr = AST_NULL;
    ASTRef last_expr = AST_NULL;
    size_t elem_count = 0;

    while (c->token_pos < c->token_count && c->tokens[c->token_pos].type != TK_RBRACE) {
        int32_t designated_member_idx = -1;
        /* Struct/union designated initializer: .field = val */
        if (c->tokens[c->token_pos].type == TK_DOT &&
            c->token_pos + 1 < c->token_count &&
            c->tokens[c->token_pos + 1].type == TK_IDENT) {
            const char *field_name = c->tokens[c->token_pos + 1].str_val;
            c->token_pos += 2;
            if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_ASSIGN) {
                c->token_pos++;
            }
            if (lit_type.tag == TYPE_STRUCT || lit_type.tag == TYPE_UNION) {
                for (size_t mi = 0; mi < lit_type.member_count; mi++) {
                    if (strcmp(lit_type.members[mi].name, field_name) == 0) {
                        designated_member_idx = (int32_t)mi;
                        /* C99: flexible array members cannot be initialized */
                        if (lit_type.members[mi].is_flexible) {
                            diag_report_error_at_token(c, "initialization of flexible array member '%s'", field_name);
                            return AST_NULL;
                        }
                        break;
                    }
                }
            }
        } else if (c->tokens[c->token_pos].type == TK_LBRACKET) {
            /* Array designated initializer: [idx] = val */
            c->token_pos++; /* consume '[' */
            if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_NUM) {
                designated_member_idx = (int32_t)c->tokens[c->token_pos].num_val;
                /* Bounds check for fixed-size arrays */
                if (lit_type.tag == TYPE_ARRAY && lit_type.array_len > 0 &&
                    (size_t)designated_member_idx >= lit_type.array_len) {
                    diag_report_error_at_token(c,
                        "array initializer index %d exceeds array bound %zu",
                        designated_member_idx, lit_type.array_len);
                    return AST_NULL;
                }
                c->token_pos++;
            }
            if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RBRACKET) {
                c->token_pos++;
            }
            if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_ASSIGN) {
                c->token_pos++;
            }
        }

        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LBRACE) {
            size_t sub_count = 0;
            ASTRef sub_head = parse_brace_init_list_ast(c, arena, lit_type, &sub_count);
            if (sub_head != AST_NULL) {
                if (head_expr == AST_NULL) {
                    head_expr = sub_head;
                } else if (last_expr != AST_NULL) {
                    ZeroCCASTNode *last_node = zero_cc_ast_get(arena, last_expr);
                    if (last_node) last_node->next = sub_head;
                }
                ASTRef cur_sub = sub_head;
                while (cur_sub != AST_NULL) {
                    last_expr = cur_sub;
                    ZeroCCASTNode *sn = zero_cc_ast_get(arena, cur_sub);
                    cur_sub = sn ? sn->next : AST_NULL;
                }
                elem_count += sub_count;
            }
        } else {
            ASTRef elem = parse_assignment_ast(c, arena);
            if (elem != AST_NULL) {
                ZeroCCASTNode *en = zero_cc_ast_get(arena, elem);
                if (en && designated_member_idx >= 0) {
                    en->desig_idx = (uint32_t)designated_member_idx;
                    en->flags |= NODE_FLAG_DESIGNATED;
                }
                if (head_expr == AST_NULL) {
                    head_expr = elem;
                } else if (last_expr != AST_NULL) {
                    ZeroCCASTNode *last_node = zero_cc_ast_get(arena, last_expr);
                    if (last_node) last_node->next = elem;
                }
                last_expr = elem;
                elem_count++;
            } else {
                break;
            }
        }

        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_COMMA) {
            c->token_pos++;
        }
    }
    if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RBRACE) {
        c->token_pos++;
    }
    if (out_elem_count) *out_elem_count = elem_count;
    return head_expr;
}

/* Primary Expressions: literals, identifiers, parenthesized expressions */
static ASTRef parse_primary_ast(ZeroCCCompiler *c, ZeroCCASTArena *arena) {
    if (c->token_pos >= c->token_count) return AST_NULL;
    ZeroCCToken *tok = &c->tokens[c->token_pos];

    /* Switch for simple primary tokens */
    switch (tok->type) {
        case TK_NUM: {
            c->token_pos++;
            ASTRef ref = zero_cc_ast_alloc_node(arena, NODE_INT);
            ZeroCCASTNode *n = zero_cc_ast_get(arena, ref);
            n->line = (uint32_t)tok->line;
            n->col = (uint32_t)tok->col;
            n->int_val = tok->num_val;
            return ref;
        }

        case TK_CHAR_LIT: {
            /* Character constant: type int in C, value from the lexer */
            c->token_pos++;
            ASTRef ref = zero_cc_ast_alloc_node(arena, NODE_INT);
            ZeroCCASTNode *n = zero_cc_ast_get(arena, ref);
            n->line = (uint32_t)tok->line;
            n->col = (uint32_t)tok->col;
            n->int_val = tok->num_val;
            return ref;
        }

        case TK_FLOAT: {
            c->token_pos++;
            ASTRef ref = zero_cc_ast_alloc_node(arena, NODE_FLOAT);
            ZeroCCASTNode *n = zero_cc_ast_get(arena, ref);
            n->line = (uint32_t)tok->line;
            n->col = (uint32_t)tok->col;
            n->float_val = tok->float_val;
            return ref;
        }

        case TK_STRING: {
            c->token_pos++;
            ASTRef ref = zero_cc_ast_alloc_node(arena, NODE_STRING);
            ZeroCCASTNode *n = zero_cc_ast_get(arena, ref);
            n->line = (uint32_t)tok->line;
            n->col = (uint32_t)tok->col;
            const char *s = tok->allocated_str ? tok->allocated_str : tok->str_val;
            /* Intern short strings (allocated_str is already stable heap) */
            n->s.str = (tok->allocated_str) ? s : zero_cc_intern_string(c, s);
            n->s.len = strlen(n->s.str);
            return ref;
        }

        case TK_NULLPTR: {
            c->token_pos++;
            ASTRef ref = zero_cc_ast_alloc_node(arena, NODE_NULLPTR);
            ZeroCCASTNode *n = zero_cc_ast_get(arena, ref);
            n->line = (uint32_t)tok->line;
            n->col = (uint32_t)tok->col;
            return ref;
        }

        case TK_TRUE:
        case TK_FALSE: {
            c->token_pos++;
            ASTRef ref = zero_cc_ast_alloc_node(arena, NODE_BOOL);
            ZeroCCASTNode *n = zero_cc_ast_get(arena, ref);
            n->line = (uint32_t)tok->line;
            n->col = (uint32_t)tok->col;
            n->int_val = (tok->type == TK_TRUE) ? 1 : 0;
            return ref;
        }

        case TK_IDENT: {
            c->token_pos++;
            /* Check for __builtin_va_arg(ap, type) - special variadic argument extraction */
            if (strcmp(tok->str_val, "__builtin_va_arg") == 0) {
                c->token_pos++; /* skip __builtin_va_arg */
                if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LPAREN) c->token_pos++; /* skip '(' */
                
                ASTRef va_arg_ref = zero_cc_ast_alloc_node(arena, NODE_VA_ARG);
                ZeroCCASTNode *va_arg_node = zero_cc_ast_get(arena, va_arg_ref);
                
                /* Parse the va_list argument */
                ASTRef ap_ref = parse_assignment_ast(c, arena);
                va_arg_node = zero_cc_ast_get(arena, va_arg_ref);
                va_arg_node->left = ap_ref;
                
                if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_COMMA) c->token_pos++; /* skip ',' */
                
                /* Parse the type (e.g., int, double, char*, StructName) */
                ZeroCCType va_t = zero_cc_parse_type_name(c);
                va_arg_node = zero_cc_ast_get(arena, va_arg_ref);
                va_arg_node->aux.extra = 0;
                if (c->sem_ctx && c->sem_ctx->perm_arena) {
                    TypeInfo *va_ti = zero_cc_type_to_typeinfo(c->sem_ctx->perm_arena, va_t);
                    size_t off = arena_index_of(c->sem_ctx->perm_arena, va_ti);
                    if (off != (size_t)-1) {
                        va_arg_node->aux.step = (uint32_t)(off + 1);
                    }
                }
                
                if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RPAREN) c->token_pos++; /* skip ')' */
                return va_arg_ref;
            }
            
            /* Check for __builtin_va_start(ap, last) - special variadic start initialization */
            if (strcmp(tok->str_val, "__builtin_va_start") == 0) {
                c->token_pos++; /* skip __builtin_va_start */
                if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LPAREN) c->token_pos++; /* skip '(' */
                
                ASTRef va_start_ref = zero_cc_ast_alloc_node(arena, NODE_VA_START);
                ZeroCCASTNode *va_start_node = zero_cc_ast_get(arena, va_start_ref);
                
                /* Parse the va_list argument (ap) */
                va_start_node->left = parse_assignment_ast(c, arena);
                
                if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_COMMA) c->token_pos++; /* skip ',' */
                
                /* Parse the last fixed argument */
                va_start_node->right = parse_assignment_ast(c, arena);
                
                if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RPAREN) c->token_pos++; /* skip ')' */
                
                return va_start_ref;
            }

            ZeroCCEnumConst *ec = zero_cc_find_enum_const(c, tok->str_val);
            if (ec) {
                ASTRef ref = zero_cc_ast_alloc_node(arena, NODE_INT);
                ZeroCCASTNode *n = zero_cc_ast_get(arena, ref);
                n->line = (uint32_t)tok->line;
                n->col = (uint32_t)tok->col;
                n->int_val = ec->value;
                return ref;
            }
            
            ASTRef ref = zero_cc_ast_alloc_node(arena, NODE_IDENT);
            ZeroCCASTNode *n = zero_cc_ast_get(arena, ref);
            n->line = (uint32_t)tok->line;
            n->col = (uint32_t)tok->col;
            /* Intern: tok->str_val dies when the token buffer grows */
            n->s.str = zero_cc_intern_string(c, tok->str_val);
            n->s.len = strlen(n->s.str);
            return ref;
        }

        /* Fall through to handle complex cases */
        case TK_GENERIC:
        case TK_LPAREN:
            break;

        default:
            return AST_NULL;
    }

    /* Complex cases requiring lookahead and nested parsing */

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
                    selected_expr = parse_assignment_ast(c, arena);
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
        size_t end_pos = c->token_pos;
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RPAREN) {
            c->token_pos++;
            end_pos = c->token_pos;
        }
        if (matched) return selected_expr;
        if (default_expr_start > 0) {
            c->token_pos = default_expr_start;
            ASTRef def_expr = parse_assignment_ast(c, arena);
            c->token_pos = end_pos;
            return def_expr;
        }
        return AST_NULL;
    }

    /* Parenthesized expressions and casts */
    if (tok->type == TK_LPAREN) {
        size_t cast_start = c->token_pos;
        if (c->token_pos + 1 < c->token_count &&
            is_type_specifier_at(c, c->token_pos + 1, c->tokens[c->token_pos + 1].type)) {
            size_t type_pos = cast_start + 1;
            ZeroCCType lit_type = zero_cc_parse_type_name_at_pos(c, &type_pos);
            if (type_pos < c->token_count && c->tokens[type_pos].type == TK_RPAREN) {
                size_t after_rparen = type_pos + 1;
                if (after_rparen < c->token_count && c->tokens[after_rparen].type == TK_LBRACE) {
                    /* Compound literal: (type){ ... } */
                    c->token_pos = after_rparen; /* at '{' */
                    size_t elem_count = 0;
                    ASTRef head_expr = parse_brace_init_list_ast(c, arena, lit_type, &elem_count);

                    if (lit_type.tag == TYPE_ARRAY) {
                        if (lit_type.array_len == 0) {
                            lit_type.array_len = elem_count > 0 ? elem_count : 1;
                        }
                        size_t unit_sz = (lit_type.base && lit_type.base->size > 0) ? lit_type.base->size : 4;
                        lit_type.size = unit_sz * lit_type.array_len;
                        lit_type.align = (lit_type.base && lit_type.base->align > 0) ? lit_type.base->align : 4;
                    } else if (lit_type.tag != TYPE_STRUCT && lit_type.tag != TYPE_UNION) {
                        if (lit_type.size == 0) lit_type.size = 4;
                        if (lit_type.align == 0) lit_type.align = 4;
                    }

                    static uint32_t s_clit_id = 0;
                    char clit_name[48];
                    snprintf(clit_name, sizeof(clit_name), "__clit_%u", s_clit_id++);
                    ZeroCCSym *sym = zero_cc_add_local(c, clit_name, lit_type);

                    ASTRef comp_ref = zero_cc_ast_alloc_node(arena, NODE_COMPOUND_LIT);
                    ZeroCCASTNode *cn = zero_cc_ast_get(arena, comp_ref);
                    cn->left = head_expr;
                    if (sym) {
                        cn->s.str = sym->name;
                        cn->s.len = strlen(sym->name);
                    }
                    if (c->sem_ctx && c->sem_ctx->perm_arena) {
                        TypeInfo *ti = zero_cc_type_to_typeinfo(c->sem_ctx->perm_arena, lit_type);
                        ast_node_set_typeinfo(c, cn, ti);
                    }
                    return comp_ref;
                }
                /* It's a cast: create NODE_CAST with the expression */
                c->token_pos = after_rparen;
                ASTRef cast_ref = zero_cc_ast_alloc_node(arena, NODE_CAST);
                ZeroCCASTNode *cast_node = zero_cc_ast_get(arena, cast_ref);
                if (c->sem_ctx && c->sem_ctx->perm_arena) {
                    TypeInfo *ti = zero_cc_type_to_typeinfo(c->sem_ctx->perm_arena, lit_type);
                    ast_node_set_typeinfo(c, cast_node, ti);
                }
                /* Parse the expression to be cast. C11 6.5.4: the cast operand is
                 * another cast-expression (a unary-expression), NOT a full
                 * assignment-expression. Using parse_unary_ast keeps `(int)x == 1`
                 * grouped as `((int)x) == 1` instead of `(int)(x == 1)`. */
                cast_node->left = parse_unary_ast(c, arena);
                return cast_ref;
            }
            c->token_pos = cast_start;
        }
        if (c->expr_depth >= 150) {
            diag_report_error_at_token(c, "expression nesting depth exceeds parser limit");
            c->had_error = 1;
            c->token_pos++;
            return AST_NULL;
        }

        c->token_pos++;
        c->expr_depth++;
        ASTRef inner = parse_expr_ast_internal(c, arena);
        c->expr_depth--;
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
                size_t arg_start = c->token_pos;
                ASTRef arg = parse_assignment_ast(c, arena);
                if (arg == AST_NULL && c->token_pos == arg_start) {
                    /* No progress: the argument list is malformed (e.g. a
                     * dangling postfix operator).  Report and synchronize
                     * to the matching ')' instead of spinning forever. */
                    diag_report_error_at_token(c, "expected expression in argument list");
                    c->had_error = 1;
                    int paren_depth = 0;
                    while (c->token_pos < c->token_count) {
                        ZeroCCTokType st = c->tokens[c->token_pos].type;
                        if (st == TK_LPAREN) paren_depth++;
                        else if (st == TK_RPAREN) {
                            if (paren_depth == 0) { c->token_pos++; break; }
                            paren_depth--;
                        }
                        c->token_pos++;
                    }
                    break;
                }
                if (arg != AST_NULL) {
                    if (call_node->right == AST_NULL) {
                        call_node->right = arg;
                    } else if (last_arg != AST_NULL) {
                        ZeroCCASTNode *last_node = zero_cc_ast_get(arena, last_arg);
                        last_node->next = arg;
                    }
                    last_arg = arg;
                }
                if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_COMMA) c->token_pos++;
            }
            if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RPAREN) c->token_pos++;
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
                mn->s.str = zero_cc_intern_string(c, c->tokens[c->token_pos].str_val);
                mn->s.len = strlen(mn->s.str);
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

    /* Switch for unary operators */
    switch (t) {
        case TK_SIZEOF: {
            size_t pos = c->token_pos;
            pos++; /* consume TK_SIZEOF */
            if (pos < c->token_count && c->tokens[pos].type == TK_LPAREN) pos++;
            ZeroCCType ty;
            /* Type operand (incl. `*` suffix) needs the type parser; the
             * expression grammar treats `*` as multiplication. */
            if (zero_cc_tok_is_type_start(c, pos)) {
                ty = zero_cc_parse_type_name_at_pos(c, &pos);
            } else {
                size_t id_sz = zero_cc_sizeof_ident_size(c, &pos);
                if (id_sz > 0) {
                    if (pos < c->token_count && c->tokens[pos].type == TK_RPAREN) pos++;
                    c->token_pos = pos;
                    ASTRef ref = zero_cc_ast_alloc_node(arena, NODE_INT);
                    ZeroCCASTNode *n = zero_cc_ast_get(arena, ref);
                    n->int_val = (int64_t)id_sz;
                    return ref;
                }
                ty = zero_cc_type_of_equality(c, &pos);
            }
            if (pos < c->token_count && c->tokens[pos].type == TK_RPAREN) pos++;
            c->token_pos = pos;
            size_t sz = ty.size;
            
            ASTRef ref = zero_cc_ast_alloc_node(arena, NODE_INT);
            ZeroCCASTNode *n = zero_cc_ast_get(arena, ref);
            n->int_val = (int64_t)sz;
            return ref;
        }
        case TK_ALIGNOF: {
            size_t pos = c->token_pos;
            pos++; /* consume TK_ALIGNOF */
            if (pos < c->token_count && c->tokens[pos].type == TK_LPAREN) pos++;
            ZeroCCType ty;
            if (zero_cc_tok_is_type_start(c, pos)) {
                ty = zero_cc_parse_type_name_at_pos(c, &pos);
            } else {
                size_t id_al = zero_cc_sizeof_ident_align(c, &pos);
                if (id_al > 0) {
                    if (pos < c->token_count && c->tokens[pos].type == TK_RPAREN) pos++;
                    c->token_pos = pos;
                    ASTRef ref = zero_cc_ast_alloc_node(arena, NODE_INT);
                    ZeroCCASTNode *n = zero_cc_ast_get(arena, ref);
                    n->int_val = (int64_t)id_al;
                    return ref;
                }
                ty = zero_cc_type_of_equality(c, &pos);
            }
            if (pos < c->token_count && c->tokens[pos].type == TK_RPAREN) pos++;
            c->token_pos = pos;
            size_t align = ty.align ? ty.align : 1;
            
            ASTRef ref = zero_cc_ast_alloc_node(arena, NODE_INT);
            ZeroCCASTNode *n = zero_cc_ast_get(arena, ref);
            n->int_val = (int64_t)align;
            return ref;
        }
        case TK_PLUS:
        case TK_MINUS:
        case TK_BANG:
        case TK_TILDE:
        case TK_STAR:
        case TK_AMP:
        case TK_INC:
        case TK_DEC: {
            c->token_pos++;
            ASTRef operand = parse_unary_ast(c, arena);
            ASTRef un_ref = zero_cc_ast_alloc_node(arena, NODE_UNOP);
            ZeroCCASTNode *un = zero_cc_ast_get(arena, un_ref);
            un->left = operand;
            un->aux.op = (uint32_t)t;
            un->flags = 0; /* prefix */
            return un_ref;
        }
        default:
            break;
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

/* Assignment Expression: lhs op= rhs (right-associative) */
static ASTRef parse_assignment_ast(ZeroCCCompiler *c, ZeroCCASTArena *arena) {
    ASTRef left = parse_ternary_ast(c, arena);
    if (c->token_pos < c->token_count && is_assign_op(c->tokens[c->token_pos].type)) {
        ZeroCCTokType op = c->tokens[c->token_pos].type;
        c->token_pos++;
        ASTRef right = parse_assignment_ast(c, arena);
        ASTRef assign_ref = zero_cc_ast_alloc_node(arena, NODE_ASSIGN);
        ZeroCCASTNode *an = zero_cc_ast_get(arena, assign_ref);
        an->left = left;
        an->right = right;
        an->aux.op = (uint32_t)op;
        return assign_ref;
    }
    return left;
}

/* Comma Expression: lhs, rhs (lowest precedence, left-associative) */
static ASTRef parse_expr_ast_internal(ZeroCCCompiler *c, ZeroCCASTArena *arena) {
    ASTRef left = parse_assignment_ast(c, arena);
    while (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_COMMA) {
        c->token_pos++;
        ASTRef right = parse_assignment_ast(c, arena);
        ASTRef bin_ref = zero_cc_ast_alloc_node(arena, NODE_COMMA);
        ZeroCCASTNode *bn = zero_cc_ast_get(arena, bin_ref);
        bn->left = left;
        bn->right = right;
        bn->aux.op = TK_COMMA;
        left = bin_ref;
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

static bool is_type_specifier_at(ZeroCCCompiler *c, size_t pos, ZeroCCTokType t) {
    if (t == TK_INT || t == TK_CHAR || t == TK_VOID || t == TK_STRUCT || t == TK_UNION || t == TK_ENUM ||
        t == TK_FLOAT_KW || t == TK_DOUBLE_KW || t == TK_BOOL_KW ||
        t == TK_SIGNED || t == TK_UNSIGNED || t == TK_SHORT || t == TK_LONG ||
        t == TK_AUTO || t == TK_ATOMIC ||
        t == TK_CONST || t == TK_VOLATILE || t == TK_RESTRICT ||
        t == TK_TYPEOF || t == TK_TYPEOF_UNQUAL) {
        return true;
    }
    if (t == TK_IDENT && pos < c->token_count) {
        const char *tname = c->tokens[pos].allocated_str ?
                            c->tokens[pos].allocated_str : c->tokens[pos].str_val;
        if (zero_cc_find_typedef(c, tname) != NULL || zero_cc_find_struct(c, tname) != NULL || zero_cc_find_enum(c, tname) != NULL) {
            return true;
        }
    }
    return false;
}

static bool is_type_specifier(ZeroCCCompiler *c, ZeroCCTokType t) {
    return is_type_specifier_at(c, c->token_pos, t);
}


#endif /* ZERO_CC_PARSER_AST_EXPR_H */
