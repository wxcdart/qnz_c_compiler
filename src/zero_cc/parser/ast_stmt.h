#ifndef ZERO_CC_PARSER_AST_STMT_H
#define ZERO_CC_PARSER_AST_STMT_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "zero_cc/types/types.h"
#include "zero_cc/types/global_vars.h"
#include "zero_cc/parser/parser.h"
#include "zero_cc/jit/type_info.h"
#include "zero_cc/semantic/decl.h"
#include "zero_cc/parser/definitions.h"
#include "zero_cc/parser/init_common.h"

static ASTRef parse_expr_ast_internal(ZeroCCCompiler *c, ZeroCCASTArena *arena);
static ASTRef parse_assignment_ast(ZeroCCCompiler *c, ZeroCCASTArena *arena);
static ASTRef parse_brace_init_list_ast(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCType lit_type, size_t base_off, size_t *leaf_cursor, size_t *out_elem_count);

#include "zero_cc/parser/zero_cc_declarator_stack.h"

#include "zero_cc/parser/decl_local.h"

static void zero_cc_synchronize_ast(ZeroCCCompiler *c) {
    if (c->token_pos < c->token_count) {
        c->token_pos++;
    }
    while (c->token_pos < c->token_count) {
        ZeroCCTokType t = c->tokens[c->token_pos].type;
        if (t == TK_SEMI) {
            c->token_pos++;
            return;
        }
        if (t == TK_RBRACE || t == TK_EOF) {
            return;
        }
        /* Stop at statement-starting keywords */
        if (t == TK_IF || t == TK_WHILE || t == TK_DO || t == TK_FOR ||
            t == TK_SWITCH || t == TK_CASE || t == TK_DEFAULT ||
            t == TK_RETURN || t == TK_BREAK || t == TK_CONTINUE ||
            t == TK_GOTO || t == TK_STATIC_ASSERT || t == TK_LBRACE ||
            is_type_specifier(c, t)) {
            return;
        }
        c->token_pos++;
    }
}

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

    /* GNU asm statement:
     *   __asm__ [ __volatile__ | volatile ] ( "template"* [: ops]* ) ;
     *   __asm__ [ __volatile__ | volatile ] "template"* [: ops]* ;
     * The template (and the whole operand group) may be wrapped in a
     * single paren pair.  Empty templates (compiler barriers such as
     * `"" : : : "memory"` or clobber-only forms) lower to a no-op.  A
     * non-empty template emits machine instructions ZeroCC has no model
     * for, so it is a clean reject (c-torture execute/990413-2 uses i387
     * fpatan/fsqrt). */
    if (t == TK_IDENT && c->tokens[c->token_pos].str_val &&
        (strcmp(c->tokens[c->token_pos].str_val, "__asm__") == 0 ||
         strcmp(c->tokens[c->token_pos].str_val, "__asm") == 0 ||
         strcmp(c->tokens[c->token_pos].str_val, "asm") == 0)) {
        c->token_pos++;
        if (c->token_pos < c->token_count &&
            ((c->tokens[c->token_pos].type == TK_IDENT &&
              c->tokens[c->token_pos].str_val &&
              (strcmp(c->tokens[c->token_pos].str_val, "__volatile__") == 0 ||
               strcmp(c->tokens[c->token_pos].str_val, "__volatile") == 0 ||
               strcmp(c->tokens[c->token_pos].str_val, "volatile") == 0)) ||
             c->tokens[c->token_pos].type == TK_VOLATILE)) {
            c->token_pos++;
        }
        bool asm_paren = false;
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LPAREN) {
            asm_paren = true;
            c->token_pos++;
        }
        /* Template: one or more adjacent string literals. */
        bool asm_nonempty = false;
        while (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_STRING) {
            for (const char *ch = c->tokens[c->token_pos].str_val; *ch; ch++) {
                if (*ch != ' ' && *ch != '\t' && *ch != '\n' && *ch != '\r') {
                    asm_nonempty = true;
                    break;
                }
            }
            c->token_pos++;
        }
        /* Operand lists: `: "constraint" (expr) , ...` - consume each
         * constraint string and (for output/input) its parenthesized
         * expression; clobbers are bare strings. */
        int colon_section = 0; /* 0 = template, 1 = outputs, 2 = inputs, 3 = clobbers */
        ASTRef out_exprs[8] = {AST_NULL};
        char out_constraints[8][32] = {{0}};
        int out_count = 0;
        ASTRef in_exprs[8] = {AST_NULL};
        char in_constraints[8][32] = {{0}};
        int in_count = 0;
        char last_constraint[32] = {0};

        bool asm_bail = false;
        while (c->token_pos < c->token_count &&
               c->tokens[c->token_pos].type != TK_SEMI &&
               !(asm_paren && c->tokens[c->token_pos].type == TK_RPAREN)) {
            ZeroCCTokType ot = c->tokens[c->token_pos].type;
            if (ot == TK_COLON) {
                colon_section++;
                last_constraint[0] = '\0';
                c->token_pos++;
            } else if (ot == TK_COLONCOLON) {
                colon_section += 2;
                last_constraint[0] = '\0';
                c->token_pos++;
            } else if (ot == TK_COMMA) {
                last_constraint[0] = '\0';
                c->token_pos++;
            } else if (ot == TK_STRING) {
                const char *s = c->tokens[c->token_pos].str_val;
                if (s) snprintf(last_constraint, sizeof(last_constraint), "%s", s);
                c->token_pos++;
            } else if (ot == TK_LPAREN) {
                c->token_pos++;
                ASTRef e = parse_expr_ast_internal(c, arena);
                if (colon_section == 1 && out_count < 8) {
                    out_exprs[out_count] = e;
                    snprintf(out_constraints[out_count], sizeof(out_constraints[0]), "%s", last_constraint);
                    out_count++;
                } else if (colon_section == 2 && in_count < 8) {
                    in_exprs[in_count] = e;
                    snprintf(in_constraints[in_count], sizeof(in_constraints[0]), "%s", last_constraint);
                    in_count++;
                }
                if (c->token_pos < c->token_count &&
                    c->tokens[c->token_pos].type == TK_RPAREN) c->token_pos++;
            } else {
                asm_bail = true;
                break;
            }
        }
        if (asm_bail) {
            /* Desynced: skip to the ';' at bracket depth 0 (an inner
             * `)` from an operand list must not end the skip). */
            int depth = 0;
            while (c->token_pos < c->token_count) {
                ZeroCCTokType bt = c->tokens[c->token_pos].type;
                if (bt == TK_LPAREN || bt == TK_LBRACKET || bt == TK_LBRACE) depth++;
                else if (bt == TK_RPAREN || bt == TK_RBRACKET || bt == TK_RBRACE) {
                    if (depth == 0 && asm_paren) break;
                    if (depth > 0) depth--;
                } else if (bt == TK_SEMI && depth == 0) break;
                c->token_pos++;
            }
        }
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RPAREN) c->token_pos++;
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
        ASTRef head_stmt = AST_NULL;
        ASTRef tail_stmt = AST_NULL;
        if (!asm_nonempty && !asm_bail) {
            for (int i = 0; i < out_count; i++) {
                if (out_exprs[i] == AST_NULL) continue;
                if (out_constraints[i][0] == '+') {
                    /* In-out operand: evaluate expression for side effects */
                    ASTRef es_ref = zero_cc_ast_alloc_node(arena, NODE_EXPR_STMT);
                    ZeroCCASTNode *esn = zero_cc_ast_get(arena, es_ref);
                    if (esn) {
                        esn->line = line;
                        esn->col = col;
                        esn->left = out_exprs[i];
                        if (head_stmt == AST_NULL) {
                            head_stmt = es_ref;
                            tail_stmt = es_ref;
                        } else {
                            ZeroCCASTNode *tn = zero_cc_ast_get(arena, tail_stmt);
                            if (tn) tn->next = es_ref;
                            tail_stmt = es_ref;
                        }
                    }
                    continue;
                }
                /* Find input matching output i */
                ASTRef src_expr = AST_NULL;
                for (int j = 0; j < in_count; j++) {
                    if (in_constraints[j][0] == ('0' + i)) {
                        src_expr = in_exprs[j];
                        break;
                    }
                }
                if (src_expr == AST_NULL && i < in_count) {
                    src_expr = in_exprs[i];
                }
                if (src_expr != AST_NULL) {
                    ASTRef assign_ref = zero_cc_ast_alloc_node(arena, NODE_ASSIGN);
                    ZeroCCASTNode *an = zero_cc_ast_get(arena, assign_ref);
                    if (an) {
                        an->line = line;
                        an->col = col;
                        an->left = out_exprs[i];
                        an->right = src_expr;
                        an->aux.op = (uint32_t)TK_ASSIGN;
                        ASTRef es_ref = zero_cc_ast_alloc_node(arena, NODE_EXPR_STMT);
                        ZeroCCASTNode *esn = zero_cc_ast_get(arena, es_ref);
                        if (esn) {
                            esn->line = line;
                            esn->col = col;
                            esn->left = assign_ref;
                            if (head_stmt == AST_NULL) {
                                head_stmt = es_ref;
                                tail_stmt = es_ref;
                            } else {
                                ZeroCCASTNode *tn = zero_cc_ast_get(arena, tail_stmt);
                                if (tn) tn->next = es_ref;
                                tail_stmt = es_ref;
                            }
                        }
                    }
                }
            }
        }
        if (asm_nonempty) {
            diag_report_error_at_token(c,
                "GNU asm statement with a non-empty template is not supported");
            c->had_error = 1;
        }
        return head_stmt;
    }

    /* GNU local label declaration: __label__ l1, l2; */
    if (t == TK_IDENT && c->tokens[c->token_pos].str_val &&
        strcmp(c->tokens[c->token_pos].str_val, "__label__") == 0) {
        c->token_pos++;
        while (c->token_pos < c->token_count && c->tokens[c->token_pos].type != TK_SEMI &&
               c->tokens[c->token_pos].type != TK_RBRACE && c->tokens[c->token_pos].type != TK_EOF) {
            c->token_pos++;
        }
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_SEMI) {
            c->token_pos++;
        }
        return zero_cc_parse_stmt_ast(c, arena, ctx);
    }

    /* Goto statement: goto label; OR goto *expr; */
    if (t == TK_GOTO) {
        c->token_pos++;
        ASTRef g_ref = zero_cc_ast_alloc_node(arena, NODE_GOTO);
        ZeroCCASTNode *gn = zero_cc_ast_get(arena, g_ref);
        gn->line = line;
        gn->col = col;
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_STAR) {
            c->token_pos++;
            gn->left = parse_expr_ast_internal(c, arena);
        } else if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_IDENT) {
            gn->s.str = zero_cc_intern_string(c, c->tokens[c->token_pos].str_val);
            gn->s_len = strlen(gn->s.str);
            c->token_pos++;
        }
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_SEMI) {
            c->token_pos++;
        }
        return g_ref;
    }

    /* Labeled statement: label: stmt (a statement-leading IDENT immediately
       followed by ':' is a user label, not an expression) */
    if (t == TK_IDENT && c->token_pos + 1 < c->token_count &&
        c->tokens[c->token_pos + 1].type == TK_COLON) {
        ASTRef l_ref = zero_cc_ast_alloc_node(arena, NODE_LABEL);
        ZeroCCASTNode *ln = zero_cc_ast_get(arena, l_ref);
        ln->line = line;
        ln->col = col;
        ln->s.str = zero_cc_intern_string(c, c->tokens[c->token_pos].str_val);
        ln->s_len = strlen(ln->s.str);
        c->token_pos += 2; /* ident ':' */
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type != TK_RBRACE) {
            ln->left = zero_cc_parse_stmt_ast(c, arena, ctx);
        }
        return l_ref;
    }

    /* Block-scoped typedef: typedef X Alias; - register into the TU-wide
       typedef table (same handlers as file scope); emits no IR. */
    if (t == TK_TYPEDEF) {
        if (c->token_pos + 1 < c->token_count &&
            (c->tokens[c->token_pos + 1].type == TK_STRUCT ||
             c->tokens[c->token_pos + 1].type == TK_UNION)) {
            zero_cc_stmt_typedef_struct(c);
        } else if (c->token_pos + 1 < c->token_count && c->tokens[c->token_pos + 1].type == TK_ENUM) {
            zero_cc_stmt_typedef_enum(c);
        } else {
            zero_cc_stmt_typedef_scalar(c, arena);
        }
        return AST_NULL;
    }

    /* Compound Block: { stmt* } */
    if (t == TK_LBRACE) {
        c->token_pos++;
        ASTRef block_ref = zero_cc_ast_alloc_node(arena, NODE_BLOCK);
        ZeroCCASTNode *bn = zero_cc_ast_get(arena, block_ref);
        bn->line = line;
        bn->col = col;
        uint32_t locals_at_entry = (uint32_t)c->local_count;

        ASTRef head = AST_NULL;
        ASTRef tail = AST_NULL;

        while (c->token_pos < c->token_count &&
               c->tokens[c->token_pos].type != TK_RBRACE &&
               c->tokens[c->token_pos].type != TK_EOF) {
            size_t entry_pos = c->token_pos;
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
                while (tail != AST_NULL) {
                    ZeroCCASTNode *cur_node = zero_cc_ast_get(arena, tail);
                    if (cur_node && cur_node->next != AST_NULL) {
                        tail = cur_node->next;
                    } else {
                        break;
                    }
                }
            } else {
                /* If statement failed to parse or did not advance token position, report error and synchronize */
                if (c->token_pos == entry_pos) {
                    diag_report_error_at_token(c, "unexpected token '%s' in statement",
                                               c->tokens[c->token_pos].str_val[0] ? c->tokens[c->token_pos].str_val : "unknown");
                    zero_cc_synchronize_ast(c);
                }
            }
        }
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RBRACE) {
            c->token_pos++;
        } else {
            /* Block ran into end of input without its closing '}':
             * truncated source (C99 6.8p10).  Previously accepted
             * silently, so a pasted-without-the-end file compiled and
             * ran garbage. */
            diag_report_error_at_token(c, "expected '}' to end block");
        }
        bn = zero_cc_ast_get(arena, block_ref);
        if (bn) {
            bn->left = head;
            /* Block-scope metadata for the lowering pass (C11 6.2.1):
             * aux.alt = locals at block entry + 1
             * aux.step = locals at block exit + 1 (0 means unannotated).
             * The lowering caps name lookup at (aux.step - 1) inside the block,
             * and restores to (aux.alt - 1) on block exit so inner declarations
             * do not leak into following outer statements. */
            bn->aux.alt = locals_at_entry + 1;
            bn->aux.step = (uint32_t)(c->local_count + 1);
        }
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
            ASTRef expr = parse_expr_ast_internal(c, arena);
            rn = zero_cc_ast_get(arena, ret_ref);
            if (rn) rn->left = expr;
        }
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_SEMI) {
            c->token_pos++;
        } else if (c->token_pos < c->token_count &&
                   c->tokens[c->token_pos].type != TK_RBRACE) {
            /* A return statement always ends in ';' (C99 6.8.4.1).  The
             * missing-';' case used to be silently swallowed, so
             * `return x + 5 return;` only surfaced as a misleading
             * "should return a value" on the next token. */
            diag_report_error_at_token(c, "expected ';' after return statement");
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

        /* For-scope metadata (C11 6.8.5): the loop variable declared in the
         * init (e.g. `for (int i = 0; ...)`) is visible for the whole for
         * statement (init/cond/step/body) but not after it.  Record the
         * symbol count right after the init so the lowering can cap name
         * lookup for the entire for: a LATER sibling for redeclaring the
         * same name must not shadow this loop's header (aux.op = count+1,
         * 0 = no init declaration). */
        {
            ZeroCCASTNode *fn_now = zero_cc_ast_get(arena, for_ref);
            if (fn_now) fn_now->aux.op = (uint32_t)(c->local_count + 1);
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

    /* Case statement: case const_expr: stmt or case low ... high: stmt */
    if (t == TK_CASE) {
        c->token_pos++;
        ASTRef case_ref = zero_cc_ast_alloc_node(arena, NODE_CASE);
        ZeroCCASTNode *cn = zero_cc_ast_get(arena, case_ref);
        cn->line = line;
        cn->col = col;

        ASTRef val = parse_expr_ast_internal(c, arena);
        ASTRef val_high = AST_NULL;
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_ELLIPSIS) {
            c->token_pos++; /* consume '...' */
            val_high = parse_expr_ast_internal(c, arena);
        }
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_COLON) {
            c->token_pos++;
        }
        ASTRef stmt = AST_NULL;
        /* Sprint 72: a following case/default label is a *nested* labeled
         * statement - C allows `case A: case B: s;` and the braceless
         * `switch (x) case A: case B: s;` (the whole labeled statement is the
         * switch body). Parse it as this label's statement; the lowering emits
         * the label then lowers the nested statement, equivalent to the flat
         * sibling form. Only a closing brace ends an empty label. */
        if (c->token_pos < c->token_count &&
            c->tokens[c->token_pos].type != TK_RBRACE) {
            stmt = zero_cc_parse_stmt_ast(c, arena, ctx);
        }
        cn = zero_cc_ast_get(arena, case_ref);
        if (cn) {
            cn->left = val;
            cn->right = stmt;
            cn->aux.step = (uint32_t)val_high;
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
        /* Sprint 72: a following case/default label is a *nested* labeled
         * statement (see the TK_CASE branch); only a closing brace ends an
         * empty label. */
        if (c->token_pos < c->token_count &&
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
        ASTRef cond = parse_assignment_ast(c, arena);
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

    /* Local variable, typedef, or block-scoped enum declaration */
    ASTRef decl_ref = AST_NULL;
    if (zero_cc_parse_local_decl(c, arena, ctx, line, col, &decl_ref)) {
        return decl_ref;
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
