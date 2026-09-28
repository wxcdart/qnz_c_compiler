#ifndef ZERO_CC_STATEMENTS_DEFINITIONS_H
#define ZERO_CC_STATEMENTS_DEFINITIONS_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Statement Parser */
#ifndef ZERO_CC_STATEMENTS_H
#define ZERO_CC_STATEMENTS_H

static inline void zero_cc_parse_stmt(ZeroCCCompiler *c) {
    size_t decl_align_spec = 0;

    /* Skip C23 attributes: [[ ... ]] */
    while (c->tokens[c->token_pos].type == TK_LDBRACKET) {
        c->token_pos++;
        int depth = 1;
        while (c->tokens[c->token_pos].type != TK_EOF && depth > 0) {
            if (c->tokens[c->token_pos].type == TK_LDBRACKET) depth++;
            else if (c->tokens[c->token_pos].type == TK_RDBRACKET) depth--;
            c->token_pos++;
        }
    }

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
            if (align_val > 0 && (decl_align_spec == 0 || (size_t)align_val > decl_align_spec)) {
                decl_align_spec = (size_t)align_val;
            }
            if (c->tokens[c->token_pos].type == TK_RPAREN) c->token_pos++;
        }
        t = c->tokens[c->token_pos].type;
    }
    /* Skip storage/constness specifiers: constexpr, const, static, extern, register */
    while (t == TK_CONSTEXPR || t == TK_CONST || t == TK_STATIC || t == TK_EXTERN || t == TK_REGISTER) {
        c->token_pos++;
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
            ZeroCCTokType decl_type_tok = t;
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
                    if (align_val > 0 && (decl_align_spec == 0 || (size_t)align_val > decl_align_spec)) {
                        decl_align_spec = (size_t)align_val;
                    }
                    if (c->tokens[c->token_pos].type == TK_RPAREN) c->token_pos++;
                }
            }
            if (c->tokens[c->token_pos].type == TK_IDENT) {
                char var_name[128];
                snprintf(var_name, sizeof(var_name), "%s", c->tokens[c->token_pos].str_val);
                c->token_pos++;

                ZeroCCType vtype;
                if (found_type) {
                    vtype = *found_type;
                } else if (decl_type_tok == TK_CHAR || decl_type_tok == TK_BOOL_KW) {
                    vtype = zero_cc_type_char();
                } else if (decl_type_tok == TK_SHORT) {
                    vtype = zero_cc_type_int16();
                } else if (decl_type_tok == TK_INT) {
                    vtype = zero_cc_type_int();
                } else if (decl_type_tok == TK_FLOAT_KW) {
                    vtype = zero_cc_type_float();
                } else if (decl_type_tok == TK_DOUBLE_KW) {
                    vtype = zero_cc_type_double();
                } else if (decl_type_tok == TK_VOID) {
                    vtype = zero_cc_type_void();
                } else {
                    vtype = zero_cc_type_int64();
                }
                if (is_ptr) {
                    vtype = zero_cc_type_ptr(&vtype);
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

#endif /* ZERO_CC_STATEMENTS_DEFINITIONS_H */
