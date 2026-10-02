/* Type Definitions: typedef struct, typedef enum, standalone enum, scalar typedef */
#ifndef ZERO_CC_STATEMENTS_DEFINITIONS_H
#define ZERO_CC_STATEMENTS_DEFINITIONS_H

#include "zero_cc/statements.h"

/* Forward declaration */
/* Typedef or Standalone Struct/Union Definition */
static inline void zero_cc_stmt_typedef_struct(ZeroCCCompiler *c) {
    bool is_typedef = false;
    if (c->tokens[c->token_pos].type == TK_TYPEDEF) {
        is_typedef = true;
        c->token_pos++; /* consume TK_TYPEDEF */
    }
    bool is_union = (c->tokens[c->token_pos].type == TK_UNION);
    if (c->tokens[c->token_pos].type == TK_STRUCT || is_union) {
        c->token_pos++;
        char st_name[128] = "";
        if (c->tokens[c->token_pos].type == TK_IDENT) {
            snprintf(st_name, sizeof(st_name), "%s", c->tokens[c->token_pos].str_val);
            c->token_pos++;
        }
        if (c->tokens[c->token_pos].type == TK_LBRACE) {
            c->token_pos++;
            ZeroCCType st; memset(&st, 0, sizeof(st));
            st.tag = is_union ? TYPE_UNION : TYPE_STRUCT;
            snprintf(st.struct_name, sizeof(st.struct_name), "%s", st_name);
            size_t current_offset = 0;
            size_t max_size = 0;
            st.align = 1;
            size_t bitfield_storage_offset = 0;
            size_t bitfield_storage_size = 0;
            size_t bitfield_bit_offset = 0;
            bool in_bitfield = false;
            while (c->tokens[c->token_pos].type != TK_RBRACE && c->tokens[c->token_pos].type != TK_EOF) {
                size_t member_align = 0;
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
                    mt == TK_SIGNED || mt == TK_UNSIGNED || mt == TK_SHORT || mt == TK_LONG || mt == TK_ATOMIC) {
                    bool is_unsigned = (mt == TK_UNSIGNED || mt == TK_BOOL_KW);
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
                            else if (c->tokens[c->token_pos].type == TK_UNSIGNED) { mt = TK_UNSIGNED; is_unsigned = true; c->token_pos++; }
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
                            else if (c->tokens[c->token_pos].type == TK_UNSIGNED) { mt = TK_UNSIGNED; is_unsigned = true; c->token_pos++; }
                            else if (c->tokens[c->token_pos].type == TK_IDENT) { mt = TK_IDENT; c->token_pos++; }
                            else { mt = TK_INT; }
                        }
                    }
                    c->token_pos++;
                    if (mt == TK_UNSIGNED) {
                        is_unsigned = true;
                        if (c->tokens[c->token_pos].type == TK_INT) { mt = TK_INT; c->token_pos++; }
                        else if (c->tokens[c->token_pos].type == TK_CHAR) { mt = TK_CHAR; c->token_pos++; }
                        else if (c->tokens[c->token_pos].type == TK_SHORT) { mt = TK_SHORT; c->token_pos++; }
                        else if (c->tokens[c->token_pos].type == TK_LONG) { mt = TK_LONG; c->token_pos++; }
                        else { mt = TK_INT; }
                    } else if (mt == TK_SIGNED) {
                        is_unsigned = false;
                        if (c->tokens[c->token_pos].type == TK_INT) { mt = TK_INT; c->token_pos++; }
                        else if (c->tokens[c->token_pos].type == TK_CHAR) { mt = TK_CHAR; c->token_pos++; }
                        else if (c->tokens[c->token_pos].type == TK_SHORT) { mt = TK_SHORT; c->token_pos++; }
                        else if (c->tokens[c->token_pos].type == TK_LONG) { mt = TK_LONG; c->token_pos++; }
                        else { mt = TK_INT; }
                    }
                    bool is_ptr = false;
                    while (c->tokens[c->token_pos].type == TK_STAR) {
                        is_ptr = true;
                        c->token_pos++;
                    }
                    size_t base_align = 8, base_size = 8;
                    if (mt == TK_CHAR || mt == TK_BOOL_KW) { base_align = 1; base_size = 1; }
                    else if (mt == TK_SHORT) { base_align = 2; base_size = 2; }
                    else if (mt == TK_INT || mt == TK_FLOAT_KW) { base_align = 4; base_size = 4; }
                    else if (mt == TK_LONG || mt == TK_DOUBLE_KW || mt == TK_VOID) { base_align = 8; base_size = 8; }
                    if (is_ptr) { base_align = 8; base_size = 8; }
                    size_t effective_align = member_align > base_align ? member_align : base_align;
                    if (effective_align > st.align) st.align = effective_align;

                    char mname[128] = "";
                    if (c->tokens[c->token_pos].type == TK_IDENT) {
                        snprintf(mname, sizeof(mname), "%s", c->tokens[c->token_pos].str_val);
                        c->token_pos++;
                    }

                    size_t elem_count = 1;
                    bool is_flexible = false;
                    if (c->tokens[c->token_pos].type == TK_LBRACKET) {
                        c->token_pos++;
                        if (c->tokens[c->token_pos].type == TK_NUM) {
                            elem_count = (size_t)c->tokens[c->token_pos].num_val;
                            c->token_pos++;
                        } else if (c->tokens[c->token_pos].type == TK_RBRACKET) {
                            /* Flexible array member: empty brackets [] */
                            is_flexible = true;
                            elem_count = 0;
                        }
                        if (c->tokens[c->token_pos].type == TK_RBRACKET) c->token_pos++;
                    }

                    bool is_bitfield = false;
                    uint8_t bit_width = 0;
                    if (c->tokens[c->token_pos].type == TK_COLON) {
                        is_bitfield = true;
                        c->token_pos++;
                        if (c->tokens[c->token_pos].type == TK_NUM) {
                            bit_width = (uint8_t)c->tokens[c->token_pos].num_val;
                            c->token_pos++;
                        }
                    }

                    if (is_union) {
                        if (st.member_count < 32 && mname[0] != '\0') {
                            ZeroCCStructMember *m = &st.members[st.member_count++];
                            snprintf(m->name, sizeof(m->name), "%s", mname);
                            m->offset = 0;
                            m->size = is_bitfield ? base_size : (base_size * elem_count);
                            m->align = effective_align;
                            m->is_flexible = false;
                            m->is_bitfield = is_bitfield;
                            m->bit_offset = 0;
                            m->bit_width = is_bitfield ? bit_width : (uint8_t)(base_size * 8);
                            m->is_signed = !is_unsigned;
                            if (mt == TK_CHAR || mt == TK_BOOL_KW) m->tag = TYPE_INT8;
                            else if (mt == TK_SHORT) m->tag = TYPE_INT16;
                            else if (mt == TK_INT || mt == TK_FLOAT_KW) m->tag = TYPE_INT32;
                            else if (mt == TK_LONG || mt == TK_DOUBLE_KW || mt == TK_VOID) m->tag = TYPE_INT64;
                            else if (mt == TK_IDENT) m->tag = TYPE_STRUCT;
                            else m->tag = TYPE_INT32;
                            if (is_ptr) m->tag = TYPE_PTR;
                            if (m->size > max_size) max_size = m->size;
                        }
                    } else {
                        if (is_bitfield) {
                            if (bit_width == 0) {
                                /* Zero-width bit-field: force alignment to next storage boundary */
                                if (in_bitfield) {
                                    current_offset = bitfield_storage_offset + bitfield_storage_size;
                                    in_bitfield = false;
                                }
                                current_offset = (current_offset + effective_align - 1) & ~(effective_align - 1);
                            } else {
                                if (in_bitfield && bitfield_storage_size == base_size &&
                                    (bitfield_bit_offset + bit_width <= base_size * 8)) {
                                    /* Fits in current storage unit */
                                    size_t m_offset = bitfield_storage_offset;
                                    uint8_t m_bit_off = (uint8_t)bitfield_bit_offset;
                                    bitfield_bit_offset += bit_width;
                                    if (mname[0] != '\0' && st.member_count < 32) {
                                        ZeroCCStructMember *m = &st.members[st.member_count++];
                                        snprintf(m->name, sizeof(m->name), "%s", mname);
                                        m->offset = m_offset;
                                        m->size = base_size;
                                        m->align = effective_align;
                                        m->is_flexible = false;
                                        m->is_bitfield = true;
                                        m->bit_offset = m_bit_off;
                                        m->bit_width = bit_width;
                                        m->is_signed = !is_unsigned;
                                        if (mt == TK_CHAR || mt == TK_BOOL_KW) m->tag = TYPE_INT8;
                                        else if (mt == TK_SHORT) m->tag = TYPE_INT16;
                                        else if (mt == TK_INT || mt == TK_FLOAT_KW) m->tag = TYPE_INT32;
                                        else if (mt == TK_LONG || mt == TK_DOUBLE_KW || mt == TK_VOID) m->tag = TYPE_INT64;
                                        else m->tag = TYPE_INT32;
                                    }
                                } else {
                                    /* Allocate new storage unit */
                                    if (in_bitfield) {
                                        current_offset = bitfield_storage_offset + bitfield_storage_size;
                                    }
                                    current_offset = (current_offset + effective_align - 1) & ~(effective_align - 1);
                                    bitfield_storage_offset = current_offset;
                                    bitfield_storage_size = base_size;
                                    bitfield_bit_offset = bit_width;
                                    in_bitfield = true;
                                    if (mname[0] != '\0' && st.member_count < 32) {
                                        ZeroCCStructMember *m = &st.members[st.member_count++];
                                        snprintf(m->name, sizeof(m->name), "%s", mname);
                                        m->offset = bitfield_storage_offset;
                                        m->size = base_size;
                                        m->align = effective_align;
                                        m->is_flexible = false;
                                        m->is_bitfield = true;
                                        m->bit_offset = 0;
                                        m->bit_width = bit_width;
                                        m->is_signed = !is_unsigned;
                                        if (mt == TK_CHAR || mt == TK_BOOL_KW) m->tag = TYPE_INT8;
                                        else if (mt == TK_SHORT) m->tag = TYPE_INT16;
                                        else if (mt == TK_INT || mt == TK_FLOAT_KW) m->tag = TYPE_INT32;
                                        else if (mt == TK_LONG || mt == TK_DOUBLE_KW || mt == TK_VOID) m->tag = TYPE_INT64;
                                        else m->tag = TYPE_INT32;
                                    }
                                }
                            }
                        } else {
                            /* Regular member */
                            if (in_bitfield) {
                                current_offset = bitfield_storage_offset + bitfield_storage_size;
                                in_bitfield = false;
                            }
                            current_offset = (current_offset + effective_align - 1) & ~(effective_align - 1);
                            if (mname[0] != '\0' && st.member_count < 32) {
                                ZeroCCStructMember *m = &st.members[st.member_count++];
                                snprintf(m->name, sizeof(m->name), "%s", mname);
                                m->offset = current_offset;
                                m->size = is_flexible ? 0 : (base_size * elem_count);
                                m->align = effective_align;
                                m->is_flexible = is_flexible;
                                m->is_bitfield = false;
                                m->bit_offset = 0;
                                m->bit_width = 0;
                                m->is_signed = !is_unsigned;
                                if (mt == TK_CHAR || mt == TK_BOOL_KW) m->tag = TYPE_INT8;
                                else if (mt == TK_SHORT) m->tag = TYPE_INT16;
                                else if (mt == TK_INT || mt == TK_FLOAT_KW) m->tag = TYPE_INT32;
                                else if (mt == TK_LONG || mt == TK_DOUBLE_KW || mt == TK_VOID) m->tag = TYPE_INT64;
                                else if (mt == TK_IDENT) m->tag = TYPE_STRUCT;
                                else m->tag = TYPE_INT32;
                                if (is_ptr) m->tag = TYPE_PTR;
                                if (!is_flexible) {
                                    current_offset += m->size;
                                }
                            }
                        }
                    }
                }
                if (c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
            }
            if (in_bitfield) {
                current_offset = bitfield_storage_offset + bitfield_storage_size;
                in_bitfield = false;
            }
            if (is_union) {
                st.size = (max_size + st.align - 1) & ~(st.align - 1);
            } else {
                st.size = (current_offset + st.align - 1) & ~(st.align - 1);
            }
            if (st.align == 0) st.align = 1;
            if (c->tokens[c->token_pos].type == TK_RBRACE) c->token_pos++;
            if (st_name[0] != '\0') {
                zero_cc_register_struct(c, st_name, st);
            }
            if (c->tokens[c->token_pos].type == TK_IDENT) {
                char alias[128];
                snprintf(alias, sizeof(alias), "%s", c->tokens[c->token_pos].str_val);
                c->token_pos++;
                zero_cc_register_struct(c, alias, st);
                if (is_typedef) {
                    zero_cc_register_typedef(c, alias, st);
                }
            }
            if (c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
            return;
        }
    }
    if (c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
}

