/* Type Definitions: typedef struct, typedef enum, standalone enum, scalar typedef */
#ifndef ZERO_CC_PARSER_DEFINITIONS_H
#define ZERO_CC_PARSER_DEFINITIONS_H

#include "zero_cc/types/types.h"
#include "zero_cc/parser/const_eval.h"

/* Forward declaration */
static inline ZeroCCType zero_cc_parse_struct_body(ZeroCCCompiler *c, bool is_union);

/* Parse struct or union body { ... } */
static inline ZeroCCType zero_cc_parse_struct_body(ZeroCCCompiler *c, bool is_union) {
    ZeroCCType st; memset(&st, 0, sizeof(st));
    st.tag = is_union ? TYPE_UNION : TYPE_STRUCT;
    /* Resolve #pragma pack in effect at this token position */
    c->default_pack = 0;
    for (size_t pi = 0; pi < c->pack_ev_n && c->pack_ev_tok[pi] <= c->token_pos; pi++)
        c->default_pack = c->pack_ev_val[pi];
    size_t current_offset = 0;
    size_t max_size = 0;
    st.align = 1;
    size_t bitfield_storage_offset = 0;
    size_t bitfield_storage_size = 0;
    size_t bitfield_bit_offset = 0;
    bool in_bitfield = false;

    if (c->tokens[c->token_pos].type == TK_LBRACE) {
        c->token_pos++;
    }

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
                diag_report_error_at_token(c, "Expected '(' after _Static_assert");
                return st;
            }
            c->token_pos++;
            int64_t val = zero_cc_eval_const_expr(c);
            if (c->tokens[c->token_pos].type == TK_COMMA) {
                c->token_pos++;
                if (c->tokens[c->token_pos].type == TK_STRING) c->token_pos++;
                if (c->tokens[c->token_pos].type != TK_RPAREN) {
                    diag_report_error_at_token(c, "Expected ')' after message");
                    return st;
                }
                c->token_pos++;
            } else if (c->tokens[c->token_pos].type == TK_RPAREN) {
                c->token_pos++;
            } else {
                diag_report_error_at_token(c, "Expected ')' or ',' after constant expression");
                return st;
            }
            if (c->tokens[c->token_pos].type != TK_SEMI) {
                diag_report_error_at_token(c, "Expected ';' after _Static_assert");
                return st;
            }
            c->token_pos++;
            if (val == 0) {
                diag_report_error_at_token(c, "Static assertion failed");
                return st;
            }
            continue;
        }

        /* Nested struct/union/enum or anonymous member */
        if (mt == TK_STRUCT || mt == TK_UNION || mt == TK_ENUM) {
            bool is_enum = (mt == TK_ENUM);
            bool inner_is_union = (mt == TK_UNION);
            c->token_pos++;
            char inner_tag[128] = "";
            if (c->tokens[c->token_pos].type == TK_IDENT) {
                snprintf(inner_tag, sizeof(inner_tag), "%s", c->tokens[c->token_pos].str_val);
                c->token_pos++;
            }
            ZeroCCType inner_type;
            memset(&inner_type, 0, sizeof(inner_type));
            inner_type.tag = is_enum ? TYPE_ENUM : (inner_is_union ? TYPE_UNION : TYPE_STRUCT);
            inner_type.size = is_enum ? 4 : 0;
            inner_type.align = is_enum ? 4 : 1;
            if (inner_tag[0] != '\0') {
                snprintf(inner_type.struct_name, sizeof(inner_type.struct_name), "%s", inner_tag);
            }
            if (c->tokens[c->token_pos].type == TK_LBRACE) {
                if (is_enum) {
                    c->token_pos++;
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
                            if (enum_val == INT64_MAX) {
                                diag_report_error_at_token(c, "overflow in enumeration values");
                            } else {
                                enum_val++;
                            }
                        }
                        if (c->tokens[c->token_pos].type == TK_COMMA) c->token_pos++;
                    }
                    if (c->tokens[c->token_pos].type == TK_RBRACE) c->token_pos++;
                    if (inner_tag[0] != '\0') {
                        zero_cc_register_enum(c, inner_tag, inner_type);
                    }
                } else {
                    inner_type = zero_cc_parse_struct_body(c, inner_is_union);
                    if (inner_tag[0] != '\0') {
                        snprintf(inner_type.struct_name, sizeof(inner_type.struct_name), "%s", inner_tag);
                        zero_cc_register_struct(c, inner_tag, inner_type);
                    }
                }
            } else if (inner_tag[0] != '\0') {
                if (is_enum) {
                    ZeroCCType *found = zero_cc_find_enum(c, inner_tag);
                    if (found) inner_type = *found;
                } else {
                    ZeroCCType *found = zero_cc_find_struct(c, inner_tag);
                    if (found) inner_type = *found;
                }
            }

            if (c->tokens[c->token_pos].type == TK_SEMI) {
                /* Anonymous struct or union member */
                c->token_pos++;
                if (in_bitfield) {
                    current_offset = bitfield_storage_offset + bitfield_storage_size;
                    in_bitfield = false;
                }
                size_t anon_align = inner_type.align > 0 ? inner_type.align : 1;
                if (member_align > anon_align) anon_align = member_align;
                if (anon_align > st.align) st.align = anon_align;

                size_t anon_offset = 0;
                if (is_union) {
                    anon_offset = 0;
                    if (inner_type.size > max_size) max_size = inner_type.size;
                } else {
                    anon_offset = (current_offset + anon_align - 1) & ~(anon_align - 1);
                    current_offset = anon_offset + inner_type.size;
                }

                /* Flatten inner members into current struct/union */
                for (size_t i = 0; i < inner_type.member_count; i++) {
                    if (st.member_count < sizeof(st.members)/sizeof(st.members[0])) {
                        ZeroCCStructMember *m = &st.members[st.member_count++];
                        *m = inner_type.members[i];
                        m->offset += anon_offset;
                    }
                }
                continue;
            }

            /* Named struct/union member */
            bool is_ptr = false;
            while (c->tokens[c->token_pos].type == TK_STAR) {
                is_ptr = true;
                c->token_pos++;
            }
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
                    is_flexible = true;
                    elem_count = 0;
                }
                if (c->tokens[c->token_pos].type == TK_RBRACKET) c->token_pos++;
            }

            if (in_bitfield) {
                current_offset = bitfield_storage_offset + bitfield_storage_size;
                in_bitfield = false;
            }

            size_t base_align = is_ptr ? 8 : (inner_type.align > 0 ? inner_type.align : 8);
            size_t base_size = is_ptr ? 8 : inner_type.size;
            size_t effective_align = member_align > base_align ? member_align : base_align;
            /* Apply #pragma pack if set */
            if (c->default_pack > 0 && effective_align > (size_t)c->default_pack) {
                effective_align = (size_t)c->default_pack;
            }
            if (effective_align > st.align) st.align = effective_align;

            if (is_union) {
                if (mname[0] != '\0' && st.member_count < sizeof(st.members)/sizeof(st.members[0])) {
                    ZeroCCStructMember *m = &st.members[st.member_count++];
                    snprintf(m->name, sizeof(m->name), "%s", mname);
                    m->offset = 0;
                    m->size = base_size * elem_count;
                    m->align = effective_align;
                    m->is_flexible = false;
                    m->is_bitfield = false;
                    m->tag = is_ptr ? TYPE_PTR : (is_enum ? TYPE_ENUM : (inner_is_union ? TYPE_UNION : TYPE_STRUCT));
                    if (!is_ptr) {
                        m->base = zero_cc_type_alloc_base(&inner_type);
                    }
                    if (m->size > max_size) max_size = m->size;
                }
            } else {
                /* Align offset based on effective_align (already pack-capped) */
                current_offset = (current_offset + effective_align - 1) & ~(effective_align - 1);
                if (mname[0] != '\0' && st.member_count < sizeof(st.members)/sizeof(st.members[0])) {
                    ZeroCCStructMember *m = &st.members[st.member_count++];
                    snprintf(m->name, sizeof(m->name), "%s", mname);
                    m->offset = current_offset;
                    m->size = is_flexible ? 0 : (base_size * elem_count);
                    m->align = effective_align;
                    m->is_flexible = is_flexible;
                    m->is_bitfield = false;
                    m->tag = is_ptr ? TYPE_PTR : (is_enum ? TYPE_ENUM : (inner_is_union ? TYPE_UNION : TYPE_STRUCT));
                    if (!is_ptr) {
                        m->base = zero_cc_type_alloc_base(&inner_type);
                    }
                    if (!is_flexible) {
                        current_offset += m->size;
                    }
                }
            }
            if (c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
            continue;
        }

        bool member_is_const = false;
        bool member_is_volatile = false;
        bool member_is_atomic = false;
        while (c->tokens[c->token_pos].type == TK_CONST ||
               c->tokens[c->token_pos].type == TK_VOLATILE ||
               c->tokens[c->token_pos].type == TK_RESTRICT) {
            if (c->tokens[c->token_pos].type == TK_CONST) member_is_const = true;
            if (c->tokens[c->token_pos].type == TK_VOLATILE) member_is_volatile = true;
            c->token_pos++;
        }
        mt = c->tokens[c->token_pos].type;

        if (mt == TK_IDENT || mt == TK_INT || mt == TK_CHAR || mt == TK_VOID ||
            mt == TK_FLOAT_KW || mt == TK_DOUBLE_KW || mt == TK_BOOL_KW ||
            mt == TK_SIGNED || mt == TK_UNSIGNED || mt == TK_SHORT || mt == TK_LONG || mt == TK_ATOMIC) {
            bool is_unsigned = (mt == TK_UNSIGNED || mt == TK_BOOL_KW);
            char type_ident[128] = "";
            if (mt == TK_IDENT) {
                snprintf(type_ident, sizeof(type_ident), "%s", c->tokens[c->token_pos].str_val);
            }
            if (mt == TK_ATOMIC) {
                member_is_atomic = true;
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
                    else if (c->tokens[c->token_pos].type == TK_IDENT) {
                        snprintf(type_ident, sizeof(type_ident), "%s", c->tokens[c->token_pos].str_val);
                        mt = TK_IDENT; c->token_pos++;
                    }
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
                    else if (c->tokens[c->token_pos].type == TK_IDENT) {
                        snprintf(type_ident, sizeof(type_ident), "%s", c->tokens[c->token_pos].str_val);
                        mt = TK_IDENT; c->token_pos++;
                    }
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
            while (c->tokens[c->token_pos].type == TK_CONST ||
                   c->tokens[c->token_pos].type == TK_VOLATILE ||
                   c->tokens[c->token_pos].type == TK_RESTRICT ||
                   c->tokens[c->token_pos].type == TK_ATOMIC) {
                if (c->tokens[c->token_pos].type == TK_CONST) member_is_const = true;
                if (c->tokens[c->token_pos].type == TK_VOLATILE) member_is_volatile = true;
                if (c->tokens[c->token_pos].type == TK_ATOMIC) member_is_atomic = true;
                c->token_pos++;
            }
            bool is_ptr = false;
            bool member_ptr_is_const = false;
            bool member_ptr_is_volatile = false;
            while (c->tokens[c->token_pos].type == TK_STAR) {
                is_ptr = true;
                c->token_pos++;
                while (c->tokens[c->token_pos].type == TK_CONST ||
                       c->tokens[c->token_pos].type == TK_VOLATILE ||
                       c->tokens[c->token_pos].type == TK_RESTRICT) {
                    if (c->tokens[c->token_pos].type == TK_CONST) member_ptr_is_const = true;
                    if (c->tokens[c->token_pos].type == TK_VOLATILE) member_ptr_is_volatile = true;
                    c->token_pos++;
                }
            }
            size_t base_align = 8, base_size = 8;
            if (mt == TK_CHAR || mt == TK_BOOL_KW) { base_align = 1; base_size = 1; }
            else if (mt == TK_SHORT) { base_align = 2; base_size = 2; }
            else if (mt == TK_INT || mt == TK_FLOAT_KW) { base_align = 4; base_size = 4; }
            else if (mt == TK_LONG || mt == TK_DOUBLE_KW || mt == TK_VOID) { base_align = 8; base_size = 8; }
            else if (mt == TK_IDENT) {
                ZeroCCType *td = zero_cc_find_typedef(c, type_ident);
                if (!td) td = zero_cc_find_struct(c, type_ident);
                if (td) {
                    base_align = td->align > 0 ? td->align : 8;
                    base_size = td->size;
                }
            }
            if (is_ptr) { base_align = 8; base_size = 8; }
            size_t effective_align = member_align > base_align ? member_align : base_align;
            if (c->default_pack > 0 && effective_align > (size_t)c->default_pack) effective_align = (size_t)c->default_pack;
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
                if (st.member_count < sizeof(st.members)/sizeof(st.members[0]) && mname[0] != '\0') {
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
                    m->is_const = is_ptr ? member_ptr_is_const : member_is_const;
                    m->is_volatile = is_ptr ? member_ptr_is_volatile : member_is_volatile;
                    m->is_atomic = member_is_atomic;
                    if (mt == TK_CHAR || mt == TK_BOOL_KW) m->tag = TYPE_INT8;
                    else if (mt == TK_SHORT) m->tag = TYPE_INT16;
                    else if (mt == TK_FLOAT_KW) m->tag = TYPE_FLOAT;
                    else if (mt == TK_INT) m->tag = TYPE_INT32;
                    else if (mt == TK_DOUBLE_KW) m->tag = TYPE_DOUBLE;
                    else if (mt == TK_LONG || mt == TK_VOID) m->tag = TYPE_INT64;
                    else if (mt == TK_IDENT) {
                        ZeroCCType *td = zero_cc_find_typedef(c, type_ident);
                        if (!td) td = zero_cc_find_struct(c, type_ident);
                        if (td && td->tag == TYPE_UNION) m->tag = TYPE_UNION;
                        else m->tag = TYPE_STRUCT;
                        if (td && !is_ptr) {
                            m->base = zero_cc_type_alloc_base(td);
                        }
                    }
                    else m->tag = TYPE_INT32;
                    if (is_ptr) {
                        ZeroCCType dummy;
                        memset(&dummy, 0, sizeof(dummy));
                        dummy.tag = m->tag;
                        dummy.size = base_size;
                        dummy.align = base_align;
                        dummy.is_const = member_is_const;
                        dummy.is_volatile = member_is_volatile;
                        m->tag = TYPE_PTR;
                        m->base = zero_cc_type_alloc_base(&dummy);
                    } else if (!is_bitfield && (elem_count > 1 || is_flexible)) {
                        ZeroCCType elem_t;
                        if (m->base) {
                            elem_t = *m->base;
                        } else {
                            memset(&elem_t, 0, sizeof(elem_t));
                            elem_t.tag = m->tag;
                            elem_t.size = base_size;
                            elem_t.align = base_align;
                        }
                        elem_t.is_const = member_is_const;
                        elem_t.is_volatile = member_is_volatile;
                        m->tag = TYPE_ARRAY;
                        m->base = zero_cc_type_alloc_base(&elem_t);
                    }
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
                            if (mname[0] != '\0' && st.member_count < sizeof(st.members)/sizeof(st.members[0])) {
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
                                m->is_const = is_ptr ? member_ptr_is_const : member_is_const;
                                m->is_volatile = is_ptr ? member_ptr_is_volatile : member_is_volatile;
                                m->is_atomic = member_is_atomic;
                                if (mt == TK_CHAR || mt == TK_BOOL_KW) m->tag = TYPE_INT8;
                                else if (mt == TK_SHORT) m->tag = TYPE_INT16;
                                else if (mt == TK_FLOAT_KW) m->tag = TYPE_FLOAT;
                                else if (mt == TK_INT) m->tag = TYPE_INT32;
                                else if (mt == TK_DOUBLE_KW) m->tag = TYPE_DOUBLE;
                                else if (mt == TK_LONG || mt == TK_VOID) m->tag = TYPE_INT64;
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
                            if (mname[0] != '\0' && st.member_count < sizeof(st.members)/sizeof(st.members[0])) {
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
                                m->is_const = is_ptr ? member_ptr_is_const : member_is_const;
                                m->is_volatile = is_ptr ? member_ptr_is_volatile : member_is_volatile;
                                m->is_atomic = member_is_atomic;
                                if (mt == TK_CHAR || mt == TK_BOOL_KW) m->tag = TYPE_INT8;
                                else if (mt == TK_SHORT) m->tag = TYPE_INT16;
                                else if (mt == TK_FLOAT_KW) m->tag = TYPE_FLOAT;
                                else if (mt == TK_INT) m->tag = TYPE_INT32;
                                else if (mt == TK_DOUBLE_KW) m->tag = TYPE_DOUBLE;
                                else if (mt == TK_LONG || mt == TK_VOID) m->tag = TYPE_INT64;
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
                    if (mname[0] != '\0' && st.member_count < sizeof(st.members)/sizeof(st.members[0])) {
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
                        m->is_const = is_ptr ? member_ptr_is_const : member_is_const;
                        m->is_volatile = is_ptr ? member_ptr_is_volatile : member_is_volatile;
                        m->is_atomic = member_is_atomic;
                        if (mt == TK_CHAR || mt == TK_BOOL_KW) m->tag = TYPE_INT8;
                        else if (mt == TK_SHORT) m->tag = TYPE_INT16;
                        else if (mt == TK_FLOAT_KW) m->tag = TYPE_FLOAT;
                        else if (mt == TK_INT) m->tag = TYPE_INT32;
                        else if (mt == TK_DOUBLE_KW) m->tag = TYPE_DOUBLE;
                        else if (mt == TK_LONG || mt == TK_VOID) m->tag = TYPE_INT64;
                        else if (mt == TK_IDENT) {
                            ZeroCCType *td = zero_cc_find_typedef(c, type_ident);
                            if (!td) td = zero_cc_find_struct(c, type_ident);
                            if (td && td->tag == TYPE_UNION) m->tag = TYPE_UNION;
                            else m->tag = TYPE_STRUCT;
                            if (td && !is_ptr) {
                                m->base = zero_cc_type_alloc_base(td);
                            }
                        }
                        else m->tag = TYPE_INT32;
                        if (is_ptr) {
                            ZeroCCType dummy;
                            memset(&dummy, 0, sizeof(dummy));
                            dummy.tag = m->tag;
                            dummy.size = base_size;
                            dummy.align = base_align;
                            dummy.is_const = member_is_const;
                            dummy.is_volatile = member_is_volatile;
                            m->tag = TYPE_PTR;
                            m->base = zero_cc_type_alloc_base(&dummy);
                        } else if (elem_count > 1 || is_flexible) {
                            ZeroCCType elem_t;
                            if (m->base) {
                                elem_t = *m->base;
                            } else {
                                memset(&elem_t, 0, sizeof(elem_t));
                                elem_t.tag = m->tag;
                                elem_t.size = base_size;
                                elem_t.align = base_align;
                            }
                            elem_t.is_const = member_is_const;
                            elem_t.is_volatile = member_is_volatile;
                            m->tag = TYPE_ARRAY;
                            m->base = zero_cc_type_alloc_base(&elem_t);
                        }
                        if (!is_flexible) {
                            current_offset += m->size;
                        }
                    }
                }
            }
            while (c->tokens[c->token_pos].type == TK_COMMA) {
                c->token_pos++;
                bool c_is_ptr = false;
                bool c_member_ptr_is_const = false;
                bool c_member_ptr_is_volatile = false;
                while (c->tokens[c->token_pos].type == TK_STAR) {
                    c_is_ptr = true;
                    c->token_pos++;
                    while (c->tokens[c->token_pos].type == TK_CONST ||
                           c->tokens[c->token_pos].type == TK_VOLATILE ||
                           c->tokens[c->token_pos].type == TK_RESTRICT) {
                        if (c->tokens[c->token_pos].type == TK_CONST) c_member_ptr_is_const = true;
                        if (c->tokens[c->token_pos].type == TK_VOLATILE) c_member_ptr_is_volatile = true;
                        c->token_pos++;
                    }
                }
                size_t c_base_align = c_is_ptr ? 8 : base_align;
                size_t c_base_size = c_is_ptr ? 8 : base_size;
                size_t c_effective_align = member_align > c_base_align ? member_align : c_base_align;
                if (c->default_pack > 0 && c_effective_align > (size_t)c->default_pack) c_effective_align = (size_t)c->default_pack;
                if (c_effective_align > st.align) st.align = c_effective_align;

                char c_mname[128] = "";
                if (c->tokens[c->token_pos].type == TK_IDENT) {
                    snprintf(c_mname, sizeof(c_mname), "%s", c->tokens[c->token_pos].str_val);
                    c->token_pos++;
                }

                size_t c_elem_count = 1;
                bool c_is_flexible = false;
                if (c->tokens[c->token_pos].type == TK_LBRACKET) {
                    c->token_pos++;
                    if (c->tokens[c->token_pos].type == TK_NUM) {
                        c_elem_count = (size_t)c->tokens[c->token_pos].num_val;
                        c->token_pos++;
                    } else if (c->tokens[c->token_pos].type == TK_RBRACKET) {
                        c_is_flexible = true;
                        c_elem_count = 0;
                    }
                    if (c->tokens[c->token_pos].type == TK_RBRACKET) c->token_pos++;
                }

                bool c_is_bitfield = false;
                uint8_t c_bit_width = 0;
                if (c->tokens[c->token_pos].type == TK_COLON) {
                    c_is_bitfield = true;
                    c->token_pos++;
                    if (c->tokens[c->token_pos].type == TK_NUM) {
                        c_bit_width = (uint8_t)c->tokens[c->token_pos].num_val;
                        c->token_pos++;
                    }
                }

                if (is_union) {
                    if (st.member_count < sizeof(st.members)/sizeof(st.members[0]) && c_mname[0] != '\0') {
                        ZeroCCStructMember *m = &st.members[st.member_count++];
                        snprintf(m->name, sizeof(m->name), "%s", c_mname);
                        m->offset = 0;
                        m->size = c_is_bitfield ? c_base_size : (c_base_size * c_elem_count);
                        m->align = c_effective_align;
                        m->is_flexible = false;
                        m->is_bitfield = c_is_bitfield;
                        m->bit_offset = 0;
                        m->bit_width = c_is_bitfield ? c_bit_width : (uint8_t)(c_base_size * 8);
                        m->is_signed = !is_unsigned;
                        m->is_const = c_is_ptr ? c_member_ptr_is_const : member_is_const;
                        m->is_volatile = c_is_ptr ? c_member_ptr_is_volatile : member_is_volatile;
                        m->is_atomic = member_is_atomic;
                        if (mt == TK_CHAR || mt == TK_BOOL_KW) m->tag = TYPE_INT8;
                        else if (mt == TK_SHORT) m->tag = TYPE_INT16;
                        else if (mt == TK_FLOAT_KW) m->tag = TYPE_FLOAT;
                        else if (mt == TK_INT) m->tag = TYPE_INT32;
                        else if (mt == TK_DOUBLE_KW) m->tag = TYPE_DOUBLE;
                        else if (mt == TK_LONG || mt == TK_VOID) m->tag = TYPE_INT64;
                        else if (mt == TK_IDENT) {
                            ZeroCCType *td = zero_cc_find_typedef(c, type_ident);
                            if (!td) td = zero_cc_find_struct(c, type_ident);
                            if (td && td->tag == TYPE_UNION) m->tag = TYPE_UNION;
                            else m->tag = TYPE_STRUCT;
                            if (td && !c_is_ptr) {
                                m->base = zero_cc_type_alloc_base(td);
                            }
                        }
                        else m->tag = TYPE_INT32;
                        if (c_is_ptr) {
                            ZeroCCType dummy;
                            memset(&dummy, 0, sizeof(dummy));
                            dummy.tag = m->tag;
                            dummy.size = base_size;
                            dummy.align = base_align;
                            dummy.is_const = member_is_const;
                            dummy.is_volatile = member_is_volatile;
                            m->tag = TYPE_PTR;
                            m->base = zero_cc_type_alloc_base(&dummy);
                        } else if (!c_is_bitfield && (c_elem_count > 1 || c_is_flexible)) {
                            ZeroCCType elem_t;
                            if (m->base) {
                                elem_t = *m->base;
                            } else {
                                memset(&elem_t, 0, sizeof(elem_t));
                                elem_t.tag = m->tag;
                                elem_t.size = base_size;
                                elem_t.align = base_align;
                            }
                            elem_t.is_const = member_is_const;
                            elem_t.is_volatile = member_is_volatile;
                            m->tag = TYPE_ARRAY;
                            m->base = zero_cc_type_alloc_base(&elem_t);
                        }
                        if (m->size > max_size) max_size = m->size;
                    }
                } else {
                    if (in_bitfield) {
                        current_offset = bitfield_storage_offset + bitfield_storage_size;
                        in_bitfield = false;
                    }
                    current_offset = (current_offset + c_effective_align - 1) & ~(c_effective_align - 1);
                    if (c_mname[0] != '\0' && st.member_count < sizeof(st.members)/sizeof(st.members[0])) {
                        ZeroCCStructMember *m = &st.members[st.member_count++];
                        snprintf(m->name, sizeof(m->name), "%s", c_mname);
                        m->offset = current_offset;
                        m->size = c_is_flexible ? 0 : (c_base_size * c_elem_count);
                        m->align = c_effective_align;
                        m->is_flexible = c_is_flexible;
                        m->is_bitfield = false;
                        m->bit_offset = 0;
                        m->bit_width = 0;
                        m->is_signed = !is_unsigned;
                        m->is_const = c_is_ptr ? c_member_ptr_is_const : member_is_const;
                        m->is_volatile = c_is_ptr ? c_member_ptr_is_volatile : member_is_volatile;
                        m->is_atomic = member_is_atomic;
                        if (mt == TK_CHAR || mt == TK_BOOL_KW) m->tag = TYPE_INT8;
                        else if (mt == TK_SHORT) m->tag = TYPE_INT16;
                        else if (mt == TK_FLOAT_KW) m->tag = TYPE_FLOAT;
                        else if (mt == TK_INT) m->tag = TYPE_INT32;
                        else if (mt == TK_DOUBLE_KW) m->tag = TYPE_DOUBLE;
                        else if (mt == TK_LONG || mt == TK_VOID) m->tag = TYPE_INT64;
                        else if (mt == TK_IDENT) {
                            ZeroCCType *td = zero_cc_find_typedef(c, type_ident);
                            if (!td) td = zero_cc_find_struct(c, type_ident);
                            if (td && td->tag == TYPE_UNION) m->tag = TYPE_UNION;
                            else m->tag = TYPE_STRUCT;
                            if (td && !c_is_ptr) {
                                m->base = zero_cc_type_alloc_base(td);
                            }
                        }
                        else m->tag = TYPE_INT32;
                        if (c_is_ptr) {
                            ZeroCCType dummy;
                            memset(&dummy, 0, sizeof(dummy));
                            dummy.tag = m->tag;
                            dummy.size = base_size;
                            dummy.align = base_align;
                            dummy.is_const = member_is_const;
                            dummy.is_volatile = member_is_volatile;
                            m->tag = TYPE_PTR;
                            m->base = zero_cc_type_alloc_base(&dummy);
                        } else if (c_elem_count > 1 || c_is_flexible) {
                            ZeroCCType elem_t;
                            if (m->base) {
                                elem_t = *m->base;
                            } else {
                                memset(&elem_t, 0, sizeof(elem_t));
                                elem_t.tag = m->tag;
                                elem_t.size = base_size;
                                elem_t.align = base_align;
                            }
                            elem_t.is_const = member_is_const;
                            elem_t.is_volatile = member_is_volatile;
                            m->tag = TYPE_ARRAY;
                            m->base = zero_cc_type_alloc_base(&elem_t);
                        }
                        if (!c_is_flexible) {
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
    /* Apply #pragma pack to final struct alignment */
    if (c->default_pack > 0 && st.align > (size_t)c->default_pack) {
        st.align = (size_t)c->default_pack;
        st.size = (current_offset + st.align - 1) & ~(st.align - 1);
    }
    if (st.align == 0) st.align = 1;
    if (c->tokens[c->token_pos].type == TK_RBRACE) c->token_pos++;
    return st;
}

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
            ZeroCCType st = zero_cc_parse_struct_body(c, is_union);
            if (st_name[0] != '\0') {
                snprintf(st.struct_name, sizeof(st.struct_name), "%s", st_name);
                zero_cc_register_struct(c, st_name, st);
            }
            if (c->tokens[c->token_pos].type == TK_IDENT) {
                char alias[128];
                snprintf(alias, sizeof(alias), "%s", c->tokens[c->token_pos].str_val);
                c->token_pos++;
                if (st.struct_name[0] == '\0') {
                    snprintf(st.struct_name, sizeof(st.struct_name), "%s", alias);
                }
                zero_cc_register_struct(c, alias, st);
                if (is_typedef) {
                    zero_cc_register_typedef(c, alias, st);
                }
            }
            if (c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
            return;
        } else if (st_name[0] != '\0') {
            /* Opaque / forward struct typedef or declaration: e.g. typedef struct FILE FILE; */
            ZeroCCType *existing = zero_cc_find_struct(c, st_name);
            ZeroCCType st;
            if (existing) {
                st = *existing;
            } else {
                memset(&st, 0, sizeof(st));
                st.tag = is_union ? TYPE_UNION : TYPE_STRUCT;
                st.size = 8;
                st.align = 8;
                snprintf(st.struct_name, sizeof(st.struct_name), "%s", st_name);
                zero_cc_register_struct(c, st_name, st);
            }
            int ptr_depth = 0;
            while (c->tokens[c->token_pos].type == TK_STAR) {
                ptr_depth++;
                c->token_pos++;
            }
            for (int pd = 0; pd < ptr_depth; pd++) {
                st = zero_cc_type_ptr(&st);
            }
            if (is_typedef) {
                char alias[128] = "";
                if (c->tokens[c->token_pos].type == TK_IDENT) {
                    snprintf(alias, sizeof(alias), "%s", c->tokens[c->token_pos].str_val);
                    c->token_pos++;
                } else if (ptr_depth == 0) {
                    /* Handle case where st_name itself was the alias */
                    snprintf(alias, sizeof(alias), "%s", st_name);
                }
                if (alias[0] != '\0') {
                    if (st.tag == TYPE_STRUCT || st.tag == TYPE_UNION) {
                        zero_cc_register_struct(c, alias, st);
                    }
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
            et.tag = TYPE_ENUM;
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
                    if (enum_val == INT64_MAX) {
                        diag_report_error_at_token(c, "overflow in enumeration values");
                    } else {
                        enum_val++;
                    }
                }
                if (c->tokens[c->token_pos].type == TK_COMMA) c->token_pos++;
            }
            if (c->tokens[c->token_pos].type == TK_RBRACE) c->token_pos++;
            if (en_name[0] != '\0') {
                zero_cc_register_enum(c, en_name, et);
            }
            if (c->tokens[c->token_pos].type == TK_IDENT) {
                char alias[128];
                snprintf(alias, sizeof(alias), "%s", c->tokens[c->token_pos].str_val);
                c->token_pos++;
                zero_cc_register_typedef(c, alias, et);
            }
            if (c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
            return;
        } else if (en_name[0] != '\0') {
            ZeroCCType *existing = zero_cc_find_enum(c, en_name);
            ZeroCCType et = existing ? *existing : zero_cc_type_enum(en_name);
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
        et.tag = TYPE_ENUM;
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
                if (enum_val == INT64_MAX) {
                    diag_report_error_at_token(c, "overflow in enumeration values");
                } else {
                    enum_val++;
                }
            }
            if (c->tokens[c->token_pos].type == TK_COMMA) c->token_pos++;
        }
        if (c->tokens[c->token_pos].type == TK_RBRACE) c->token_pos++;
        if (en_name[0] != '\0') {
            zero_cc_register_enum(c, en_name, et);
        }
        if (c->tokens[c->token_pos].type == TK_IDENT) {
            char alias[128];
            snprintf(alias, sizeof(alias), "%s", c->tokens[c->token_pos].str_val);
            c->token_pos++;
            zero_cc_register_typedef(c, alias, et);
        }
        if (c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
        return;
    } else if (en_name[0] != '\0') {
        if (!zero_cc_find_enum(c, en_name)) {
            ZeroCCType et = zero_cc_type_enum(en_name);
            zero_cc_register_enum(c, en_name, et);
        }
    }
    if (c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
}

/* Scalar Typedef */
static inline void zero_cc_stmt_typedef_scalar(ZeroCCCompiler *c) {
    c->token_pos++; /* consume TK_TYPEDEF */
    ZeroCCTokType t = c->tokens[c->token_pos].type;
    ZeroCCType ttype; memset(&ttype, 0, sizeof(ttype));
    bool is_unsigned = false;
    bool is_signed = false;
    if (t == TK_UNSIGNED) { is_unsigned = true; c->token_pos++; t = c->tokens[c->token_pos].type; }
    else if (t == TK_SIGNED) { is_signed = true; c->token_pos++; t = c->tokens[c->token_pos].type; }

    if (t == TK_INT) { ttype = is_unsigned ? zero_cc_type_uint32() : zero_cc_type_int(); c->token_pos++; }
    else if (t == TK_CHAR) { ttype = is_unsigned ? zero_cc_type_uint8() : zero_cc_type_char(); c->token_pos++; }
    else if (t == TK_VOID) { ttype = zero_cc_type_void(); c->token_pos++; }
    else if (t == TK_FLOAT_KW) { ttype = zero_cc_type_float(); c->token_pos++; }
    else if (t == TK_DOUBLE_KW) { ttype = zero_cc_type_double(); c->token_pos++; }
    else if (t == TK_BOOL_KW) { ttype = zero_cc_type_bool(); c->token_pos++; }
    else if (t == TK_SHORT) { ttype = is_unsigned ? zero_cc_type_uint16() : zero_cc_type_int16(); c->token_pos++; }
    else if (t == TK_LONG) {
        c->token_pos++;
        if (c->tokens[c->token_pos].type == TK_LONG) c->token_pos++;
        ttype = is_unsigned ? zero_cc_type_uint64() : zero_cc_type_int64();
    }
    else if (t == TK_IDENT) {
        const char *tname = c->tokens[c->token_pos].str_val;
        c->token_pos++;
        ZeroCCType *found = zero_cc_find_typedef(c, tname);
        if (found) { ttype = *found; }
        else { ttype = is_unsigned ? zero_cc_type_uint32() : zero_cc_type_int(); }
    } else {
        ttype = is_unsigned ? zero_cc_type_uint32() : zero_cc_type_int();
    }
    (void)is_signed;
    bool is_ptr = false;
    while (c->tokens[c->token_pos].type == TK_STAR) { is_ptr = true; c->token_pos++; }
    if (is_ptr) { ttype = zero_cc_type_ptr(&ttype); }

    /* Function pointer typedef: typedef RetType (*Alias)(params...); */
    if (c->tokens[c->token_pos].type == TK_LPAREN) {
        c->token_pos++; /* consume '(' */
        while (c->tokens[c->token_pos].type == TK_STAR) c->token_pos++;
        char alias[128] = "";
        if (c->tokens[c->token_pos].type == TK_IDENT) {
            snprintf(alias, sizeof(alias), "%s", c->tokens[c->token_pos].str_val);
            c->token_pos++;
        }
        if (c->tokens[c->token_pos].type == TK_RPAREN) c->token_pos++;
        if (c->tokens[c->token_pos].type == TK_LPAREN) {
            int pdepth = 1;
            c->token_pos++;
            while (c->token_pos < c->token_count && pdepth > 0) {
                if (c->tokens[c->token_pos].type == TK_LPAREN) pdepth++;
                else if (c->tokens[c->token_pos].type == TK_RPAREN) pdepth--;
                c->token_pos++;
            }
        }
        ZeroCCType fp_type = zero_cc_type_ptr(&ttype);
        fp_type.is_func_ptr = true;
        if (alias[0] != '\0') {
            zero_cc_register_typedef(c, alias, fp_type);
        }
        if (c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
        return;
    }

    if (c->tokens[c->token_pos].type == TK_IDENT) {
        char alias[128];
        snprintf(alias, sizeof(alias), "%s", c->tokens[c->token_pos].str_val);
        c->token_pos++;
        zero_cc_register_typedef(c, alias, ttype);
    }
    if (c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
}

#endif /* ZERO_CC_STATEMENTS_DEFINITIONS_H */
