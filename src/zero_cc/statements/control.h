/* Control Flow Statements */
#ifndef ZERO_CC_STATEMENTS_CONTROL_H
#define ZERO_CC_STATEMENTS_CONTROL_H

/* Return Statement: return <expr>; */
static inline void zero_cc_stmt_return(ZeroCCCompiler *c) {
    c->token_pos++; /* consume TK_RETURN */
    if (c->tokens[c->token_pos].type != TK_SEMI) {
        /* Check if returning a struct variable: return sym; */
        if (c->tokens[c->token_pos].type == TK_IDENT && c->tokens[c->token_pos + 1].type == TK_SEMI) {
            ZeroCCSym *sym = zero_cc_find_local(c, c->tokens[c->token_pos].str_val);
            if (sym && sym->type.tag == TYPE_STRUCT) {
                c->token_pos++;
                /* mov rax, [rbp - sym_offset] */
                zero_cc_emit_u8(&c->text_section, 0x48);
                zero_cc_emit_u8(&c->text_section, 0x8B);
                zero_cc_emit_u8(&c->text_section, 0x85);
                zero_cc_emit_u32(&c->text_section, (uint32_t)(-sym->stack_offset));
                if (sym->type.size > 8) {
                    /* mov rdx, [rbp - (sym_offset - 8)] */
                    int32_t off2 = (int32_t)sym->stack_offset - 8;
                    zero_cc_emit_u8(&c->text_section, 0x48);
                    zero_cc_emit_u8(&c->text_section, 0x8B);
                    zero_cc_emit_u8(&c->text_section, 0x95);
                    zero_cc_emit_u32(&c->text_section, (uint32_t)(-off2));
                }
                zero_cc_emit_u8(&c->text_section, 0xC9); /* leave */
                zero_cc_emit_u8(&c->text_section, 0xC3); /* ret */
                if (c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
                return;
            }
        }
        zero_cc_parse_expr(c);
    }
    zero_cc_emit_u8(&c->text_section, 0xC9); /* leave */
    zero_cc_emit_u8(&c->text_section, 0xC3); /* ret */
    if (c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
}

/* If / Else Statement */
static inline void zero_cc_stmt_if(ZeroCCCompiler *c) {
    c->token_pos++; /* consume TK_IF */
    if (c->tokens[c->token_pos].type == TK_LPAREN) c->token_pos++;
    zero_cc_parse_expr(c);
    if (c->tokens[c->token_pos].type == TK_RPAREN) c->token_pos++;

    zero_cc_emit_u8(&c->text_section, 0x48);
    zero_cc_emit_u8(&c->text_section, 0x85);
    zero_cc_emit_u8(&c->text_section, 0xC0);
    zero_cc_emit_u8(&c->text_section, 0x0F);
    zero_cc_emit_u8(&c->text_section, 0x84);
    size_t jz_pos = c->text_section.length;
    zero_cc_emit_u32(&c->text_section, 0);

    zero_cc_parse_stmt(c);

    if (c->tokens[c->token_pos].type == TK_ELSE) {
        c->token_pos++;
        zero_cc_emit_u8(&c->text_section, 0xE9);
        size_t jmp_pos = c->text_section.length;
        zero_cc_emit_u32(&c->text_section, 0);

        int32_t else_rel = (int32_t)(c->text_section.length - (jz_pos + 4));
        memcpy(c->text_section.data + jz_pos, &else_rel, 4);

        zero_cc_parse_stmt(c);

        int32_t end_rel = (int32_t)(c->text_section.length - (jmp_pos + 4));
        memcpy(c->text_section.data + jmp_pos, &end_rel, 4);
    } else {
        int32_t end_rel = (int32_t)(c->text_section.length - (jz_pos + 4));
        memcpy(c->text_section.data + jz_pos, &end_rel, 4);
    }
}

/* Static Assert Declaration */
static inline void zero_cc_stmt_static_assert(ZeroCCCompiler *c) {
    c->token_pos++; /* consume TK_STATIC_ASSERT */
    if (c->tokens[c->token_pos].type != TK_LPAREN) {
        c->had_error = 1;
        snprintf(c->error_msg, sizeof(c->error_msg), "Expected '(' after _Static_assert");
        return;
    }
    c->token_pos++; /* skip '(' */
    int64_t val = zero_cc_eval_const_expr(c);
    if (c->tokens[c->token_pos].type == TK_COMMA) {
        c->token_pos++; /* skip ',' */
        if (c->tokens[c->token_pos].type == TK_STRING) c->token_pos++; /* skip message */
        if (c->tokens[c->token_pos].type != TK_RPAREN) {
            c->had_error = 1;
            snprintf(c->error_msg, sizeof(c->error_msg), "Expected ')' after message");
            return;
        }
        c->token_pos++; /* skip ')' */
    } else if (c->tokens[c->token_pos].type == TK_RPAREN) {
        c->token_pos++; /* skip ')' */
    } else {
        c->had_error = 1;
        snprintf(c->error_msg, sizeof(c->error_msg), "Expected ')' or ',' after constant expression");
        return;
    }
    if (c->tokens[c->token_pos].type != TK_SEMI) {
        c->had_error = 1;
        snprintf(c->error_msg, sizeof(c->error_msg), "Expected ';' after _Static_assert");
        return;
    }
    c->token_pos++; /* consume ';' */
    if (val == 0) {
        c->had_error = 1;
        snprintf(c->error_msg, sizeof(c->error_msg), "Static assertion failed");
        return;
    }
}

/* Switch Statement: switch (expr) { case V: ... break; default: ... } */
static inline void zero_cc_stmt_switch(ZeroCCCompiler *c) {
    c->token_pos++; /* consume TK_SWITCH */
    if (c->tokens[c->token_pos].type == TK_LPAREN) c->token_pos++;
    zero_cc_parse_expr(c); /* switch value in rax */
    if (c->tokens[c->token_pos].type == TK_RPAREN) c->token_pos++;

    zero_cc_emit_u8(&c->text_section, 0x50); /* push rax */

    if (c->tokens[c->token_pos].type == TK_LBRACE) c->token_pos++;

    size_t break_patches[128];
    size_t break_count = 0;
    size_t next_case_patch = 0;
    bool has_pending_case_patch = false;

    while (c->tokens[c->token_pos].type != TK_RBRACE && c->tokens[c->token_pos].type != TK_EOF) {
        while (c->tokens[c->token_pos].type == TK_LDBRACKET) {
            c->token_pos++;
            int depth = 1;
            while (c->tokens[c->token_pos].type != TK_EOF && depth > 0) {
                if (c->tokens[c->token_pos].type == TK_LDBRACKET) depth++;
                else if (c->tokens[c->token_pos].type == TK_RDBRACKET) depth--;
                c->token_pos++;
            }
            if (c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
        }

        if (c->tokens[c->token_pos].type == TK_CASE) {
            c->token_pos++;
            int64_t case_val = 0;
            if (c->tokens[c->token_pos].type == TK_NUM) {
                case_val = c->tokens[c->token_pos].num_val;
                c->token_pos++;
            } else if (c->tokens[c->token_pos].type == TK_IDENT) {
                ZeroCCEnumConst *ec = zero_cc_find_enum_const(c, c->tokens[c->token_pos].str_val);
                if (ec) case_val = ec->value;
                c->token_pos++;
            }
            if (c->tokens[c->token_pos].type == TK_COLON) c->token_pos++;

            if (has_pending_case_patch) {
                int32_t rel = (int32_t)(c->text_section.length - (next_case_patch + 4));
                memcpy(c->text_section.data + next_case_patch, &rel, 4);
            }

            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0x81);
            zero_cc_emit_u8(&c->text_section, 0x3C);
            zero_cc_emit_u8(&c->text_section, 0x24);
            zero_cc_emit_u32(&c->text_section, (uint32_t)case_val);

            zero_cc_emit_u8(&c->text_section, 0x0F);
            zero_cc_emit_u8(&c->text_section, 0x85);
            next_case_patch = c->text_section.length;
            zero_cc_emit_u32(&c->text_section, 0);
            has_pending_case_patch = true;
            continue;
        }

        if (c->tokens[c->token_pos].type == TK_DEFAULT) {
            c->token_pos++;
            if (c->tokens[c->token_pos].type == TK_COLON) c->token_pos++;
            if (has_pending_case_patch) {
                int32_t rel = (int32_t)(c->text_section.length - (next_case_patch + 4));
                memcpy(c->text_section.data + next_case_patch, &rel, 4);
                has_pending_case_patch = false;
            }
            continue;
        }

        if (c->tokens[c->token_pos].type == TK_BREAK) {
            c->token_pos++;
            if (c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
            zero_cc_emit_u8(&c->text_section, 0xE9);
            if (break_count < 128) {
                break_patches[break_count++] = c->text_section.length;
            }
            zero_cc_emit_u32(&c->text_section, 0);
            continue;
        }

        size_t ppos = c->token_pos;
        zero_cc_parse_stmt(c);
        if (c->token_pos == ppos) c->token_pos++;
    }

    if (c->tokens[c->token_pos].type == TK_RBRACE) c->token_pos++;

    if (has_pending_case_patch) {
        int32_t rel = (int32_t)(c->text_section.length - (next_case_patch + 4));
        memcpy(c->text_section.data + next_case_patch, &rel, 4);
    }

    for (size_t b = 0; b < break_count; b++) {
        int32_t rel = (int32_t)(c->text_section.length - (break_patches[b] + 4));
        memcpy(c->text_section.data + break_patches[b], &rel, 4);
    }

    zero_cc_emit_u8(&c->text_section, 0x58); /* pop rax */
}

/* While Loop */
static inline void zero_cc_stmt_while(ZeroCCCompiler *c) {
    c->token_pos++; /* consume TK_WHILE */
    size_t loop_start = c->text_section.length;
    if (c->tokens[c->token_pos].type == TK_LPAREN) c->token_pos++;
    zero_cc_parse_expr(c);
    if (c->tokens[c->token_pos].type == TK_RPAREN) c->token_pos++;

    zero_cc_emit_u8(&c->text_section, 0x48);
    zero_cc_emit_u8(&c->text_section, 0x85);
    zero_cc_emit_u8(&c->text_section, 0xC0);
    zero_cc_emit_u8(&c->text_section, 0x0F);
    zero_cc_emit_u8(&c->text_section, 0x84);
    size_t jz_pos = c->text_section.length;
    zero_cc_emit_u32(&c->text_section, 0);

    zero_cc_parse_stmt(c);

    zero_cc_emit_u8(&c->text_section, 0xE9);
    int32_t back_rel = (int32_t)(loop_start - (c->text_section.length + 4));
    zero_cc_emit_u32(&c->text_section, (uint32_t)back_rel);

    int32_t exit_rel = (int32_t)(c->text_section.length - (jz_pos + 4));
    memcpy(c->text_section.data + jz_pos, &exit_rel, 4);
}

/* Do-While Loop: do { body } while (cond); */
static inline void zero_cc_stmt_do_while(ZeroCCCompiler *c) {
    c->token_pos++; /* consume TK_DO */
    size_t loop_start = c->text_section.length;
    zero_cc_parse_stmt(c);

    if (c->tokens[c->token_pos].type == TK_WHILE) {
        c->token_pos++;
        if (c->tokens[c->token_pos].type == TK_LPAREN) c->token_pos++;
        zero_cc_parse_expr(c);
        if (c->tokens[c->token_pos].type == TK_RPAREN) c->token_pos++;
        if (c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;

        zero_cc_emit_u8(&c->text_section, 0x48);
        zero_cc_emit_u8(&c->text_section, 0x85);
        zero_cc_emit_u8(&c->text_section, 0xC0);
        zero_cc_emit_u8(&c->text_section, 0x0F);
        zero_cc_emit_u8(&c->text_section, 0x85); /* jnz */
        int32_t back_rel = (int32_t)(loop_start - (c->text_section.length + 4));
        zero_cc_emit_u32(&c->text_section, (uint32_t)back_rel);
    }
}

/* For Loop: for (init; cond; step) body */
static inline void zero_cc_stmt_for(ZeroCCCompiler *c) {
    c->token_pos++; /* consume TK_FOR */
    if (c->tokens[c->token_pos].type == TK_LPAREN) c->token_pos++;

    if (c->tokens[c->token_pos].type != TK_SEMI) {
        zero_cc_parse_stmt(c);
    } else {
        c->token_pos++;
    }

    size_t cond_start = c->text_section.length;
    bool has_cond = (c->tokens[c->token_pos].type != TK_SEMI);
    size_t jz_pos = 0;
    if (has_cond) {
        zero_cc_parse_expr(c);
        zero_cc_emit_u8(&c->text_section, 0x48);
        zero_cc_emit_u8(&c->text_section, 0x85);
        zero_cc_emit_u8(&c->text_section, 0xC0);
        zero_cc_emit_u8(&c->text_section, 0x0F);
        zero_cc_emit_u8(&c->text_section, 0x84);
        jz_pos = c->text_section.length;
        zero_cc_emit_u32(&c->text_section, 0);
    }
    if (c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;

    size_t step_tok_start = c->token_pos;
    int paren_depth = 1;
    while (c->tokens[c->token_pos].type != TK_EOF && paren_depth > 0) {
        if (c->tokens[c->token_pos].type == TK_LPAREN) paren_depth++;
        else if (c->tokens[c->token_pos].type == TK_RPAREN) {
            paren_depth--;
            if (paren_depth == 0) break;
        }
        c->token_pos++;
    }
    size_t step_tok_end = c->token_pos;
    if (c->tokens[c->token_pos].type == TK_RPAREN) c->token_pos++;

    zero_cc_parse_stmt(c);

    if (step_tok_end > step_tok_start) {
        size_t saved_pos = c->token_pos;
        c->token_pos = step_tok_start;
        zero_cc_parse_stmt(c);
        if (c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
        c->token_pos = saved_pos;
    }

    zero_cc_emit_u8(&c->text_section, 0xE9);
    int32_t back_rel = (int32_t)(cond_start - (c->text_section.length + 4));
    zero_cc_emit_u32(&c->text_section, (uint32_t)back_rel);

    if (has_cond) {
        int32_t exit_rel = (int32_t)(c->text_section.length - (jz_pos + 4));
        memcpy(c->text_section.data + jz_pos, &exit_rel, 4);
    }
}

#endif /* ZERO_CC_STATEMENTS_CONTROL_H */