/* Typedef Enum Definition */
static inline void zero_cc_stmt_typedef_enum(ZeroCCCompiler *c) {
    c->token_pos++; /* consume TK_TYPEDEF */
    if (c->tokens[c->token_pos].type == TK_ENUM) {
        c->token_pos++;
        char en_name[128] = "";
        if (c->tokens[c->token_pos].type == TK_IDENT) {
            snprintf(en_name, sizeof(en_name), "%s", c->tokens[c->token_pos].str_val);
            c->token_pos++;
        }
        if (c->tokens[c->token_pos].type == TK_LBRACE) {
            c->token_pos++;
            ZeroCCType et; memset(&et, 0, sizeof(et));
            et.tag = TYPE_INT32;
            et.size = 4;
            et.align = 4;
            snprintf(et.struct_name, sizeof(et.struct_name), "%s", en_name);
            int64_t enum_val = 0;
            while (c->tokens[c->token_pos].type != TK_RBRACE && c->tokens[c->token_pos].type != TK_EOF) {
                if (c->tokens[c->token_pos].type == TK_IDENT) {
                    char ename[128];
                    snprintf(ename, sizeof(ename), "%s", c->tokens[c->token_pos].str_val);
                    c->token_pos++;
                    if (c->tokens[c->token_pos].type == TK_ASSIGN) {
                        c->token_pos++;
                        enum_val = zero_cc_eval_const_expr(c);
                    }
                    zero_cc_register_enum_const(c, ename, enum_val);
                    enum_val++;
                }
                if (c->tokens[c->token_pos].type == TK_COMMA) c->token_pos++;
            }
            if (c->tokens[c->token_pos].type == TK_RBRACE) c->token_pos++;
            if (c->tokens[c->token_pos].type == TK_IDENT) {
                char alias[128];
                snprintf(alias, sizeof(alias), "%s", c->tokens[c->token_pos].str_val);
                c->token_pos++;
                zero_cc_register_typedef(c, alias, et);
            }
            if (c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
            return;
        }
    }
    if (c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
}

/* Standalone Enum Declaration */
static inline void zero_cc_stmt_enum(ZeroCCCompiler *c) {
    c->token_pos++; /* consume TK_ENUM */
    char en_name[128] = "";
    if (c->tokens[c->token_pos].type == TK_IDENT) {
        snprintf(en_name, sizeof(en_name), "%s", c->tokens[c->token_pos].str_val);
        c->token_pos++;
    }
    if (c->tokens[c->token_pos].type == TK_LBRACE) {
        c->token_pos++;
        ZeroCCType et; memset(&et, 0, sizeof(et));
        et.tag = TYPE_INT32;
        et.size = 4;
        et.align = 4;
        snprintf(et.struct_name, sizeof(et.struct_name), "%s", en_name);
        int64_t enum_val = 0;
        while (c->tokens[c->token_pos].type != TK_RBRACE && c->tokens[c->token_pos].type != TK_EOF) {
            if (c->tokens[c->token_pos].type == TK_IDENT) {
                char ename[128];
                snprintf(ename, sizeof(ename), "%s", c->tokens[c->token_pos].str_val);
                c->token_pos++;
                if (c->tokens[c->token_pos].type == TK_ASSIGN) {
                    c->token_pos++;
                    enum_val = zero_cc_eval_const_expr(c);
                }
                zero_cc_register_enum_const(c, ename, enum_val);
                enum_val++;
            }
            if (c->tokens[c->token_pos].type == TK_COMMA) c->token_pos++;
        }
        if (c->tokens[c->token_pos].type == TK_RBRACE) c->token_pos++;
        if (c->tokens[c->token_pos].type == TK_IDENT) {
            char alias[128];
            snprintf(alias, sizeof(alias), "%s", c->tokens[c->token_pos].str_val);
            c->token_pos++;
            zero_cc_register_typedef(c, alias, et);
        }
        if (c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
        return;
    }
    if (c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
}

/* Scalar Typedef */
static inline void zero_cc_stmt_typedef_scalar(ZeroCCCompiler *c) {
    c->token_pos++; /* consume TK_TYPEDEF */
    ZeroCCTokType t = c->tokens[c->token_pos].type;
    ZeroCCType ttype; memset(&ttype, 0, sizeof(ttype));
    if (t == TK_INT) { ttype = zero_cc_type_int(); c->token_pos++; }
    else if (t == TK_CHAR) { ttype = zero_cc_type_char(); c->token_pos++; }
    else if (t == TK_VOID) { ttype = zero_cc_type_void(); c->token_pos++; }
    else if (t == TK_FLOAT_KW) { ttype = zero_cc_type_float(); c->token_pos++; }
    else if (t == TK_DOUBLE_KW) { ttype = zero_cc_type_double(); c->token_pos++; }
    else if (t == TK_BOOL_KW) { ttype = zero_cc_type_bool(); c->token_pos++; }
    else if (t == TK_SHORT) { ttype = zero_cc_type_int16(); c->token_pos++; }
    else if (t == TK_LONG) { ttype = zero_cc_type_int64(); c->token_pos++; }
    else if (t == TK_SIGNED) { c->token_pos++; ttype = zero_cc_type_int(); }
    else if (t == TK_UNSIGNED) { c->token_pos++; ttype = zero_cc_type_int(); }
    else if (t == TK_IDENT) {
        const char *tname = c->tokens[c->token_pos].str_val;
        c->token_pos++;
        ZeroCCType *found = zero_cc_find_typedef(c, tname);
        if (found) { ttype = *found; }
        else { ttype = zero_cc_type_int(); }
    } else {
        ttype = zero_cc_type_int();
    }
    bool is_ptr = false;
    while (c->tokens[c->token_pos].type == TK_STAR) { is_ptr = true; c->token_pos++; }
    if (is_ptr) { ttype = zero_cc_type_ptr(&ttype); }
    if (c->tokens[c->token_pos].type == TK_IDENT) {
        char alias[128];
        snprintf(alias, sizeof(alias), "%s", c->tokens[c->token_pos].str_val);
        c->token_pos++;
        zero_cc_register_typedef(c, alias, ttype);
    }
    if (c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
}

#endif /* ZERO_CC_STATEMENTS_DEFINITIONS_H */
