#ifndef ZERO_CC_PARSER_AST_STMT_H
#define ZERO_CC_PARSER_AST_STMT_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "../types_lexer.h"
#include "../parser.h"
#include "../jit/type_info.h"
#include "../semantic/decl.h"

static ASTRef parse_expr_ast_internal(ZeroCCCompiler *c, ZeroCCASTArena *arena);

ASTRef zero_cc_parse_stmt_ast(ZeroCCCompiler *c, ZeroCCASTArena *arena, SemContext *ctx) {
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
            ASTRef s = zero_cc_parse_stmt_ast(c, arena, ctx);
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

        ASTRef then_stmt = zero_cc_parse_stmt_ast(c, arena, ctx);
        ASTRef else_stmt = AST_NULL;

        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_ELSE) {
            c->token_pos++;
            else_stmt = zero_cc_parse_stmt_ast(c, arena, ctx);
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

        ASTRef body = zero_cc_parse_stmt_ast(c, arena, ctx);
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

        ASTRef body = zero_cc_parse_stmt_ast(c, arena, ctx);
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
            init_stmt = zero_cc_parse_stmt_ast(c, arena, ctx);
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
        ASTRef body_stmt = zero_cc_parse_stmt_ast(c, arena, ctx);

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
        ASTRef body = zero_cc_parse_stmt_ast(c, arena, ctx);

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
            stmt = zero_cc_parse_stmt_ast(c, arena, ctx);
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
            stmt = zero_cc_parse_stmt_ast(c, arena, ctx);
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
    /* Parse alignment, storage specifiers & type qualifiers */
    bool is_const = false;
    bool is_volatile = false;
    bool is_atomic = false;

    while (c->token_pos < c->token_count &&
           (c->tokens[c->token_pos].type == TK_ALIGNAS ||
            c->tokens[c->token_pos].type == TK_CONSTEXPR ||
            c->tokens[c->token_pos].type == TK_CONST ||
            c->tokens[c->token_pos].type == TK_VOLATILE ||
            c->tokens[c->token_pos].type == TK_RESTRICT ||
            c->tokens[c->token_pos].type == TK_STATIC ||
            c->tokens[c->token_pos].type == TK_EXTERN ||
            c->tokens[c->token_pos].type == TK_THREAD_LOCAL ||
            c->tokens[c->token_pos].type == TK_REGISTER)) {
        if (c->tokens[c->token_pos].type == TK_CONST || c->tokens[c->token_pos].type == TK_CONSTEXPR) {
            is_const = true;
            c->token_pos++;
        } else if (c->tokens[c->token_pos].type == TK_VOLATILE) {
            is_volatile = true;
            c->token_pos++;
        } else if (c->tokens[c->token_pos].type == TK_RESTRICT) {
            c->token_pos++;
        } else if (c->tokens[c->token_pos].type == TK_ALIGNAS) {
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
        if (type_tok == TK_STRUCT || type_tok == TK_UNION) {
            if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_IDENT) {
                type_start_pos = c->token_pos;
                c->token_pos++;
            }
        }
        /* Check _Atomic(T) or _Atomic T */
        if (type_tok == TK_ATOMIC) {
            is_atomic = true;
            if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LPAREN) {
                c->token_pos++;
                if (c->token_pos < c->token_count && is_type_specifier(c, c->tokens[c->token_pos].type)) {
                    type_tok = c->tokens[c->token_pos].type;
                    type_start_pos = c->token_pos;
                    c->token_pos++;
                }
                if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RPAREN) {
                    c->token_pos++;
                }
            } else if (c->token_pos < c->token_count && is_type_specifier(c, c->tokens[c->token_pos].type)) {
                type_tok = c->tokens[c->token_pos].type;
                type_start_pos = c->token_pos;
                c->token_pos++;
            }
        }
        /* Also allow type _Atomic, const, volatile x; */
        while (c->token_pos < c->token_count && (c->tokens[c->token_pos].type == TK_ATOMIC ||
                                                 c->tokens[c->token_pos].type == TK_CONST ||
                                                 c->tokens[c->token_pos].type == TK_VOLATILE ||
                                                 c->tokens[c->token_pos].type == TK_RESTRICT)) {
            if (c->tokens[c->token_pos].type == TK_ATOMIC) is_atomic = true;
            if (c->tokens[c->token_pos].type == TK_CONST) is_const = true;
            if (c->tokens[c->token_pos].type == TK_VOLATILE) is_volatile = true;
            c->token_pos++;
        }

        /* Pointer asterisks and pointer qualifiers */
        bool is_ptr = false;
        bool ptr_is_const = false;
        bool ptr_is_volatile = false;
        while (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_STAR) {
            is_ptr = true;
            c->token_pos++;
            while (c->token_pos < c->token_count && (c->tokens[c->token_pos].type == TK_CONST ||
                                                     c->tokens[c->token_pos].type == TK_VOLATILE ||
                                                     c->tokens[c->token_pos].type == TK_RESTRICT)) {
                if (c->tokens[c->token_pos].type == TK_CONST) ptr_is_const = true;
                if (c->tokens[c->token_pos].type == TK_VOLATILE) ptr_is_volatile = true;
                c->token_pos++;
            }
        }

        /* Check for identifier name */
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_IDENT) {
            ZeroCCToken *var_tok = &c->tokens[c->token_pos];
            const char *vname = var_tok->allocated_str ? var_tok->allocated_str : var_tok->str_val;
            c->token_pos++;

            /* Check if this is a function declarator */
            bool is_function = false;
            ASTRef param_list = AST_NULL;
            if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LPAREN) {
                is_function = true;
                c->token_pos++; // consume '('
                /* Parse parameter list */
                ASTRef param_head = AST_NULL;
                ASTRef param_tail = AST_NULL;
                bool first_param = true;
                while (c->token_pos < c->token_count) {
                    if (c->tokens[c->token_pos].type == TK_RPAREN) {
                        break;
                    }
                    if (!first_param) {
                        if (c->tokens[c->token_pos].type == TK_COMMA) {
                            c->token_pos++; // consume comma
                        } else {
                            break; // error
                        }
                    }
                    first_param = false;
                    /* Check for void parameter list: void alone means no params */
                    if (c->tokens[c->token_pos].type == TK_VOID) {
                        c->token_pos++;
                        if (c->tokens[c->token_pos].type == TK_RPAREN) {
                            break;
                        }
                    }
                    /* Parse parameter declaration: qualifiers + type specifier + optional pointer + identifier */
                    bool param_is_const = false;
                    bool param_is_volatile = false;
                    while (c->token_pos < c->token_count &&
                           (c->tokens[c->token_pos].type == TK_CONST ||
                            c->tokens[c->token_pos].type == TK_VOLATILE ||
                            c->tokens[c->token_pos].type == TK_RESTRICT)) {
                        if (c->tokens[c->token_pos].type == TK_CONST) param_is_const = true;
                        if (c->tokens[c->token_pos].type == TK_VOLATILE) param_is_volatile = true;
                        c->token_pos++;
                    }
                    ZeroCCTokType param_type_tok = c->tokens[c->token_pos].type;
                    if (!is_type_specifier(c, param_type_tok)) {
                        break; // error
                    }
                    c->token_pos++;
                    while (c->token_pos < c->token_count &&
                           (c->tokens[c->token_pos].type == TK_CONST ||
                            c->tokens[c->token_pos].type == TK_VOLATILE ||
                            c->tokens[c->token_pos].type == TK_RESTRICT)) {
                        if (c->tokens[c->token_pos].type == TK_CONST) param_is_const = true;
                        if (c->tokens[c->token_pos].type == TK_VOLATILE) param_is_volatile = true;
                        c->token_pos++;
                    }
                    bool param_is_ptr = false;
                    bool param_ptr_is_const = false;
                    bool param_ptr_is_volatile = false;
                    while (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_STAR) {
                        param_is_ptr = true;
                        c->token_pos++;
                        while (c->token_pos < c->token_count &&
                               (c->tokens[c->token_pos].type == TK_CONST ||
                                c->tokens[c->token_pos].type == TK_VOLATILE ||
                                c->tokens[c->token_pos].type == TK_RESTRICT)) {
                            if (c->tokens[c->token_pos].type == TK_CONST) param_ptr_is_const = true;
                            if (c->tokens[c->token_pos].type == TK_VOLATILE) param_ptr_is_volatile = true;
                            c->token_pos++;
                        }
                    }
                    if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_IDENT) {
                        const char *pname = c->tokens[c->token_pos].str_val;
                        c->token_pos++;
                        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LBRACKET) {
                            c->token_pos++;
                            int bdepth = 1;
                            while (c->token_pos < c->token_count && bdepth > 0) {
                                if (c->tokens[c->token_pos].type == TK_LBRACKET) bdepth++;
                                else if (c->tokens[c->token_pos].type == TK_RBRACKET) bdepth--;
                                c->token_pos++;
                            }
                            param_is_ptr = true;
                        }
                        /* Create parameter node */
                        ASTRef p_ref = zero_cc_ast_alloc_node(arena, NODE_DECL);
                        ZeroCCASTNode *p_node = zero_cc_ast_get(arena, p_ref);
                        p_node->line = c->tokens[c->token_pos].line; // approximate
                        p_node->col = c->tokens[c->token_pos].col;
                        p_node->s.str = pname;
                        p_node->s.len = strlen(pname);
                        /* Store parameter type info */
                        ZeroCCType ptype;
                        if (param_type_tok == TK_CHAR || param_type_tok == TK_BOOL_KW) ptype = zero_cc_type_char();
                        else if (param_type_tok == TK_SHORT) ptype = zero_cc_type_int16();
                        else if (param_type_tok == TK_INT) ptype = zero_cc_type_int();
                        else if (param_type_tok == TK_FLOAT_KW) ptype = zero_cc_type_float();
                        else if (param_type_tok == TK_DOUBLE_KW) ptype = zero_cc_type_double();
                        else if (param_type_tok == TK_VOID) ptype = zero_cc_type_void();
                        else ptype = zero_cc_type_int64();
                        ptype.is_const = param_is_const;
                        ptype.is_volatile = param_is_volatile;
                        if (param_is_ptr) {
                            ptype = zero_cc_type_ptr(&ptype);
                            ptype.is_const = param_ptr_is_const;
                            ptype.is_volatile = param_ptr_is_volatile;
                        }
                        TypeInfo *ptype_ti = zero_cc_type_to_typeinfo(ctx->perm_arena, ptype);
                        decl_node_set_type(ctx, p_node, ptype_ti);
                        if (param_head == AST_NULL) {
                            param_head = p_ref;
                            param_tail = p_ref;
                        } else {
                            ZeroCCASTNode *tail_node = zero_cc_ast_get(arena, param_tail);
                            if (tail_node) tail_node->next = p_ref;
                            param_tail = p_ref;
                        }
                    }
                }
                if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RPAREN) {
                    c->token_pos++; // consume ')'
                }
                param_list = param_head;
            }

            if (is_function) {
                /* Function definition or prototype */
                if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LBRACE) {
                    /* Function definition with body */
                    c->token_pos++; // consume '{'
                    ASTRef head = AST_NULL;
                    ASTRef tail = AST_NULL;
                    while (c->token_pos < c->token_count &&
                           c->tokens[c->token_pos].type != TK_RBRACE &&
                           c->tokens[c->token_pos].type != TK_EOF) {
                        ASTRef s = zero_cc_parse_stmt_ast(c, arena, ctx);
                        if (s != AST_NULL) {
                            if (head == AST_NULL) {
                                head = s;
                                tail = s;
                            } else {
                                ZeroCCASTNode *tail_node = zero_cc_ast_get(arena, tail);
                                if (tail_node) tail_node->next = s;
                                tail = s;
                            }
                        }
                    }
                    if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RBRACE) {
                        c->token_pos++; // consume '}'
                    }
                    ASTRef body_ref = head;

                    /* Create function node */
                    ASTRef func_ref = zero_cc_ast_alloc_node(arena, NODE_DECL);
                    ZeroCCASTNode *func_node = zero_cc_ast_get(arena, func_ref);
                    func_node->line = line;
                    func_node->col = col;
                    func_node->s.str = vname;
                    func_node->s.len = strlen(vname);
                    func_node->left = param_list;   // parameter list
                    func_node->right = body_ref;    // body
                    /* Store return type in aux.extra for later semantic analysis */
                    ZeroCCType ret_type;
                    if (type_tok == TK_CHAR || type_tok == TK_BOOL_KW) ret_type = zero_cc_type_char();
                    else if (type_tok == TK_SHORT) ret_type = zero_cc_type_int16();
                    else if (type_tok == TK_INT) ret_type = zero_cc_type_int();
                    else if (type_tok == TK_FLOAT_KW) ret_type = zero_cc_type_float();
                    else if (type_tok == TK_DOUBLE_KW) ret_type = zero_cc_type_double();
                    else if (type_tok == TK_VOID) ret_type = zero_cc_type_void();
                    else {
                        ZeroCCType *tdef = zero_cc_find_typedef(c, c->tokens[type_start_pos].str_val);
                        ret_type = tdef ? *tdef : zero_cc_type_int64();
                    }
                    if (is_ptr) ret_type = zero_cc_type_ptr(&ret_type);
                    /* Store return type in arena and save arena offset */
                    ZeroCCType *ret_type_ptr = arena_alloc(ctx->perm_arena, sizeof(ZeroCCType));
                    *ret_type_ptr = ret_type;
                    func_node->aux.extra = (uint32_t)(arena_index_of(ctx->perm_arena, ret_type_ptr) + 1);
                    return func_ref;
                } else {
                    /* Function prototype */
                    if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_SEMI) {
                        c->token_pos++;
                    }
                    ASTRef func_ref = zero_cc_ast_alloc_node(arena, NODE_DECL);
                    ZeroCCASTNode *func_node = zero_cc_ast_get(arena, func_ref);
                    func_node->line = line;
                    func_node->col = col;
                    func_node->s.str = vname;
                    func_node->s.len = strlen(vname);
                    func_node->flags |= NODE_FLAG_FUNC_PROTO;
                    func_node->left = param_list;
                    func_node->right = AST_NULL;
                    ZeroCCType ret_type;
                    if (type_tok == TK_CHAR || type_tok == TK_BOOL_KW) ret_type = zero_cc_type_char();
                    else if (type_tok == TK_SHORT) ret_type = zero_cc_type_int16();
                    else if (type_tok == TK_INT) ret_type = zero_cc_type_int();
                    else if (type_tok == TK_FLOAT_KW) ret_type = zero_cc_type_float();
                    else if (type_tok == TK_DOUBLE_KW) ret_type = zero_cc_type_double();
                    else if (type_tok == TK_VOID) ret_type = zero_cc_type_void();
                    else {
                        ZeroCCType *tdef = zero_cc_find_typedef(c, c->tokens[type_start_pos].str_val);
                        if (!tdef) tdef = zero_cc_find_struct(c, c->tokens[type_start_pos].str_val);
                        ret_type = tdef ? *tdef : zero_cc_type_int64();
                    }
                    if (is_ptr) ret_type = zero_cc_type_ptr(&ret_type);
                    /* Store return type in arena and save arena offset */
                    ZeroCCType *ret_type_ptr = arena_alloc(ctx->perm_arena, sizeof(ZeroCCType));
                    *ret_type_ptr = ret_type;
                    func_node->aux.extra = (uint32_t)(arena_index_of(ctx->perm_arena, ret_type_ptr) + 1);
                    return func_ref;
                }
            } else {
                /* Not a function: treat as variable declaration */
                bool is_array = false;
                bool is_vla = false;
                ASTRef dim_expr = AST_NULL;
                size_t fixed_count = 0;

                if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LBRACKET) {
                    c->token_pos++; // consume '['
                    is_array = true;
                    if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RBRACKET) {
                        c->token_pos++;
                        fixed_count = 0;
                    } else {
                        dim_expr = parse_expr_ast_internal(c, arena);
                        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RBRACKET) {
                            c->token_pos++;
                        }
                        ZeroCCASTNode *dim_node = zero_cc_ast_get(arena, dim_expr);
                        if (dim_node && dim_node->tag == NODE_INT && dim_node->int_val >= 0) {
                            fixed_count = (size_t)dim_node->int_val;
                        } else {
                            is_vla = true;
                        }
                    }
                }

                ZeroCCType decl_type;
                if (type_tok == TK_CHAR || type_tok == TK_BOOL_KW) decl_type = zero_cc_type_char();
                else if (type_tok == TK_SHORT) decl_type = zero_cc_type_int16();
                else if (type_tok == TK_INT) decl_type = zero_cc_type_int();
                else if (type_tok == TK_FLOAT_KW) decl_type = zero_cc_type_float();
                else if (type_tok == TK_DOUBLE_KW) decl_type = zero_cc_type_double();
                else if (type_tok == TK_VOID) decl_type = zero_cc_type_void();
                else {
                    ZeroCCType *tdef = zero_cc_find_typedef(c, c->tokens[type_start_pos].str_val);
                    if (!tdef) tdef = zero_cc_find_struct(c, c->tokens[type_start_pos].str_val);
                    decl_type = tdef ? *tdef : zero_cc_type_int64();
                }
                decl_type.is_const = is_const;
                decl_type.is_volatile = is_volatile;
                decl_type.is_atomic = is_atomic;
                if (is_ptr) {
                    decl_type = zero_cc_type_ptr(&decl_type);
                    decl_type.is_const = ptr_is_const;
                    decl_type.is_volatile = ptr_is_volatile;
                }

                TypeInfo *decl_ti = NULL;
                if (is_vla) {
                    ZeroCCType elem_type = decl_type;
                    decl_type = zero_cc_type_array(&elem_type, 0);
                    decl_type.is_vla = true;
                    decl_type.size = 8;
                    decl_type.align = 8;
                    TypeInfo *elem_ti = zero_cc_type_to_typeinfo(ctx->perm_arena, elem_type);
                    decl_ti = type_array_of(ctx->perm_arena, elem_ti, 0);
                    if (decl_ti) {
                        decl_ti->details.array.is_vla = true;
                        if (elem_type.is_const) decl_ti->modifiers |= TYPE_MOD_CONST;
                        if (elem_type.is_volatile) decl_ti->modifiers |= TYPE_MOD_VOLATILE;
                    }
                } else if (is_array) {
                    ZeroCCType elem_type = decl_type;
                    decl_type = zero_cc_type_array(&elem_type, fixed_count);
                    TypeInfo *elem_ti = zero_cc_type_to_typeinfo(ctx->perm_arena, elem_type);
                    decl_ti = type_array_of(ctx->perm_arena, elem_ti, fixed_count);
                    if (decl_ti) {
                        if (elem_type.is_const) decl_ti->modifiers |= TYPE_MOD_CONST;
                        if (elem_type.is_volatile) decl_ti->modifiers |= TYPE_MOD_VOLATILE;
                    }
                } else {
                    decl_ti = zero_cc_type_to_typeinfo(ctx->perm_arena, decl_type);
                }

                ZeroCCSym *sym = zero_cc_add_local(c, vname, decl_type);
                if (sym) {
                    if (is_vla) sym->is_vla = true;
                    if (is_atomic) sym->is_atomic = true;
                    if (decl_type.is_const) sym->is_const = true;
                    if (decl_type.is_volatile) sym->is_volatile = true;
                }

                ASTRef decl_ref = zero_cc_ast_alloc_node(arena, NODE_DECL);
                ZeroCCASTNode *dn = zero_cc_ast_get(arena, decl_ref);
                dn->line = line;
                dn->col = col;
                dn->s.str = vname;
                dn->s.len = strlen(vname);
                if (is_vla) {
                    dn->flags |= NODE_DECL_IS_VLA;
                    dn->right = dim_expr;
                }

                /* Attach type to AST node for semantic analysis */
                decl_node_set_type(ctx, dn, decl_ti);

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


#endif /* ZERO_CC_PARSER_AST_STMT_H */
