/* Type Definitions: typedef struct, typedef enum, standalone enum */
#ifndef ZERO_CC_STATEMENTS_DEFINITIONS_H
#define ZERO_CC_STATEMENTS_DEFINITIONS_H

/* Typedef Struct Definition: typedef struct { ... } Name; */
static inline void zero_cc_stmt_typedef_struct(ZeroCCCompiler *c) {
    c->token_pos++; /* consume TK_TYPEDEF */
    if (c->tokens[c->token_pos].type != TK_STRUCT) return;
    c->token_pos++; /* consume TK_STRUCT */

    char st_name[128] = "";
    if (c->tokens[c->token_pos].type == TK_IDENT) {
        snprintf(st_name, sizeof(st_name), "%s", c->tokens[c->token_pos].str_val);
        c->token_pos++;
    }

    ZeroCCStructDef st;
    memset(&st, 0, sizeof(st));
    size_t current_offset = 0;
    size_t member_align = 0;
    size_t base_size = 0;
    size_t base_align = 0;

    if (c->tokens[c->token_pos].type == TK_LBRACE) {
        c->token_pos++;
        while (c->tokens[c->token_pos].type != TK_RBRACE && c->tokens[c->token_pos].type != TK_EOF) {
            size_t mt = c->tokens[c->token_pos].type;
            if (mt == TK_ALIGNAS) {
                c->token_pos++;
                if (c->tokens[c->token_pos].type == TK_LPAREN) {
                    c->token_pos++;
                    int64_t align_val = zero_cc_eval_const_expr(c);
                    if (align_val > 0) member_align = (size_t)align_val;
                    if (c->tokens[c->token_pos].type == TK_RPAREN) c->token_pos++;
                }
            }
            if (mt == TK_STATIC_ASSERT) {
                c->token_pos++;
                if (c->tokens[c->token_pos].type != TK_LPAREN) {
                    c->had_error = 1;
                    snprintf(c->error_msg, sizeof(c->error_msg), "Expected '(' after _Static_assert");
                    return;
                }
                c->token_pos++;
                int64_t val = zero_cc_eval_const_expr(c);
                if (c->tokens[c->token_pos].type == TK_COMMA) {
                    c->token_pos++;
                    if (c->tokens[c->token_pos].type == TK_STRING) c->token_pos++;
                    if (c->tokens[c->token_pos].type != TK_RPAREN) {
                        c->had_error = 1;
                        snprintf(c->error_msg, sizeof(c->error_msg), "Expected ')' after message");
                        return;
                    }
                    c->token_pos++;
                } else if (c->tokens[c->token_pos].type == TK_RPAREN) {
                    c->token_pos++;
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
                c->token_pos++;
                if (val == 0) {
                    c->had_error = 1;
                    snprintf(c->error_msg, sizeof(c->error_msg), "Static assertion failed");
                    return;
                }
                continue;
            }
            if (mt == TK_IDENT || mt == TK_INT || mt == TK_CHAR || mt == TK_VOID ||
                mt == TK_FLOAT_KW || mt == TK_DOUBLE_KW || mt == TK_BOOL_KW ||
                mt == TK_SIGNED || mt == TK_UNSIGNED || mt == TK_SHORT || mt == TK_LONG ||
                mt == TK_STRUCT || mt == TK_ENUM || mt == TK_UNION || mt == TK_ATOMIC) {
                bool is_ptr = false;
                bool is_static = false;
                ZeroCCTokType base_type = mt;
                c->token_pos++;

                if (mt == TK_ATOMIC) {
                    if (c->tokens[c->token_pos].type == TK_LPAREN) {
                        c->token_pos++;
                        if (c->tokens[c->token_pos].type == TK_INT) { base_type = TK_INT; c->token_pos++; }
                        else if (c->tokens[c->token_pos].type == TK_CHAR) { base_type = TK_CHAR; c->token_pos++; }
                        else if (c->tokens[c->token_pos].type == TK_VOID) { base_type = TK_VOID; c->token_pos++; }
                        else if (c->tokens[c->token_pos].type == TK_FLOAT_KW) { base_type = TK_FLOAT_KW; c->token_pos++; }
                        else if (c->tokens[c->token_pos].type == TK_DOUBLE_KW) { base_type = TK_DOUBLE_KW; c->token_pos++; }
                        else if (c->tokens[c->token_pos].type == TK_BOOL_KW) { base_type = TK_BOOL_KW; c->token_pos++; }
                        else if (c->tokens[c->token_pos].type == TK_SHORT) { base_type = TK_SHORT; c->token_pos++; }
                        else if (c->tokens[c->token_pos].type == TK_LONG) { base_type = TK_LONG; c->token_pos++; }
                        else if (c->tokens[c->token_pos].type == TK_SIGNED) { base_type = TK_SIGNED; c->token_pos++; }
                        else if (c->tokens[c->token_pos].type == TK_UNSIGNED) { base_type = TK_UNSIGNED; c->token_pos++; }
                        else if (c->tokens[c->token_pos].type == TK_IDENT) { base_type = TK_IDENT; c->token_pos++; }
                        else { base_type = TK_INT; }
                        if (c->tokens[c->token_pos].type == TK_RPAREN) c->token_pos++;
                    }
                }

                while (c->tokens[c->token_pos].type == TK_STAR) {
                    is_ptr = true;
                    c->token_pos++;
                }
                if (c->tokens[c->token_pos].type == TK_STATIC) {
                    is_static = true;
                    c->token_pos++;
                }

                if (mt == TK_CHAR || mt == TK_BOOL_KW) { base_align = 1; base_size = 1; }
                else if (mt == TK_SHORT) { base_align = 2; base_size = 2; }
                else if (mt == TK_INT || mt == TK_FLOAT_KW) { base_align = 4; base_size = 4; }
                else if (mt == TK_LONG || mt == TK_DOUBLE_KW || mt == TK_VOID) { base_align = 8; base_size = 8; }
                if (is_ptr) { base_align = 8; base_size = 8; }

                size_t effective_align = member_align > 0 ? member_align : base_align;
                if (effective_align > st.align) st.align = effective_align;

                if (c->tokens[c->token_pos].type == TK_IDENT) {
                    char mem_name[128];
                    snprintf(mem_name, sizeof(mem_name), "%s", c->tokens[c->token_pos].str_val);
                    c->token_pos++;

                    if (c->tokens[c->token_pos].type == TK_LBRACKET) {
                        c->token_pos++;
                        if (c->tokens[c->token_pos].type == TK_NUM) {
                            size_t arr_count = (size_t)c->tokens[c->token_pos].num_val;
                            c->token_pos++;
                            if (c->tokens[c->token_pos].type == TK_RBRACKET) c->token_pos++;
                            base_size *= arr_count;
                            current_offset = (current_offset + effective_align - 1) & ~(effective_align - 1);
                            st.members[st.member_count].name = strdup(mem_name);
                            st.members[st.member_count].offset = current_offset;
                            st.members[st.member_count].size = base_size;
                            st.members[st.member_count].align = effective_align;
                            st.member_count++;
                            current_offset += base_size;
                        }
                    } else {
                        current_offset = (current_offset + effective_align - 1) & ~(effective_align - 1);
                        st.members[st.member_count].name = strdup(mem_name);
                        st.members[st.member_count].offset = current_offset;
                        st.members[st.member_count].size = base_size;
                        st.members[st.member_count].align = effective_align;
                        st.member_count++;
                        current_offset += base_size;
                    }
                }
                if (is_static) {
                    while (c->tokens[c->token_pos].type != TK_SEMI && c->tokens[c->token_pos].type != TK_EOF) {
                        c->token_pos++;
                    }
                }
                if (c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
            } else {
                while (c->tokens[c->token_pos].type != TK_SEMI && c->tokens[c->token_pos].type != TK_EOF) c->token_pos++;
                if (c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
            }
        }
        st.size = (current_offset + st.align - 1) & ~(st.align - 1);
        if (st.align == 0) st.align = 1;
        if (c->tokens[c->token_pos].type == TK_RBRACE) c->token_pos++;

        if (c->tokens[c->token_pos].type == TK_IDENT) {
            char alias[128];
            snprintf(alias, sizeof(alias), "%s", c->tokens[c->token_pos].str_val);
            c->token_pos++;
            zero_cc_register_struct(c, alias, st);
            zero_cc_register_typedef(c, alias, st);
        }
        if (c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
    }
}

/* Typedef Enum Definition: typedef enum { ... } Name; */
static inline void zero_cc_stmt_typedef_enum(ZeroCCCompiler *c) {
    c->token_pos++; /* consume TK_TYPEDEF */
    if (c->tokens[c->token_pos].type != TK_ENUM) return;
    c->token_pos++; /* consume TK_ENUM */

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
    }
}

/* Scalar/Primitive Typedef: typedef unsigned long size_t; */
static inline void zero_cc_stmt_typedef_scalar(ZeroCCCompiler *c) {
    c->token_pos++; /* consume TK_TYPEDEF */

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
}

/* Standalone Enum Declaration: enum [tag] { ... }; */
static inline void zero_cc_stmt_enum(ZeroCCCompiler *c) {
    c->token_pos++; /* consume TK_ENUM */
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
        if (c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
    }
}

#endif /* ZERO_CC_STATEMENTS_DEFINITIONS_H */
