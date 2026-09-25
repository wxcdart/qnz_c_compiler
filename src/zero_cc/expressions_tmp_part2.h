        zero_cc_emit_u8(&c->text_section, 0x58); /* pop rax (LHS in rax) */

        if (op == TK_PLUS) {
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0x01);
            zero_cc_emit_u8(&c->text_section, 0xC8);
        } else if (op == TK_MINUS) {
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0x29);
            zero_cc_emit_u8(&c->text_section, 0xC8);
        } else if (op == TK_STAR) {
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0x0F);
            zero_cc_emit_u8(&c->text_section, 0xAF);
            zero_cc_emit_u8(&c->text_section, 0xC1);
        } else if (op == TK_SLASH) {
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0x99);
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0xF7);
            zero_cc_emit_u8(&c->text_section, 0xF9);
        } else if (op == TK_PERCENT) {
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0x99);
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0xF7);
            zero_cc_emit_u8(&c->text_section, 0xF9);
            /* result in rdx -> mov rax, rdx */
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0x89);
            zero_cc_emit_u8(&c->text_section, 0xD0);
        } else if (op == TK_AMP) {
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0x21);
            zero_cc_emit_u8(&c->text_section, 0xC8);
        } else if (op == TK_PIPE) {
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0x09);
            zero_cc_emit_u8(&c->text_section, 0xC8);
        } else if (op == TK_CARET) {
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0x31);
            zero_cc_emit_u8(&c->text_section, 0xC8);
        } else if (op == TK_SHL) {
            /* shl rax, cl */
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0xD3);
            zero_cc_emit_u8(&c->text_section, 0xE0);
        } else if (op == TK_SHR) {
            /* shr rax, cl */
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0xD3);
            zero_cc_emit_u8(&c->text_section, 0xE8);
        } else {
            /* Comparisons: rax is LHS, rcx is RHS. cmp rax, rcx computes (rax - rcx) */
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0x39);
            zero_cc_emit_u8(&c->text_section, 0xC8);
            zero_cc_emit_u8(&c->text_section, 0x0F);
            if (op == TK_EQ) zero_cc_emit_u8(&c->text_section, 0x94);
            else if (op == TK_NE) zero_cc_emit_u8(&c->text_section, 0x95);
            else if (op == TK_LT) zero_cc_emit_u8(&c->text_section, 0x9C);
            else if (op == TK_LE) zero_cc_emit_u8(&c->text_section, 0x9E);
            else if (op == TK_GT) zero_cc_emit_u8(&c->text_section, 0x9F);
            else if (op == TK_GE) zero_cc_emit_u8(&c->text_section, 0x9D);
            zero_cc_emit_u8(&c->text_section, 0xC0);
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0x0F);
            zero_cc_emit_u8(&c->text_section, 0xB6);
            zero_cc_emit_u8(&c->text_section, 0xC0);
        }
    }

    /* Ternary Conditional Operator: cond ? then_val : else_val */
    if (c->tokens[c->token_pos].type == TK_QUESTION) {
        c->token_pos++;
        /* test cond in rax: test rax, rax; jz else_branch */
        zero_cc_emit_u8(&c->text_section, 0x48);
        zero_cc_emit_u8(&c->text_section, 0x85);
        zero_cc_emit_u8(&c->text_section, 0xC0);
        zero_cc_emit_u8(&c->text_section, 0x0F);
        zero_cc_emit_u8(&c->text_section, 0x84);
        size_t jz_pos = c->text_section.length;
        zero_cc_emit_u32(&c->text_section, 0);

        zero_cc_parse_expr(c); /* then_val in rax */

        /* jmp end_pos */
        zero_cc_emit_u8(&c->text_section, 0xE9);
        size_t jmp_pos = c->text_section.length;
        zero_cc_emit_u32(&c->text_section, 0);

        int32_t else_rel = (int32_t)(c->text_section.length - (jz_pos + 4));
        memcpy(c->text_section.data + jz_pos, &else_rel, 4);

        if (c->tokens[c->token_pos].type == TK_COLON) c->token_pos++;
        zero_cc_parse_expr(c); /* else_val in rax */

        int32_t end_rel = (int32_t)(c->text_section.length - (jmp_pos + 4));
        memcpy(c->text_section.data + jmp_pos, &end_rel, 4);
    }
}

