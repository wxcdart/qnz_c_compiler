/* Statement Parser */
#ifndef ZERO_CC_STATEMENTS_H
#define ZERO_CC_STATEMENTS_H

static inline void zero_cc_parse_stmt(ZeroCCCompiler *c) {
    ZeroCCTokType t = c->tokens[c->token_pos].type;

    /* Compound Block: { ... } */
    if (t == TK_LBRACE) {
        c->token_pos++;
        while (c->tokens[c->token_pos].type != TK_RBRACE && c->tokens[c->token_pos].type != TK_EOF) {
            zero_cc_parse_stmt(c);
        }
        if (c->tokens[c->token_pos].type == TK_RBRACE) c->token_pos++;
        return;
    }

    /* Inline Assembly Statements: __asm__ (...) or __asm (...) or asm (...) */
    if (t == TK_IDENT && (strcmp(c->tokens[c->token_pos].str_val, "__asm__") == 0 ||
                          strcmp(c->tokens[c->token_pos].str_val, "asm") == 0 ||
                          strcmp(c->tokens[c->token_pos].str_val, "__asm") == 0)) {
        while (c->tokens[c->token_pos].type != TK_SEMI && c->tokens[c->token_pos].type != TK_EOF) {
            c->token_pos++;
        }
        if (c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
        return;
    }

    /* Typedef Struct or Enum Definition: typedef struct { ... } Name; or typedef enum { ... } Name; */
    if (t == TK_TYPEDEF) {
        c->token_pos++;
        if (c->tokens[c->token_pos].type == TK_STRUCT) {
            c->token_pos++;
            char st_name[128] = "";
            if (c->tokens[c->token_pos].type == TK_IDENT) {
                snprintf(st_name, sizeof(st_name), "%s", c->tokens[c->token_pos].str_val);
                c->token_pos++;
            }
            if (c->tokens[c->token_pos].type == TK_LBRACE) {
                c->token_pos++;
                ZeroCCType st; memset(&st, 0, sizeof(st));
                st.tag = TYPE_STRUCT;
                snprintf(st.struct_name, sizeof(st.struct_name), "%s", st_name);
                size_t current_offset = 0;

                while (c->tokens[c->token_pos].type != TK_RBRACE && c->tokens[c->token_pos].type != TK_EOF) {
                    ZeroCCTokType mt = c->tokens[c->token_pos].type;
                    if (mt == TK_IDENT || mt == TK_INT || mt == TK_CHAR || mt == TK_VOID ||
                        mt == TK_FLOAT_KW || mt == TK_DOUBLE_KW || mt == TK_BOOL_KW ||
                        mt == TK_SIGNED || mt == TK_UNSIGNED || mt == TK_SHORT || mt == TK_LONG) {
                        const char *mem_type_name = c->tokens[c->token_pos].str_val;
                        (void)mem_type_name;
                        c->token_pos++;
                        bool is_ptr = false;
                        while (c->tokens[c->token_pos].type == TK_STAR) {
                            is_ptr = true;
                            c->token_pos++;
                        }
                        if (c->tokens[c->token_pos].type == TK_IDENT) {
                            char mname[128];
                            snprintf(mname, sizeof(mname), "%s", c->tokens[c->token_pos].str_val);
                            c->token_pos++;
                            size_t elem_count = 1;
                            if (c->tokens[c->token_pos].type == TK_LBRACKET) {
                                c->token_pos++;
                                if (c->tokens[c->token_pos].type == TK_NUM) {
                                    elem_count = (size_t)c->tokens[c->token_pos].num_val;
                                    c->token_pos++;
                                }
                                if (c->tokens[c->token_pos].type == TK_RBRACKET) c->token_pos++;
                            }
                            if (st.member_count < 32) {
                                ZeroCCStructMember *m = &st.members[st.member_count++];
                                snprintf(m->name, sizeof(m->name), "%s", mname);
                                m->offset = current_offset;
                                m->size = (is_ptr ? 8 : 8) * elem_count;
                                current_offset += m->size;
                            }
                        }
                    }
                    if (c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
                }
                st.size = (current_offset + 7) & ~7;
                if (c->tokens[c->token_pos].type == TK_RBRACE) c->token_pos++;

                /* Alias name */
                if (c->tokens[c->token_pos].type == TK_IDENT) {
                    char alias[128];
                    snprintf(alias, sizeof(alias), "%s", c->tokens[c->token_pos].str_val);
                    c->token_pos++;
                    zero_cc_register_struct(c, alias, st);
                    zero_cc_register_typedef(c, alias, st);
                }
                if (c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
                return;
            }
        } else if (c->tokens[c->token_pos].type == TK_ENUM) {
            c->token_pos++;
            if (c->tokens[c->token_pos].type == TK_IDENT) c->token_pos++; /* optional tag */
            if (c->tokens[c->token_pos].type == TK_LBRACE) {
                c->token_pos++;
                int64_t curr_val = 0;
                while (c->tokens[c->token_pos].type != TK_RBRACE && c->tokens[c->token_pos].type != TK_EOF) {
                    if (c->tokens[c->token_pos].type == TK_IDENT) {
                        char ename[128];
                        snprintf(ename, sizeof(ename), "%s", c->tokens[c->token_pos].str_val);
                        c->token_pos++;
                        if (c->tokens[c->token_pos].type == TK_ASSIGN) {
                            c->token_pos++;
                            if (c->tokens[c->token_pos].type == TK_NUM) {
                                curr_val = c->tokens[c->token_pos].num_val;
                                c->token_pos++;
                            }
                        }
                        zero_cc_register_enum_const(c, ename, curr_val++);
                    }
                    if (c->tokens[c->token_pos].type == TK_COMMA) c->token_pos++;
                }
                if (c->tokens[c->token_pos].type == TK_RBRACE) c->token_pos++;
                if (c->tokens[c->token_pos].type == TK_IDENT) {
                    char alias[128];
                    snprintf(alias, sizeof(alias), "%s", c->tokens[c->token_pos].str_val);
                    c->token_pos++;
                    zero_cc_register_typedef(c, alias, zero_cc_type_int());
                }
                if (c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
                return;
            }
        } else {
            /* Scalar / Primitive / Pointer Typedef: e.g. typedef unsigned long size_t; typedef int int32_t; */
            bool is_ptr = false;
            while (c->tokens[c->token_pos].type == TK_INT || c->tokens[c->token_pos].type == TK_CHAR ||
                   c->tokens[c->token_pos].type == TK_VOID || c->tokens[c->token_pos].type == TK_FLOAT_KW ||
                   c->tokens[c->token_pos].type == TK_DOUBLE_KW || c->tokens[c->token_pos].type == TK_BOOL_KW ||
                   c->tokens[c->token_pos].type == TK_SIGNED || c->tokens[c->token_pos].type == TK_UNSIGNED ||
                   c->tokens[c->token_pos].type == TK_SHORT || c->tokens[c->token_pos].type == TK_LONG ||
                   c->tokens[c->token_pos].type == TK_STAR) {
                if (c->tokens[c->token_pos].type == TK_STAR) is_ptr = true;
                c->token_pos++;
            }
            if (c->tokens[c->token_pos].type == TK_IDENT) {
                char alias[128];
                snprintf(alias, sizeof(alias), "%s", c->tokens[c->token_pos].str_val);
                c->token_pos++;
                ZeroCCType t = is_ptr ? zero_cc_type_ptr(NULL) : zero_cc_type_int64();
                zero_cc_register_typedef(c, alias, t);
            }
            if (c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
            return;
        }
    }

    /* Standalone Enum Declaration: enum [tag] { ... }; */
    if (t == TK_ENUM) {
        c->token_pos++;
        if (c->tokens[c->token_pos].type == TK_IDENT) c->token_pos++;
        if (c->tokens[c->token_pos].type == TK_LBRACE) {
            c->token_pos++;
            int64_t curr_val = 0;
            while (c->tokens[c->token_pos].type != TK_RBRACE && c->tokens[c->token_pos].type != TK_EOF) {
                if (c->tokens[c->token_pos].type == TK_IDENT) {
                    char ename[128];
                    snprintf(ename, sizeof(ename), "%s", c->tokens[c->token_pos].str_val);
                    c->token_pos++;
                    if (c->tokens[c->token_pos].type == TK_ASSIGN) {
                        c->token_pos++;
                        if (c->tokens[c->token_pos].type == TK_NUM) {
                            curr_val = c->tokens[c->token_pos].num_val;
                            c->token_pos++;
                        }
                    }
                    zero_cc_register_enum_const(c, ename, curr_val++);
                }
                if (c->tokens[c->token_pos].type == TK_COMMA) c->token_pos++;
            }
            if (c->tokens[c->token_pos].type == TK_RBRACE) c->token_pos++;
            if (c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
            return;
        }
    }

    /* Variable Declaration: Type var = expr; or Type *ptr = expr; or auto var = expr; */
    if (t == TK_IDENT || t == TK_INT || t == TK_CHAR || t == TK_VOID || t == TK_STRUCT ||
        t == TK_FLOAT_KW || t == TK_DOUBLE_KW || t == TK_BOOL_KW ||
        t == TK_SIGNED || t == TK_UNSIGNED || t == TK_SHORT || t == TK_LONG || t == TK_AUTO) {
        const char *tname = c->tokens[c->token_pos].str_val;
        ZeroCCType *found_type = zero_cc_find_typedef(c, tname);
        if (found_type || t == TK_INT || t == TK_CHAR || t == TK_VOID || t == TK_STRUCT ||
            t == TK_FLOAT_KW || t == TK_DOUBLE_KW || t == TK_BOOL_KW ||
            t == TK_SIGNED || t == TK_UNSIGNED || t == TK_SHORT || t == TK_LONG || t == TK_AUTO) {
            bool is_auto = (t == TK_AUTO);
            c->token_pos++;

            /* Function pointer variable declaration: type (*name)(params) = init; */
            if (c->tokens[c->token_pos].type == TK_LPAREN && c->tokens[c->token_pos + 1].type == TK_STAR &&
                c->tokens[c->token_pos + 2].type == TK_IDENT) {
                char fn_ptr_name[128];
                snprintf(fn_ptr_name, sizeof(fn_ptr_name), "%s", c->tokens[c->token_pos + 2].str_val);
                c->token_pos += 3;
                if (c->tokens[c->token_pos].type == TK_RPAREN) c->token_pos++;
                if (c->tokens[c->token_pos].type == TK_LPAREN) {
                    while (c->tokens[c->token_pos].type != TK_RPAREN && c->tokens[c->token_pos].type != TK_EOF) {
                        c->token_pos++;
                    }
                    if (c->tokens[c->token_pos].type == TK_RPAREN) c->token_pos++;
                }

                ZeroCCSym *sym = zero_cc_add_local(c, fn_ptr_name, zero_cc_type_ptr(NULL));
                if (c->tokens[c->token_pos].type == TK_ASSIGN) {
                    c->token_pos++;
                    zero_cc_parse_expr(c);
                    /* mov [rbp - offset], rax */
                    zero_cc_emit_u8(&c->text_section, 0x48);
                    zero_cc_emit_u8(&c->text_section, 0x89);
                    zero_cc_emit_u8(&c->text_section, 0x85);
                    zero_cc_emit_u32(&c->text_section, (uint32_t)(-sym->stack_offset));
                }
                if (c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
                return;
            }

            bool is_ptr = false;
            while (c->tokens[c->token_pos].type == TK_STAR) {
                is_ptr = true;
                c->token_pos++;
            }
            if (c->tokens[c->token_pos].type == TK_IDENT) {
                char var_name[128];
                snprintf(var_name, sizeof(var_name), "%s", c->tokens[c->token_pos].str_val);
                c->token_pos++;

                ZeroCCType vtype;
                if (is_ptr) {
                    vtype = zero_cc_type_ptr(found_type);
                } else if (found_type) {
                    vtype = *found_type;
                } else {
                    vtype = zero_cc_type_int64();
                }
                if (found_type) {
                    snprintf(vtype.struct_name, sizeof(vtype.struct_name), "%s", tname);
                }
                ZeroCCSym *sym = zero_cc_add_local(c, var_name, vtype);

                if (c->tokens[c->token_pos].type == TK_ASSIGN) {
                    c->token_pos++;
                    if (c->tokens[c->token_pos].type == TK_LBRACE) {
                        c->token_pos++;
                        size_t m_idx = 0;
                        while (c->tokens[c->token_pos].type != TK_RBRACE && c->tokens[c->token_pos].type != TK_EOF) {
                            if (c->tokens[c->token_pos].type == TK_DOT && c->tokens[c->token_pos + 1].type == TK_IDENT) {
                                c->token_pos++;
                                const char *dname = c->tokens[c->token_pos].str_val;
                                c->token_pos++;
                                if (c->tokens[c->token_pos].type == TK_ASSIGN) c->token_pos++;
                                for (size_t k = 0; k < sym->type.member_count; ++k) {
                                    if (strcmp(sym->type.members[k].name, dname) == 0) {
                                        m_idx = k;
                                        break;
                                    }
                                }
                            }
                            zero_cc_parse_expr(c);
                            size_t off = (m_idx < sym->type.member_count) ? sym->type.members[m_idx].offset : (m_idx * 8);
                            int32_t target_disp = -(int32_t)(sym->stack_offset - off);
                            /* mov [rbp + target_disp], rax */
                            zero_cc_emit_u8(&c->text_section, 0x48);
                            zero_cc_emit_u8(&c->text_section, 0x89);
                            zero_cc_emit_u8(&c->text_section, 0x85);
                            zero_cc_emit_u32(&c->text_section, (uint32_t)target_disp);
                            m_idx++;
                            if (c->tokens[c->token_pos].type == TK_COMMA) c->token_pos++;
                        }
                        if (c->tokens[c->token_pos].type == TK_RBRACE) c->token_pos++;
                    } else {
                        zero_cc_parse_expr(c);
                        if (is_auto) {
                            sym->type = c->last_expr_is_float ? zero_cc_type_double() : zero_cc_type_int64();
                        }
                        if (c->last_expr_is_float) {
                            /* movsd [rbp - offset], xmm0 -> F2 0F 11 85 disp32 */
                            zero_cc_emit_u8(&c->text_section, 0xF2);
                            zero_cc_emit_u8(&c->text_section, 0x0F);
                            zero_cc_emit_u8(&c->text_section, 0x11);
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
                }
                if (c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
                return;
            }
        }
    }

    /* Return Statement: return <expr>; */
    if (t == TK_RETURN) {
        c->token_pos++;
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
        return;
    }

    /* If / Else Statement */
    if (t == TK_IF) {
        c->token_pos++;
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
        return;
    }

