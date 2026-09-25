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
                st.align = 1;
                while (c->tokens[c->token_pos].type != TK_RBRACE && c->tokens[c->token_pos].type != TK_EOF) {
                    size_t member_align = 8;
                    // Handle leading _Alignas for struct member
                    if (c->tokens[c->token_pos].type == TK_ALIGNAS) {
                        c->token_pos++;
                        if (c->tokens[c->token_pos].type == TK_LPAREN) {
                            c->token_pos++;
                            int64_t align_val = zero_cc_eval_const_expr(c);
                            if (align_val > 0) member_align = (size_t)align_val;
                            if (c->tokens[c->token_pos].type == TK_RPAREN) c->token_pos++;
                        }
                    }
                    ZeroCCTokType mt = c->tokens[c->token_pos].type;
                    // Handle _Static_assert inside struct
                    if (mt == TK_STATIC_ASSERT) {
                        c->token_pos++; /* consume TK_STATIC_ASSERT */
                        if (c->tokens[c->token_pos].type != TK_LPAREN) {
                            c->had_error = 1;
                            snprintf(c->error_msg, sizeof(c->error_msg), "Expected '(' after _Static_assert");
                            return;
                        }
                        c->token_pos++; /* skip '(' */
                        int64_t val = zero_cc_eval_const_expr(c);
                        // After expression: either ',' (message) or ')' (no message)
                        if (c->tokens[c->token_pos].type == TK_COMMA) {
                            c->token_pos++; /* skip ',' */
                            if (c->tokens[c->token_pos].type == TK_STRING) {
                                // We could use the message, but just skip it
                                c->token_pos++;
                            }
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
                        continue;
                    }
                    if (mt == TK_IDENT || mt == TK_INT || mt == TK_CHAR || mt == TK_VOID ||
                        mt == TK_FLOAT_KW || mt == TK_DOUBLE_KW || mt == TK_BOOL_KW ||
                        mt == TK_SIGNED || mt == TK_UNSIGNED || mt == TK_SHORT || mt == TK_LONG || mt == TK_ATOMIC) {
                        if (mt == TK_ATOMIC) {
                            c->token_pos++;
                            if (c->tokens[c->token_pos].type == TK_LPAREN) {
                                c->token_pos++;
                                if (c->tokens[c->token_pos].type == TK_INT) { mt = TK_INT; c->token_pos++; }
                                else if (c->tokens[c->token_pos].type == TK_CHAR) { mt = TK_CHAR; c->token_pos++; }
                                else if (c->tokens[c->token_pos].type == TK_VOID) { mt = TK_VOID; c->token_pos++; }
                                else if (c->tokens[c->token_pos].type == TK_FLOAT_KW) { mt = TK_FLOAT_KW; c->token_pos++; }
                                else if (c->tokens[c->token_pos].type == TK_DOUBLE_KW) { mt = TK_DOUBLE_KW; c->token_pos++; }
                                else if (c->tokens[c->token_pos].type == TK_BOOL_KW) { mt = TK_BOOL_KW; c->token_pos++; }
                                else if (c->tokens[c->token_pos].type == TK_SHORT) { mt = TK_SHORT; c->token_pos++; }
                                else if (c->tokens[c->token_pos].type == TK_LONG) { mt = TK_LONG; c->token_pos++; }
                                else if (c->tokens[c->token_pos].type == TK_SIGNED) { mt = TK_SIGNED; c->token_pos++; }
                                else if (c->tokens[c->token_pos].type == TK_UNSIGNED) { mt = TK_UNSIGNED; c->token_pos++; }
                                else if (c->tokens[c->token_pos].type == TK_IDENT) { mt = TK_IDENT; c->token_pos++; }
                                else { mt = TK_INT; }
                                if (c->tokens[c->token_pos].type == TK_RPAREN) c->token_pos++;
                            } else {
                                if (c->tokens[c->token_pos].type == TK_INT) { mt = TK_INT; c->token_pos++; }
                                else if (c->tokens[c->token_pos].type == TK_CHAR) { mt = TK_CHAR; c->token_pos++; }
                                else if (c->tokens[c->token_pos].type == TK_VOID) { mt = TK_VOID; c->token_pos++; }
                                else if (c->tokens[c->token_pos].type == TK_FLOAT_KW) { mt = TK_FLOAT_KW; c->token_pos++; }
                                else if (c->tokens[c->token_pos].type == TK_DOUBLE_KW) { mt = TK_DOUBLE_KW; c->token_pos++; }
                                else if (c->tokens[c->token_pos].type == TK_BOOL_KW) { mt = TK_BOOL_KW; c->token_pos++; }
                                else if (c->tokens[c->token_pos].type == TK_SHORT) { mt = TK_SHORT; c->token_pos++; }
                                else if (c->tokens[c->token_pos].type == TK_LONG) { mt = TK_LONG; c->token_pos++; }
                                else if (c->tokens[c->token_pos].type == TK_SIGNED) { mt = TK_SIGNED; c->token_pos++; }
                                else if (c->tokens[c->token_pos].type == TK_UNSIGNED) { mt = TK_UNSIGNED; c->token_pos++; }
                                else if (c->tokens[c->token_pos].type == TK_IDENT) { mt = TK_IDENT; c->token_pos++; }
                                else { mt = TK_INT; }
                            }
                        }
                        const char *mem_type_name = c->tokens[c->token_pos].str_val;
                        (void)mem_type_name;
                        c->token_pos++;
                        bool is_ptr = false;
                        while (c->tokens[c->token_pos].type == TK_STAR) {
                            is_ptr = true;
                            c->token_pos++;
                        }
                        // Determine base alignment from type
                        size_t base_align = 8;
                        size_t base_size = 8;
                        if (mt == TK_CHAR || mt == TK_BOOL_KW) { base_align = 1; base_size = 1; }
                        else if (mt == TK_SHORT) { base_align = 2; base_size = 2; }
                        else if (mt == TK_INT || mt == TK_FLOAT_KW) { base_align = 4; base_size = 4; }
                        else if (mt == TK_LONG || mt == TK_DOUBLE_KW || mt == TK_VOID) { base_align = 8; base_size = 8; }
                        if (is_ptr) { base_align = 8; base_size = 8; }
                        size_t effective_align = member_align > base_align ? member_align : base_align;
                        if (effective_align > st.align) st.align = effective_align;
                        // Align current offset
                        current_offset = (current_offset + effective_align - 1) & ~(effective_align - 1);
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
                                m->size = base_size * elem_count;
                                m->align = effective_align;
                                if (mt == TK_CHAR || mt == TK_BOOL_KW) m->tag = TYPE_INT8;
                                else if (mt == TK_SHORT) m->tag = TYPE_INT16;
                                else if (mt == TK_INT || mt == TK_FLOAT_KW) m->tag = TYPE_INT32;
                                else if (mt == TK_LONG || mt == TK_DOUBLE_KW || mt == TK_VOID) m->tag = TYPE_INT64;
                                else if (mt == TK_IDENT) m->tag = TYPE_STRUCT; /* typedef struct */
                                else m->tag = TYPE_INT32;
                                if (is_ptr) m->tag = TYPE_PTR;
                                current_offset += m->size;
                            }
                        }
                    }
                    if (c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
                }
                // Final struct size aligned to struct alignment
                st.size = (current_offset + st.align - 1) & ~(st.align - 1);
                if (st.align == 0) st.align = 1;
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
    /* Handle leading _Alignas specifiers */
    while (t == TK_ALIGNAS) {
        c->token_pos++;
        if (c->tokens[c->token_pos].type == TK_LPAREN) {
            c->token_pos++;
            int64_t align_val = zero_cc_eval_const_expr(c);
            if (align_val > 0 && (decl_align_spec == 0 || align_val > decl_align_spec)) {
                decl_align_spec = align_val;
            }
            if (c->tokens[c->token_pos].type == TK_RPAREN) c->token_pos++;
        }
        t = c->tokens[c->token_pos].type;
    }
    if (t == TK_IDENT || t == TK_INT || t == TK_CHAR || t == TK_VOID || t == TK_STRUCT ||
        t == TK_FLOAT_KW || t == TK_DOUBLE_KW || t == TK_BOOL_KW ||
        t == TK_SIGNED || t == TK_UNSIGNED || t == TK_SHORT || t == TK_LONG || t == TK_AUTO || t == TK_ATOMIC) {
        const char *tname = c->tokens[c->token_pos].str_val;
        ZeroCCType *found_type = zero_cc_find_typedef(c, tname);
        if (found_type || t == TK_INT || t == TK_CHAR || t == TK_VOID || t == TK_STRUCT ||
            t == TK_FLOAT_KW || t == TK_DOUBLE_KW || t == TK_BOOL_KW ||
            t == TK_SIGNED || t == TK_UNSIGNED || t == TK_SHORT || t == TK_LONG || t == TK_AUTO || t == TK_ATOMIC) {
            bool is_auto = (t == TK_AUTO);
            bool is_atomic_decl = (t == TK_ATOMIC);
            c->token_pos++;

            /* Handle _Atomic for variable declarations */
            if (is_atomic_decl) {
                if (c->tokens[c->token_pos].type == TK_LPAREN) {
                    c->token_pos++;
                    if (c->tokens[c->token_pos].type == TK_INT) { t = TK_INT; c->token_pos++; }
                    else if (c->tokens[c->token_pos].type == TK_CHAR) { t = TK_CHAR; c->token_pos++; }
                    else if (c->tokens[c->token_pos].type == TK_VOID) { t = TK_VOID; c->token_pos++; }
                    else if (c->tokens[c->token_pos].type == TK_FLOAT_KW) { t = TK_FLOAT_KW; c->token_pos++; }
                    else if (c->tokens[c->token_pos].type == TK_DOUBLE_KW) { t = TK_DOUBLE_KW; c->token_pos++; }
                    else if (c->tokens[c->token_pos].type == TK_BOOL_KW) { t = TK_BOOL_KW; c->token_pos++; }
                    else if (c->tokens[c->token_pos].type == TK_SHORT) { t = TK_SHORT; c->token_pos++; }
                    else if (c->tokens[c->token_pos].type == TK_LONG) { t = TK_LONG; c->token_pos++; }
                    else if (c->tokens[c->token_pos].type == TK_SIGNED) { t = TK_SIGNED; c->token_pos++; }
                    else if (c->tokens[c->token_pos].type == TK_UNSIGNED) { t = TK_UNSIGNED; c->token_pos++; }
                    else if (c->tokens[c->token_pos].type == TK_IDENT) { t = TK_IDENT; c->token_pos++; }
                    else { t = TK_INT; }
                    if (c->tokens[c->token_pos].type == TK_RPAREN) c->token_pos++;
                } else {
                    if (c->tokens[c->token_pos].type == TK_INT) { t = TK_INT; c->token_pos++; }
                    else if (c->tokens[c->token_pos].type == TK_CHAR) { t = TK_CHAR; c->token_pos++; }
                    else if (c->tokens[c->token_pos].type == TK_VOID) { t = TK_VOID; c->token_pos++; }
                    else if (c->tokens[c->token_pos].type == TK_FLOAT_KW) { t = TK_FLOAT_KW; c->token_pos++; }
                    else if (c->tokens[c->token_pos].type == TK_DOUBLE_KW) { t = TK_DOUBLE_KW; c->token_pos++; }
                    else if (c->tokens[c->token_pos].type == TK_BOOL_KW) { t = TK_BOOL_KW; c->token_pos++; }
                    else if (c->tokens[c->token_pos].type == TK_SHORT) { t = TK_SHORT; c->token_pos++; }
                    else if (c->tokens[c->token_pos].type == TK_LONG) { t = TK_LONG; c->token_pos++; }
                    else if (c->tokens[c->token_pos].type == TK_SIGNED) { t = TK_SIGNED; c->token_pos++; }
                    else if (c->tokens[c->token_pos].type == TK_UNSIGNED) { t = TK_UNSIGNED; c->token_pos++; }
                    else if (c->tokens[c->token_pos].type == TK_IDENT) { t = TK_IDENT; c->token_pos++; }
                    else { t = TK_INT; }
                }
                tname = c->tokens[c->token_pos - 1].str_val;
                found_type = zero_cc_find_typedef(c, tname);
            }

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
            // Handle _Alignas between type and declarator
            while (c->tokens[c->token_pos].type == TK_ALIGNAS) {
                c->token_pos++;
                if (c->tokens[c->token_pos].type == TK_LPAREN) {
                    c->token_pos++;
                    int64_t align_val = zero_cc_eval_const_expr(c);
                    if (align_val > 0 && (decl_align_spec == 0 || align_val > decl_align_spec)) {
                        decl_align_spec = align_val;
                    }
                    if (c->tokens[c->token_pos].type == TK_RPAREN) c->token_pos++;
                }
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
                if (is_atomic_decl) {
                    vtype.is_atomic = true;
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

    /* Static Assert Declaration */
    if (t == TK_STATIC_ASSERT) {
        c->token_pos++; // consume TK_STATIC_ASSERT
        // expect '('
        if (c->tokens[c->token_pos].type != TK_LPAREN) {
            c->had_error = 1;
            snprintf(c->error_msg, sizeof(c->error_msg), "Expected '(' after _Static_assert");
            return;
        }
        c->token_pos++; // skip '('
        // parse constant expression
        int64_t val = zero_cc_eval_const_expr(c);
        // After expression: either ',' (message) or ')' (no message)
        if (c->tokens[c->token_pos].type == TK_COMMA) {
            c->token_pos++; // skip ','
            if (c->tokens[c->token_pos].type == TK_STRING) {
                c->token_pos++; // skip message
            }
            if (c->tokens[c->token_pos].type != TK_RPAREN) {
                c->had_error = 1;
                snprintf(c->error_msg, sizeof(c->error_msg), "Expected ')' after message");
                return;
            }
            c->token_pos++; // skip ')'
        } else if (c->tokens[c->token_pos].type == TK_RPAREN) {
            c->token_pos++; // skip ')'
        } else {
            c->had_error = 1;
            snprintf(c->error_msg, sizeof(c->error_msg), "Expected ')' or ',' after constant expression");
            return;
        }
        // expect ';'
        if (c->tokens[c->token_pos].type != TK_SEMI) {
            c->had_error = 1;
            snprintf(c->error_msg, sizeof(c->error_msg), "Expected ';' after _Static_assert");
            return;
        }
        c->token_pos++; // consume ';'
        if (val == 0) {
            c->had_error = 1;
            snprintf(c->error_msg, sizeof(c->error_msg), "Static assertion failed");
            return;
        }
        return;
    }
    /* Switch Statement: switch (expr) { case V: ... break; default: ... } */
    if (t == TK_SWITCH) {
        c->token_pos++;
        if (c->tokens[c->token_pos].type == TK_LPAREN) c->token_pos++;
        zero_cc_parse_expr(c); /* switch value in rax */
        if (c->tokens[c->token_pos].type == TK_RPAREN) c->token_pos++;

        /* push switch value to stack */
        zero_cc_emit_u8(&c->text_section, 0x50); /* push rax */

        if (c->tokens[c->token_pos].type == TK_LBRACE) c->token_pos++;

        size_t break_patches[128];
        size_t break_count = 0;
        size_t next_case_patch = 0;
        bool has_pending_case_patch = false;

        while (c->tokens[c->token_pos].type != TK_RBRACE && c->tokens[c->token_pos].type != TK_EOF) {
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

                /* If there was a previous case check jump, patch it here */
                if (has_pending_case_patch) {
                    int32_t rel = (int32_t)(c->text_section.length - (next_case_patch + 4));
                    memcpy(c->text_section.data + next_case_patch, &rel, 4);
                }

                /* cmp qword ptr [rsp], case_val */
                zero_cc_emit_u8(&c->text_section, 0x48);
                zero_cc_emit_u8(&c->text_section, 0x81);
                zero_cc_emit_u8(&c->text_section, 0x3C);
                zero_cc_emit_u8(&c->text_section, 0x24);
                zero_cc_emit_u32(&c->text_section, (uint32_t)case_val);

                /* jne next_case */
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
                /* jmp switch_exit */
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

        /* Back-patch break jumps */
        for (size_t b = 0; b < break_count; b++) {
            int32_t rel = (int32_t)(c->text_section.length - (break_patches[b] + 4));
            memcpy(c->text_section.data + break_patches[b], &rel, 4);
        }

        /* pop switch value from stack */
        zero_cc_emit_u8(&c->text_section, 0x58); /* pop rax */
        return;
    }

    /* While Loop */
    if (t == TK_WHILE) {
        c->token_pos++;
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
        return;
    }

    /* Do-While Loop: do { body } while (cond); */
    if (t == TK_DO) {
        c->token_pos++;
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
        return;
    }

    /* For Loop: for (init; cond; step) body */
    if (t == TK_FOR) {
        c->token_pos++;
        if (c->tokens[c->token_pos].type == TK_LPAREN) c->token_pos++;

        /* 1. Init clause */
        if (c->tokens[c->token_pos].type != TK_SEMI) {
            zero_cc_parse_stmt(c);
        } else {
            c->token_pos++;
        }

        /* 2. Condition clause */
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

        /* 3. Step clause tokens */
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

        /* 4. Body statement */
        zero_cc_parse_stmt(c);

        /* 5. Emit Step clause */
        if (step_tok_end > step_tok_start) {
            size_t saved_pos = c->token_pos;
            c->token_pos = step_tok_start;
            zero_cc_parse_stmt(c);
            if (c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
            c->token_pos = saved_pos;
        }

        /* 6. Jump back to cond */
        zero_cc_emit_u8(&c->text_section, 0xE9);
        int32_t back_rel = (int32_t)(cond_start - (c->text_section.length + 4));
        zero_cc_emit_u32(&c->text_section, (uint32_t)back_rel);

        /* 7. Back-patch exit */
        if (has_cond) {
            int32_t exit_rel = (int32_t)(c->text_section.length - (jz_pos + 4));
            memcpy(c->text_section.data + jz_pos, &exit_rel, 4);
        }
        return;
    }

    /* Struct Member Post-Increment / Post-Decrement: ptr->member++; or ptr->member--; */
    if (t == TK_IDENT && c->tokens[c->token_pos + 1].type == TK_ARROW &&
        c->tokens[c->token_pos + 2].type == TK_IDENT &&
        (c->tokens[c->token_pos + 3].type == TK_INC || c->tokens[c->token_pos + 3].type == TK_DEC)) {
        char ptr_name[128];
        snprintf(ptr_name, sizeof(ptr_name), "%s", c->tokens[c->token_pos].str_val);
        char mem_name[128];
        snprintf(mem_name, sizeof(mem_name), "%s", c->tokens[c->token_pos + 2].str_val);
        bool is_inc = (c->tokens[c->token_pos + 3].type == TK_INC);
        c->token_pos += 4;

        ZeroCCSym *sym = zero_cc_find_local(c, ptr_name);
        if (sym) {
            ZeroCCType *st = sym->type.base ? sym->type.base : zero_cc_find_struct(c, sym->type.struct_name);
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
            /* mov rcx, [rbp - sym_offset] */
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0x8B);
            zero_cc_emit_u8(&c->text_section, 0x8D);
            zero_cc_emit_u32(&c->text_section, (uint32_t)(-sym->stack_offset));

            if (is_inc) {
                /* add qword ptr [rcx + offset], 1 */
                zero_cc_emit_u8(&c->text_section, 0x48);
                zero_cc_emit_u8(&c->text_section, 0x83);
                zero_cc_emit_u8(&c->text_section, 0x81);
                zero_cc_emit_u32(&c->text_section, (uint32_t)offset);
                zero_cc_emit_u8(&c->text_section, 0x01);
            } else {
                /* sub qword ptr [rcx + offset], 1 */
                zero_cc_emit_u8(&c->text_section, 0x48);
                zero_cc_emit_u8(&c->text_section, 0x83);
                zero_cc_emit_u8(&c->text_section, 0xA9);
                zero_cc_emit_u32(&c->text_section, (uint32_t)offset);
                zero_cc_emit_u8(&c->text_section, 0x01);
            }
        }
        if (c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
        return;
    }

    /* Variable Post-Increment / Post-Decrement: x++; or x--; */
    if (t == TK_IDENT && (c->tokens[c->token_pos + 1].type == TK_INC || c->tokens[c->token_pos + 1].type == TK_DEC)) {
        char var_name[128];
        snprintf(var_name, sizeof(var_name), "%s", c->tokens[c->token_pos].str_val);
        bool is_inc = (c->tokens[c->token_pos + 1].type == TK_INC);
        c->token_pos += 2;

        ZeroCCSym *sym = zero_cc_find_local(c, var_name);
        if (sym) {
            if (is_inc) {
                /* add qword ptr [rbp - offset], 1 */
                zero_cc_emit_u8(&c->text_section, 0x48);
                zero_cc_emit_u8(&c->text_section, 0x83);
                zero_cc_emit_u8(&c->text_section, 0x85);
                zero_cc_emit_u32(&c->text_section, (uint32_t)(-sym->stack_offset));
                zero_cc_emit_u8(&c->text_section, 0x01);
            } else {
                /* sub qword ptr [rbp - offset], 1 */
                zero_cc_emit_u8(&c->text_section, 0x48);
                zero_cc_emit_u8(&c->text_section, 0x83);
                zero_cc_emit_u8(&c->text_section, 0xAD);
                zero_cc_emit_u32(&c->text_section, (uint32_t)(-sym->stack_offset));
                zero_cc_emit_u8(&c->text_section, 0x01);
            }
        }
        if (c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
        return;
    }

    /* Struct Member Array Assignment: sym.member[index] = expr; or ptr->member[index] = expr; */
    if (t == TK_IDENT && (c->tokens[c->token_pos + 1].type == TK_DOT || c->tokens[c->token_pos + 1].type == TK_ARROW) &&
        c->tokens[c->token_pos + 2].type == TK_IDENT && c->tokens[c->token_pos + 3].type == TK_LBRACKET) {
        char base_name[128];
        snprintf(base_name, sizeof(base_name), "%s", c->tokens[c->token_pos].str_val);
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

