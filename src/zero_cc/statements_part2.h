        bool is_arrow = (c->tokens[c->token_pos + 1].type == TK_ARROW);
        char mem_name[128];
        snprintf(mem_name, sizeof(mem_name), "%s", c->tokens[c->token_pos + 2].str_val);
        c->token_pos += 4;

        zero_cc_parse_expr(c); /* index in rax */
        if (c->tokens[c->token_pos].type == TK_RBRACKET) c->token_pos++;

        /* shl rax, 2 */
        zero_cc_emit_u8(&c->text_section, 0x48);
        zero_cc_emit_u8(&c->text_section, 0xC1);
        zero_cc_emit_u8(&c->text_section, 0xE0);
        zero_cc_emit_u8(&c->text_section, 0x02);

        /* push scaled index */
        zero_cc_emit_u8(&c->text_section, 0x50);

        ZeroCCSym *sym = zero_cc_find_local(c, base_name);
        size_t offset = 0;
        if (sym) {
            ZeroCCType *st = sym->type.base ? sym->type.base : zero_cc_find_struct(c, sym->type.struct_name);
            if (!st) st = zero_cc_find_typedef(c, sym->type.struct_name);
            if (st) {
                for (size_t m = 0; m < st->member_count; m++) {
                    if (strcmp(st->members[m].name, mem_name) == 0) {
                        offset = st->members[m].offset;
                        break;
                    }
                }
            }
            if (!is_arrow) {
                /* Local struct instance: member pointer is at [rbp - (sym_offset - offset)] */
                int32_t eff_offset = (int32_t)sym->stack_offset - (int32_t)offset;
                /* mov rcx, [rbp - eff_offset] (loads pointer member into rcx) */
                zero_cc_emit_u8(&c->text_section, 0x48);
                zero_cc_emit_u8(&c->text_section, 0x8B);
                zero_cc_emit_u8(&c->text_section, 0x8D);
                zero_cc_emit_u32(&c->text_section, (uint32_t)(-eff_offset));
            } else {
                /* Pointer to struct: arr->data[idx] */
                /* mov rcx, [rbp - sym_offset] (base ptr) */
                zero_cc_emit_u8(&c->text_section, 0x48);
                zero_cc_emit_u8(&c->text_section, 0x8B);
                zero_cc_emit_u8(&c->text_section, 0x8D);
                zero_cc_emit_u32(&c->text_section, (uint32_t)(-sym->stack_offset));

                /* mov rcx, [rcx + offset] (load data pointer member) */
                zero_cc_emit_u8(&c->text_section, 0x48);
                zero_cc_emit_u8(&c->text_section, 0x8B);
                zero_cc_emit_u8(&c->text_section, 0x89);
                zero_cc_emit_u32(&c->text_section, (uint32_t)offset);
            }
        }

        /* pop scaled index to rax; add rax, rcx */
        zero_cc_emit_u8(&c->text_section, 0x58);
        zero_cc_emit_u8(&c->text_section, 0x48);
        zero_cc_emit_u8(&c->text_section, 0x01);
        zero_cc_emit_u8(&c->text_section, 0xC8);

        /* push destination element address */
        zero_cc_emit_u8(&c->text_section, 0x50);

        if (c->tokens[c->token_pos].type == TK_ASSIGN) {
            c->token_pos++;
            zero_cc_parse_expr(c); /* value in rax */
            /* pop destination address to rcx */
            zero_cc_emit_u8(&c->text_section, 0x59);
            /* mov [rcx], eax */
            zero_cc_emit_u8(&c->text_section, 0x89);
            zero_cc_emit_u8(&c->text_section, 0x01);
        }
        if (c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
        return;
    }

    /* Local Variable Array Assignment: sym[index] = expr; */
    if (t == TK_IDENT && c->tokens[c->token_pos + 1].type == TK_LBRACKET) {
        char var_name[128];
        snprintf(var_name, sizeof(var_name), "%s", c->tokens[c->token_pos].str_val);
        c->token_pos += 2;
        zero_cc_parse_expr(c); /* index in rax */
        if (c->tokens[c->token_pos].type == TK_RBRACKET) c->token_pos++;

        /* shl rax, 2 */
        zero_cc_emit_u8(&c->text_section, 0x48);
        zero_cc_emit_u8(&c->text_section, 0xC1);
        zero_cc_emit_u8(&c->text_section, 0xE0);
        zero_cc_emit_u8(&c->text_section, 0x02);

        ZeroCCSym *sym = zero_cc_find_local(c, var_name);
        if (sym) {
            /* mov rcx, [rbp - sym_offset]; add rax, rcx */
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0x8B);
            zero_cc_emit_u8(&c->text_section, 0x8D);
            zero_cc_emit_u32(&c->text_section, (uint32_t)(-sym->stack_offset));

            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0x01);
            zero_cc_emit_u8(&c->text_section, 0xC8);
        }

        /* push destination address */
        zero_cc_emit_u8(&c->text_section, 0x50);

        if (c->tokens[c->token_pos].type == TK_ASSIGN) {
            c->token_pos++;
            zero_cc_parse_expr(c); /* value in rax */
            /* pop destination address to rcx */
            zero_cc_emit_u8(&c->text_section, 0x59);
            /* mov [rcx], eax */
            zero_cc_emit_u8(&c->text_section, 0x89);
            zero_cc_emit_u8(&c->text_section, 0x01);
        }
        if (c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
        return;
    }

    /* Assignment to Struct Member: sym.member = expr; or ptr->member = expr; */
    if (t == TK_IDENT && (c->tokens[c->token_pos + 1].type == TK_ARROW || c->tokens[c->token_pos + 1].type == TK_DOT)) {
        char ptr_name[128];
        snprintf(ptr_name, sizeof(ptr_name), "%s", c->tokens[c->token_pos].str_val);
        bool is_arrow = (c->tokens[c->token_pos + 1].type == TK_ARROW);
        c->token_pos += 2;
        if (c->tokens[c->token_pos].type == TK_IDENT) {
            char mem_name[128];
            snprintf(mem_name, sizeof(mem_name), "%s", c->tokens[c->token_pos].str_val);
            c->token_pos++;

            size_t arr_index_expr = 0;
            bool member_has_idx = false;
            if (c->tokens[c->token_pos].type == TK_LBRACKET) {
                member_has_idx = true;
                c->token_pos++;
                if (c->tokens[c->token_pos].type == TK_NUM) {
                    arr_index_expr = (size_t)c->tokens[c->token_pos].num_val;
                    c->token_pos++;
                }
                if (c->tokens[c->token_pos].type == TK_RBRACKET) c->token_pos++;
            }

            ZeroCCTokType assign_op = c->tokens[c->token_pos].type;
            if (assign_op == TK_ASSIGN || assign_op == TK_ADD_ASSIGN || assign_op == TK_SUB_ASSIGN ||
                assign_op == TK_MUL_ASSIGN || assign_op == TK_DIV_ASSIGN) {
                c->token_pos++;
                zero_cc_parse_expr(c); /* value in rax */

                ZeroCCSym *sym = zero_cc_find_local(c, ptr_name);
                if (sym) {
                    ZeroCCType *st = NULL;
                    if (sym->type.tag == TYPE_STRUCT) st = &sym->type;
                    else if (sym->type.tag == TYPE_PTR && sym->type.base) st = sym->type.base;
                    if (!st) st = zero_cc_find_struct(c, sym->type.struct_name);
                    if (!st) st = zero_cc_find_typedef(c, sym->type.struct_name);
                    size_t offset = 0;
                    if (st) {
                        for (size_t m = 0; m < st->member_count; m++) {
                            if (strcmp(st->members[m].name, mem_name) == 0) {
                                offset = st->members[m].offset;
                                break;
                            }
                        }
                    }
                    if (member_has_idx) {
                        offset += (arr_index_expr * 4); /* 4-byte scalar scaling */
                    }

                    if (!is_arrow) {
                        /* Direct local struct offset: [rbp - (sym_offset - offset)] */
                        int32_t eff_offset = (int32_t)sym->stack_offset - (int32_t)offset;
                        if (assign_op == TK_ADD_ASSIGN) {
                            zero_cc_emit_u8(&c->text_section, 0x48);
                            zero_cc_emit_u8(&c->text_section, 0x01);
                            zero_cc_emit_u8(&c->text_section, 0x85);
                            zero_cc_emit_u32(&c->text_section, (uint32_t)(-eff_offset));
                        } else if (assign_op == TK_SUB_ASSIGN) {
                            zero_cc_emit_u8(&c->text_section, 0x48);
                            zero_cc_emit_u8(&c->text_section, 0x29);
                            zero_cc_emit_u8(&c->text_section, 0x85);
                            zero_cc_emit_u32(&c->text_section, (uint32_t)(-eff_offset));
                        } else {
                            zero_cc_emit_u8(&c->text_section, 0x48);
                            zero_cc_emit_u8(&c->text_section, 0x89);
                            zero_cc_emit_u8(&c->text_section, 0x85);
                            zero_cc_emit_u32(&c->text_section, (uint32_t)(-eff_offset));
                        }
                    } else {
                        /* mov rcx, [rbp - sym_offset] (base ptr in rcx) */
                        zero_cc_emit_u8(&c->text_section, 0x48);
                        zero_cc_emit_u8(&c->text_section, 0x8B);
                        zero_cc_emit_u8(&c->text_section, 0x8D);
                        zero_cc_emit_u32(&c->text_section, (uint32_t)(-sym->stack_offset));

                        if (assign_op == TK_ADD_ASSIGN) {
                            /* add [rcx + offset], rax */
                            zero_cc_emit_u8(&c->text_section, 0x48);
                            zero_cc_emit_u8(&c->text_section, 0x01);
                            zero_cc_emit_u8(&c->text_section, 0x81);
                            zero_cc_emit_u32(&c->text_section, (uint32_t)offset);
                        } else if (assign_op == TK_SUB_ASSIGN) {
                            /* sub [rcx + offset], rax */
                            zero_cc_emit_u8(&c->text_section, 0x48);
                            zero_cc_emit_u8(&c->text_section, 0x29);
                            zero_cc_emit_u8(&c->text_section, 0x81);
                            zero_cc_emit_u32(&c->text_section, (uint32_t)offset);
                        } else {
                            /* mov [rcx + offset], rax */
                            zero_cc_emit_u8(&c->text_section, 0x48);
                            zero_cc_emit_u8(&c->text_section, 0x89);
                            zero_cc_emit_u8(&c->text_section, 0x81);
                            zero_cc_emit_u32(&c->text_section, (uint32_t)offset);
                        }
                    }
                }
            }
        }
        if (c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
        return;
    }

    /* Pointer Dereference Assignment: *p = expr; */
    if (t == TK_STAR && c->tokens[c->token_pos + 1].type == TK_IDENT && c->tokens[c->token_pos + 2].type == TK_ASSIGN) {
        char var_name[128];
        snprintf(var_name, sizeof(var_name), "%s", c->tokens[c->token_pos + 1].str_val);
        c->token_pos += 3;
        zero_cc_parse_expr(c);
        ZeroCCSym *sym = zero_cc_find_local(c, var_name);
        if (sym) {
            /* mov rcx, [rbp - offset]  ; load pointer value */
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0x8B);
            zero_cc_emit_u8(&c->text_section, 0x8D);
            zero_cc_emit_u32(&c->text_section, (uint32_t)(-sym->stack_offset));
            /* mov [rcx], eax */
            zero_cc_emit_u8(&c->text_section, 0x89);
            zero_cc_emit_u8(&c->text_section, 0x01);
        }
        if (c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
        return;
    }

    /* Variable Assignment: x = expr; or x += expr; or x -= expr; */
    if (t == TK_IDENT && (c->tokens[c->token_pos + 1].type == TK_ASSIGN ||
                          c->tokens[c->token_pos + 1].type == TK_ADD_ASSIGN ||
                          c->tokens[c->token_pos + 1].type == TK_SUB_ASSIGN)) {
        char var_name[128];
        snprintf(var_name, sizeof(var_name), "%s", c->tokens[c->token_pos].str_val);
        ZeroCCTokType aop = c->tokens[c->token_pos + 1].type;
        c->token_pos += 2;
        zero_cc_parse_expr(c);

        ZeroCCSym *sym = zero_cc_find_local(c, var_name);
        if (sym) {
            if (aop == TK_ADD_ASSIGN) {
                /* add [rbp - offset], rax */
                zero_cc_emit_u8(&c->text_section, 0x48);
                zero_cc_emit_u8(&c->text_section, 0x01);
                zero_cc_emit_u8(&c->text_section, 0x85);
                zero_cc_emit_u32(&c->text_section, (uint32_t)(-sym->stack_offset));
            } else if (aop == TK_SUB_ASSIGN) {
                /* sub [rbp - offset], rax */
                zero_cc_emit_u8(&c->text_section, 0x48);
                zero_cc_emit_u8(&c->text_section, 0x29);
                zero_cc_emit_u8(&c->text_section, 0x85);
                zero_cc_emit_u32(&c->text_section, (uint32_t)(-sym->stack_offset));
            } else {
                /* mov [rbp - offset], rax */
                zero_cc_emit_u8(&c->text_section, 0x48);
                zero_cc_emit_u8(&c->text_section, 0x89);
                zero_cc_emit_u8(&c->text_section, 0x85);
                zero_cc_emit_u32(&c->text_section, (uint32_t)(-sym->stack_offset));
                if (sym->type.tag == TYPE_STRUCT && sym->type.size > 8) {
                    /* mov [rbp - (sym_offset - 8)], rdx */
                    int32_t off2 = (int32_t)sym->stack_offset - 8;
                    zero_cc_emit_u8(&c->text_section, 0x48);
                    zero_cc_emit_u8(&c->text_section, 0x89);
                    zero_cc_emit_u8(&c->text_section, 0x95);
                    zero_cc_emit_u32(&c->text_section, (uint32_t)(-off2));
                }
            }
        }
        if (c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
        return;
    }

    /* General Expression Statement */
    zero_cc_parse_expr(c);
    if (c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
}

#endif /* ZERO_CC_STATEMENTS_H */

