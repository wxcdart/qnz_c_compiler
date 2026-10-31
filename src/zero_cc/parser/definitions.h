/* Type Definitions: typedef struct, typedef enum, standalone enum, scalar typedef */
#ifndef ZERO_CC_PARSER_DEFINITIONS_H
#define ZERO_CC_PARSER_DEFINITIONS_H

#include "zero_cc/types/types.h"
#include "zero_cc/parser/const_eval.h"

/* Forward declaration */
static inline ZeroCCType zero_cc_parse_struct_body(ZeroCCCompiler *c, bool is_union);

/* Shared aggregate-initializer engine (init_common.h, included after this
 * header in the TUs that need it): emits a brace-list into a caller buffer
 * and returns with the stream positioned after the closing brace. */
static inline void zero_cc_emit_brace_init_global(ZeroCCCompiler *c, ZeroCCType obj_type,
                                                  uint8_t *buf, size_t buf_off, size_t buf_cap,
                                                  size_t data_patch_off, size_t *cursor,
                                                  size_t fam_bytes);

/* M3: the struct/union member table is heap-backed (it was a fixed 64-entry
 * array and the 65th+ member was silently dropped). Reserve one slot for the
 * next member; on OOM report a clean fatal diagnostic (had_error => no binary). */
static inline bool struct_member_room(ZeroCCCompiler *c, ZeroCCType *st) {
    if (zero_cc_type_reserve_members(st, st->member_count + 1)) return true;
    diag_report_error_at_token(c, "out of memory adding struct member");
    return false;
}

/* C99 6.7.2.1: build the element type of a multi-dimensional array member
 * from the scalar/inner element and the per-dimension counts (dims[0] is
 * the OUTERMOST, written first in source).  `char m[2][2]` must have element
 * type `char[2]`, not a flattened `char` - flattening makes `m[1]` a single
 * byte instead of a 2-byte row (c-torture pr60017).  One-dimensional
 * members (ndims <= 1) are returned unchanged. */
static inline ZeroCCType zero_cc_member_nest_array_elem(ZeroCCType elem, const size_t *dims, int ndims) {
    for (int di = ndims - 1; di >= 1; di--) {
        if (dims[di] == 0) continue;
        ZeroCCType arr;
        memset(&arr, 0, sizeof(arr));
        arr.tag = TYPE_ARRAY;
        arr.base = zero_cc_type_alloc_base(&elem);
        arr.size = elem.size * dims[di];
        arr.align = elem.align;
        arr.array_len = dims[di];
        arr.is_const = elem.is_const;
        arr.is_volatile = elem.is_volatile;
        arr.is_restrict = elem.is_restrict;
        arr.is_atomic = elem.is_atomic;
        elem = arr;
    }
    return elem;
}

/* Parse one enum body `c->token_pos` at the first member (LBRACE consumed);
 * consumes through RBRACE. M1: two fixes vs the old per-site loops —
 *   (1) `false`/`true` are accepted as member names: in C they are plain
 *       identifiers unless <stdbool.h> is included (TK_FALSE/TK_TRUE tokens
 *       carried str_val; the old TK_IDENT-only check stalled the loop on
 *       `enum { false, true }` — compile/pr94238.c); and
 *   (2) a progress guard: any token that neither member nor comma parsing
 *       consumes is diagnosed and skipped, so a malformed body
 *       (e.g. `enum { 1, 2 }`) can never spin the parser forever. */
static inline void zero_cc_parse_enum_members(ZeroCCCompiler *c) {
    int64_t enum_val = 0;
    while (c->token_pos < c->token_count &&
           c->tokens[c->token_pos].type != TK_RBRACE &&
           c->tokens[c->token_pos].type != TK_EOF) {
        size_t saved = c->token_pos;
        if (c->tokens[c->token_pos].type == TK_IDENT ||
            c->tokens[c->token_pos].type == TK_FALSE ||
            c->tokens[c->token_pos].type == TK_TRUE) {
            char ename[128];
            snprintf(ename, sizeof(ename), "%s", c->tokens[c->token_pos].str_val);
            c->token_pos++;
            if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_ASSIGN) {
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
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_COMMA) c->token_pos++;
        if (c->token_pos == saved) {
            diag_report_error_at_token(c, "expected identifier in enum definition");
            c->token_pos++;
        }
    }
    if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RBRACE) c->token_pos++;
}

/* Parse struct or union body { ... } */
/* A pointer member `T **p` records only one TYPE_PTR level (tag=PTR, base=T).
 * Wrap the pointee `extra` more times so the member's type is T with the
 * full star count, matching how locals/params are typed. */
static inline void zero_cc_member_wrap_ptr_levels(ZeroCCStructMember *m, int extra) {
    for (int i = 0; i < extra && m->base; i++) {
        ZeroCCType pt = zero_cc_type_ptr(m->base);
        m->base = zero_cc_type_alloc_base(&pt);
    }
}

/* `T *a[N]` is an array of N pointers.  The member parser builds the pointer
 * (tag=PTR, base=pointee); turn it into TYPE_ARRAY-of-pointer when the
 * declarator has an array suffix (previously the array dimension was dropped
 * for pointer members, so `a[1]` indexed past a single pointer). */
static inline void zero_cc_member_make_ptr_array(ZeroCCStructMember *m) {
    ZeroCCType pt;
    memset(&pt, 0, sizeof(pt));
    pt.tag = TYPE_PTR;
    pt.size = 8;
    pt.align = 8;
    pt.base = m->base;
    m->tag = TYPE_ARRAY;
    m->base = zero_cc_type_alloc_base(&pt);
}

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
        size_t iter_start = c->token_pos;
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
        /* GNU vector member: `__attribute__((__vector_size__(N))) float mVec;`
         * - probe the preprocessor marker before the element type. */
        size_t member_vec_size = 0;
        {
            size_t save = c->token_pos;
            size_t probe = 0;
            if (zero_cc_parse_vector_attr(c, &c->token_pos, &probe)) {
                ZeroCCTokType nt = c->token_pos < c->token_count ? c->tokens[c->token_pos].type : TK_EOF;
                if (nt == TK_INT || nt == TK_CHAR || nt == TK_SHORT || nt == TK_LONG ||
                    nt == TK_SIGNED || nt == TK_UNSIGNED || nt == TK_FLOAT_KW ||
                    nt == TK_DOUBLE_KW || nt == TK_BOOL_KW) {
                    member_vec_size = probe;
                } else {
                    c->token_pos = save;
                }
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
                    zero_cc_parse_enum_members(c);
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
                    if (struct_member_room(c, &st)) {
                        ZeroCCStructMember *m = &st.members[st.member_count++];
                        *m = inner_type.members[i];
                        m->offset += anon_offset;
                    }
                }
                /* The flattened anonymous struct's own members array is
                 * now orphaned (entries were copied above) — release it. */
                free(inner_type.members);
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
            size_t dims[8] = {0};
            int ndims = 0;
            bool is_flexible = false;
            bool is_array = false;
            /* Multi-dimensional arrays: T a[d1][d2]... (C99 6.7.2.1).
             * Dimensions are constant expressions (6.7.2.1p3). */
            while (c->tokens[c->token_pos].type == TK_LBRACKET) {
                is_array = true;
                c->token_pos++;
                size_t dim = 1;
                if (c->tokens[c->token_pos].type == TK_RBRACKET) {
                    is_flexible = true;
                    dim = 0;
                } else {
                    int64_t cd = zero_cc_eval_const_expr(c);
                    if (cd > 0) dim = (size_t)cd;
                }
                if (c->tokens[c->token_pos].type == TK_RBRACKET) c->token_pos++;
                if (is_flexible) break;
                elem_count *= dim;
                if (ndims < 8) dims[ndims++] = dim;
            }

            bool is_bitfield = false;
            uint8_t bit_width = 0;
            if (c->tokens[c->token_pos].type == TK_COLON) {
                is_bitfield = true;
                c->token_pos++;
                int64_t bw = zero_cc_eval_const_expr(c);
                bit_width = (bw > 0) ? (uint8_t)bw : 0;
            }
            /* GNU aligned(N) trailing the member declarator:
             * `short s __attribute__((aligned(2), packed));` */
            {
                size_t probe = 0;
                while (zero_cc_parse_aligned_attr(c, &c->token_pos, &probe)) {
                    if (probe > member_align) member_align = probe;
                }
            }

            if (in_bitfield && !is_bitfield) {
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
                if (mname[0] != '\0' && struct_member_room(c, &st)) {
                    ZeroCCStructMember *m = &st.members[st.member_count++];
                    snprintf(m->name, sizeof(m->name), "%s", mname);
                    m->offset = 0;
                    m->size = is_bitfield ? base_size : (base_size * elem_count);
                    m->align = effective_align;
                    m->is_flexible = false;
                    m->is_bitfield = is_bitfield;
                    m->bit_offset = 0;
                    m->bit_width = is_bitfield ? bit_width : (uint8_t)(base_size * 8);
                    m->is_signed = true;
                    m->tag = is_ptr ? TYPE_PTR : (is_enum ? TYPE_ENUM : (inner_is_union ? TYPE_UNION : TYPE_STRUCT));
                    /* Pointer members also record the pointee (C5): `t *p`
                     * members need m->base so `o.p->field` resolves. */
                    m->base = zero_cc_type_alloc_base(&inner_type);
                    if (!is_ptr && !is_bitfield && (is_array || is_flexible)) {
                        m->tag = TYPE_ARRAY; /* array-of-struct member in union */
                        if (ndims > 1) {
                            ZeroCCType et = inner_type;
                            et = zero_cc_member_nest_array_elem(et, dims, ndims);
                            m->base = zero_cc_type_alloc_base(&et);
                        }
                    }
                    if (m->size > max_size) max_size = m->size;
                }
            } else if (is_bitfield) {
                if (bit_width == 0) {
                    if (in_bitfield) {
                        current_offset = bitfield_storage_offset + bitfield_storage_size;
                        in_bitfield = false;
                    }
                    current_offset = (current_offset + effective_align - 1) & ~(effective_align - 1);
                } else if (in_bitfield && bitfield_storage_size == base_size &&
                           (bitfield_bit_offset + bit_width <= base_size * 8)) {
                    size_t m_offset = bitfield_storage_offset;
                    uint8_t m_bit_off = (uint8_t)bitfield_bit_offset;
                    bitfield_bit_offset += bit_width;
                    if (mname[0] != '\0' && struct_member_room(c, &st)) {
                        ZeroCCStructMember *m = &st.members[st.member_count++];
                        snprintf(m->name, sizeof(m->name), "%s", mname);
                        m->offset = m_offset;
                        m->size = base_size;
                        m->align = effective_align;
                        m->is_flexible = false;
                        m->is_bitfield = true;
                        m->bit_offset = m_bit_off;
                        m->bit_width = bit_width;
                        m->is_signed = true;
                        m->tag = TYPE_INT32;
                    }
                } else {
                    if (in_bitfield) {
                        current_offset = bitfield_storage_offset + bitfield_storage_size;
                    }
                    current_offset = (current_offset + effective_align - 1) & ~(effective_align - 1);
                    bitfield_storage_offset = current_offset;
                    bitfield_storage_size = base_size;
                    bitfield_bit_offset = bit_width;
                    in_bitfield = true;
                    if (mname[0] != '\0' && struct_member_room(c, &st)) {
                        ZeroCCStructMember *m = &st.members[st.member_count++];
                        snprintf(m->name, sizeof(m->name), "%s", mname);
                        m->offset = bitfield_storage_offset;
                        m->size = base_size;
                        m->align = effective_align;
                        m->is_flexible = false;
                        m->is_bitfield = true;
                        m->bit_offset = 0;
                        m->bit_width = bit_width;
                        m->is_signed = true;
                        m->tag = TYPE_INT32;
                    }
                }
            } else {
                /* Align offset based on effective_align (already pack-capped) */
                current_offset = (current_offset + effective_align - 1) & ~(effective_align - 1);
                if (mname[0] != '\0' && struct_member_room(c, &st)) {
                    ZeroCCStructMember *m = &st.members[st.member_count++];
                    snprintf(m->name, sizeof(m->name), "%s", mname);
                    m->offset = current_offset;
                    m->size = is_flexible ? 0 : (base_size * elem_count);
                    m->align = effective_align;
                    m->is_flexible = is_flexible;
                    m->is_bitfield = false;
                    m->tag = is_ptr ? TYPE_PTR : (is_enum ? TYPE_ENUM : (inner_is_union ? TYPE_UNION : TYPE_STRUCT));
                    /* Pointer members also record the pointee (C5). */
                    m->base = zero_cc_type_alloc_base(&inner_type);
                    if (!is_ptr && (is_array || is_flexible)) {
                        /* Array of struct/union/enum member (`struct S arr[N]`):
                         * store as TYPE_ARRAY with the element type as base so
                         * `o.arr[i]` resolves. m->base already points to the
                         * element (inner_type); only the tag changes. */
                        m->tag = TYPE_ARRAY;
                        if (ndims > 1) {
                            ZeroCCType et = inner_type;
                            et = zero_cc_member_nest_array_elem(et, dims, ndims);
                            m->base = zero_cc_type_alloc_base(&et);
                        }
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
            mt == TK_SIGNED || mt == TK_UNSIGNED || mt == TK_SHORT || mt == TK_LONG || mt == TK_ATOMIC ||
            mt == TK_TYPEOF || mt == TK_TYPEOF_UNQUAL) {
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
            ZeroCCType typeof_type;
            memset(&typeof_type, 0, sizeof(typeof_type));
            bool is_typeof = false;
            if (mt == TK_TYPEOF || mt == TK_TYPEOF_UNQUAL) {
                is_typeof = true;
                bool unqual = (mt == TK_TYPEOF_UNQUAL);
                if (c->tokens[c->token_pos].type == TK_LPAREN) {
                    c->token_pos++;
                    if (zero_cc_tok_is_type_start(c, c->token_pos)) {
                        typeof_type = zero_cc_parse_type_name_at_pos(c, &c->token_pos);
                    } else {
                        typeof_type = zero_cc_type_of_equality(c, &c->token_pos);
                    }
                    if (c->tokens[c->token_pos].type == TK_RPAREN) {
                        c->token_pos++;
                    }
                }
                if (unqual) {
                    typeof_type.is_const = false;
                    typeof_type.is_volatile = false;
                    typeof_type.is_restrict = false;
                    typeof_type.is_atomic = false;
                }
            }
            if (mt == TK_UNSIGNED || mt == TK_SIGNED) {
                is_unsigned = (mt == TK_UNSIGNED);
                if (c->tokens[c->token_pos].type == TK_INT) { mt = TK_INT; c->token_pos++; }
                else if (c->tokens[c->token_pos].type == TK_CHAR) { mt = TK_CHAR; c->token_pos++; }
                else if (c->tokens[c->token_pos].type == TK_SHORT) {
                    mt = TK_SHORT;
                    c->token_pos++;
                    if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_INT) c->token_pos++;
                }
                else if (c->tokens[c->token_pos].type == TK_LONG) {
                    mt = TK_LONG;
                    c->token_pos++;
                    if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LONG) c->token_pos++;
                    if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_INT) c->token_pos++;
                }
                else { mt = TK_INT; }
            } else if (mt == TK_LONG) {
                if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LONG) c->token_pos++;
                if (c->token_pos < c->token_count && (c->tokens[c->token_pos].type == TK_UNSIGNED || c->tokens[c->token_pos].type == TK_SIGNED)) {
                    is_unsigned = (c->tokens[c->token_pos].type == TK_UNSIGNED);
                    c->token_pos++;
                }
                if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_INT) c->token_pos++;
            } else if (mt == TK_SHORT) {
                if (c->token_pos < c->token_count && (c->tokens[c->token_pos].type == TK_UNSIGNED || c->tokens[c->token_pos].type == TK_SIGNED)) {
                    is_unsigned = (c->tokens[c->token_pos].type == TK_UNSIGNED);
                    c->token_pos++;
                }
                if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_INT) c->token_pos++;
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
            int ptr_depth = 0;
            bool member_ptr_is_const = false;
            bool member_ptr_is_volatile = false;
            while (c->tokens[c->token_pos].type == TK_STAR) {
                is_ptr = true;
                ptr_depth++;
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
            if (is_typeof) {
                base_align = typeof_type.align > 0 ? typeof_type.align : 8;
                base_size = typeof_type.size;
            } else if (mt == TK_CHAR || mt == TK_BOOL_KW) { base_align = 1; base_size = 1; }
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
            /* Function-pointer member: `type (*name)(params);`
             * (previously the `(` after the `*` star was never handled and
             * the member loop spun forever on it).  The declarator is
             * consumed here; the member is a plain 8-byte pointer. */
            if (c->tokens[c->token_pos].type == TK_LPAREN &&
                c->token_pos + 1 < c->token_count &&
                c->tokens[c->token_pos + 1].type == TK_STAR) {
                c->token_pos++; /* ( */
                c->token_pos++; /* * */
                if (c->tokens[c->token_pos].type == TK_IDENT) {
                    snprintf(mname, sizeof(mname), "%s", c->tokens[c->token_pos].str_val);
                    c->token_pos++;
                }
                if (c->tokens[c->token_pos].type == TK_RPAREN) c->token_pos++;
                /* Skip the parameter list `( ... )` */
                if (c->tokens[c->token_pos].type == TK_LPAREN) {
                    c->token_pos++;
                    int paren_depth = 1;
                    while (c->token_pos < c->token_count && paren_depth > 0) {
                        if (c->tokens[c->token_pos].type == TK_LPAREN) paren_depth++;
                        else if (c->tokens[c->token_pos].type == TK_RPAREN) paren_depth--;
                        c->token_pos++;
                    }
                }
                is_ptr = true;
                base_align = 8;
                base_size = 8;
                effective_align = member_align > base_align ? member_align : base_align;
                if (c->default_pack > 0 && effective_align > (size_t)c->default_pack) effective_align = (size_t)c->default_pack;
                if (effective_align > st.align) st.align = effective_align;
            } else if (c->tokens[c->token_pos].type == TK_IDENT) {
                snprintf(mname, sizeof(mname), "%s", c->tokens[c->token_pos].str_val);
                c->token_pos++;
            }

            size_t elem_count = 1;
            size_t dims[8] = {0};
            int ndims = 0;
            bool is_flexible = false;
            bool is_array = false;
            /* Multi-dimensional arrays: T a[d1][d2]... (C99 6.7.2.1).
             * Dimensions are constant expressions (6.7.2.1p3):
             * `char buf[72*1024*1024];` is evaluated, not a bare NUM. */
            while (c->tokens[c->token_pos].type == TK_LBRACKET) {
                is_array = true;
                c->token_pos++;
                size_t dim = 1;
                if (c->tokens[c->token_pos].type == TK_RBRACKET) {
                    /* Flexible array member: empty brackets [] (C99 6.7.2.1p18) */
                    is_flexible = true;
                    dim = 0;
                } else {
                    int64_t cd = zero_cc_eval_const_expr(c);
                    if (cd > 0) dim = (size_t)cd;
                }
                if (c->tokens[c->token_pos].type == TK_RBRACKET) c->token_pos++;
                if (is_flexible) break;
                elem_count *= dim;
                if (ndims < 8) dims[ndims++] = dim;
            }

            bool is_bitfield = false;
            uint8_t bit_width = 0;
            if (c->tokens[c->token_pos].type == TK_COLON) {
                is_bitfield = true;
                c->token_pos++;
                int64_t bw = zero_cc_eval_const_expr(c);
                bit_width = (bw > 0) ? (uint8_t)bw : 0;
            }
            /* GNU aligned(N) trailing the member declarator:
             * `short s __attribute__((aligned(2), packed));` */
            {
                size_t probe = 0;
                while (zero_cc_parse_aligned_attr(c, &c->token_pos, &probe)) {
                    if (probe > member_align) member_align = probe;
                }
            }

            if (is_union) {
                if (struct_member_room(c, &st) && mname[0] != '\0') {
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
                    if (is_typeof) {
                        m->tag = typeof_type.tag;
                        if (typeof_type.base) m->base = zero_cc_type_alloc_base(typeof_type.base);
                    }
                    else if (mt == TK_CHAR || mt == TK_BOOL_KW) m->tag = is_unsigned ? TYPE_UINT8 : TYPE_INT8;
                    else if (mt == TK_SHORT) m->tag = is_unsigned ? TYPE_UINT16 : TYPE_INT16;
                    else if (mt == TK_FLOAT_KW) m->tag = TYPE_FLOAT;
                    else if (mt == TK_INT) m->tag = is_unsigned ? TYPE_UINT32 : TYPE_INT32;
                    else if (mt == TK_DOUBLE_KW) m->tag = TYPE_DOUBLE;
                    else if (mt == TK_LONG || mt == TK_VOID) m->tag = (mt == TK_VOID) ? TYPE_INT64 : (is_unsigned ? TYPE_UINT64 : TYPE_INT64);
                    else if (mt == TK_IDENT) {
                        ZeroCCType *td = zero_cc_find_typedef(c, type_ident);
                        if (!td) td = zero_cc_find_struct(c, type_ident);
                        if (td && td->tag == TYPE_UNION) m->tag = TYPE_UNION;
                        else m->tag = TYPE_STRUCT;
                        /* By-value member: base = the struct itself. Pointer
                         * member (`t *p`): base = the pointee (C5) so the
                         * named struct's fields are available. */
                        if (td) {
                            m->base = zero_cc_type_alloc_base(td);
                        }
                    }
                    else m->tag = TYPE_INT32;
                    if (is_ptr) {
                        if (!m->base) {
                            ZeroCCType dummy;
                            memset(&dummy, 0, sizeof(dummy));
                            dummy.tag = (mt == TK_VOID && ptr_depth > 1) ? TYPE_VOID : m->tag;
                            dummy.size = base_size;
                            dummy.align = base_align;
                            dummy.is_const = member_is_const;
                            dummy.is_volatile = member_is_volatile;
                            m->base = zero_cc_type_alloc_base(&dummy);
                        }
                        m->tag = TYPE_PTR;
                        zero_cc_member_wrap_ptr_levels(m, ptr_depth - 1);
                        if (!is_bitfield && (is_array || is_flexible)) zero_cc_member_make_ptr_array(m);
                    } else if (!is_bitfield && (is_array || is_flexible)) {
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
                        elem_t = zero_cc_member_nest_array_elem(elem_t, dims, ndims);
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
                            if (mname[0] != '\0' && struct_member_room(c, &st)) {
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
                                if (is_typeof) {
                        m->tag = typeof_type.tag;
                        if (typeof_type.base) m->base = zero_cc_type_alloc_base(typeof_type.base);
                    }
                    else if (mt == TK_CHAR || mt == TK_BOOL_KW) m->tag = is_unsigned ? TYPE_UINT8 : TYPE_INT8;
                                else if (mt == TK_SHORT) m->tag = is_unsigned ? TYPE_UINT16 : TYPE_INT16;
                                else if (mt == TK_FLOAT_KW) m->tag = TYPE_FLOAT;
                                else if (mt == TK_INT) m->tag = is_unsigned ? TYPE_UINT32 : TYPE_INT32;
                                else if (mt == TK_DOUBLE_KW) m->tag = TYPE_DOUBLE;
                                else if (mt == TK_LONG || mt == TK_VOID) m->tag = (mt == TK_VOID) ? TYPE_INT64 : (is_unsigned ? TYPE_UINT64 : TYPE_INT64);
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
                            if (mname[0] != '\0' && struct_member_room(c, &st)) {
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
                                if (is_typeof) {
                        m->tag = typeof_type.tag;
                        if (typeof_type.base) m->base = zero_cc_type_alloc_base(typeof_type.base);
                    }
                    else if (mt == TK_CHAR || mt == TK_BOOL_KW) m->tag = is_unsigned ? TYPE_UINT8 : TYPE_INT8;
                                else if (mt == TK_SHORT) m->tag = is_unsigned ? TYPE_UINT16 : TYPE_INT16;
                                else if (mt == TK_FLOAT_KW) m->tag = TYPE_FLOAT;
                                else if (mt == TK_INT) m->tag = is_unsigned ? TYPE_UINT32 : TYPE_INT32;
                                else if (mt == TK_DOUBLE_KW) m->tag = TYPE_DOUBLE;
                                else if (mt == TK_LONG || mt == TK_VOID) m->tag = (mt == TK_VOID) ? TYPE_INT64 : (is_unsigned ? TYPE_UINT64 : TYPE_INT64);
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
                    if (mname[0] != '\0' && struct_member_room(c, &st)) {
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
                        if (is_typeof) {
                        m->tag = typeof_type.tag;
                        if (typeof_type.base) m->base = zero_cc_type_alloc_base(typeof_type.base);
                    }
                    else if (mt == TK_CHAR || mt == TK_BOOL_KW) m->tag = is_unsigned ? TYPE_UINT8 : TYPE_INT8;
                        else if (mt == TK_SHORT) m->tag = is_unsigned ? TYPE_UINT16 : TYPE_INT16;
                        else if (mt == TK_FLOAT_KW) m->tag = TYPE_FLOAT;
                        else if (mt == TK_INT) m->tag = is_unsigned ? TYPE_UINT32 : TYPE_INT32;
                        else if (mt == TK_DOUBLE_KW) m->tag = TYPE_DOUBLE;
                        else if (mt == TK_LONG || mt == TK_VOID) m->tag = (mt == TK_VOID) ? TYPE_INT64 : (is_unsigned ? TYPE_UINT64 : TYPE_INT64);
                        else if (mt == TK_IDENT) {
                            ZeroCCType *td = zero_cc_find_typedef(c, type_ident);
                            if (!td) td = zero_cc_find_struct(c, type_ident);
                            if (td && td->tag == TYPE_UNION) m->tag = TYPE_UNION;
                            else m->tag = TYPE_STRUCT;
                            /* By-value member: base = the struct itself. Pointer
                             * member (`t *p`): base = the pointee (C5). */
                            if (td) {
                                m->base = zero_cc_type_alloc_base(td);
                            }
                        }
                        else m->tag = TYPE_INT32;
                        if (member_vec_size > 0 && !is_ptr && m->base == NULL &&
                            !is_array && !is_flexible && !is_bitfield) {
                            ZeroCCType elem;
                            memset(&elem, 0, sizeof(elem));
                            elem.tag = m->tag;
                            elem.size = base_size;
                            elem.align = base_align;
                            elem.is_const = member_is_const;
                            elem.is_volatile = member_is_volatile;
                            ZeroCCType vt = zero_cc_type_vector(&elem, member_vec_size);
                            m->tag = vt.tag;
                            m->size = vt.size;
                            m->align = vt.align;
                            m->base = vt.base;
                            if (m->align > st.align) st.align = m->align;
                        }
                        if (is_ptr) {
                            if (!m->base) {
                                ZeroCCType dummy;
                                memset(&dummy, 0, sizeof(dummy));
                                dummy.tag = (mt == TK_VOID && ptr_depth > 1) ? TYPE_VOID : m->tag;
                                dummy.size = base_size;
                                dummy.align = base_align;
                                dummy.is_const = member_is_const;
                                dummy.is_volatile = member_is_volatile;
                                m->base = zero_cc_type_alloc_base(&dummy);
                            }
                            m->tag = TYPE_PTR;
                        zero_cc_member_wrap_ptr_levels(m, ptr_depth - 1);
                        if (!is_bitfield && (is_array || is_flexible)) zero_cc_member_make_ptr_array(m);
                        } else if (is_array || is_flexible) {
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
                            elem_t = zero_cc_member_nest_array_elem(elem_t, dims, ndims);
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
                int c_ptr_depth = 0;
                bool c_member_ptr_is_const = false;
                bool c_member_ptr_is_volatile = false;
                while (c->tokens[c->token_pos].type == TK_STAR) {
                    c_is_ptr = true;
                    c_ptr_depth++;
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
                size_t c_dims[8] = {0};
                int c_ndims = 0;
                bool c_is_flexible = false;
                bool c_is_array = false;
                /* Multi-dimensional arrays: T a[d1][d2]... (C99 6.7.2.1).
                 * Dimensions are constant expressions (C99 6.7.2.1p3):
                 * `char buf[72*1024*1024];` - evaluated, not just a NUM. */
                while (c->tokens[c->token_pos].type == TK_LBRACKET) {
                    c_is_array = true;
                    c->token_pos++;
                    size_t dim = 1;
                    if (c->tokens[c->token_pos].type == TK_RBRACKET) {
                        c_is_flexible = true;
                        dim = 0;
                    } else {
                        int64_t cd = zero_cc_eval_const_expr(c);
                        if (cd > 0) dim = (size_t)cd;
                    }
                    if (c->tokens[c->token_pos].type == TK_RBRACKET) c->token_pos++;
                    if (c_is_flexible) break;
                    c_elem_count *= dim;
                    if (c_ndims < 8) c_dims[c_ndims++] = dim;
                }

                bool c_is_bitfield = false;
                uint8_t c_bit_width = 0;
                if (c->tokens[c->token_pos].type == TK_COLON) {
                    c_is_bitfield = true;
                    c->token_pos++;
                    int64_t bw = zero_cc_eval_const_expr(c);
                    c_bit_width = (bw > 0) ? (uint8_t)bw : 0;
                }
                /* GNU aligned(N) trailing the member declarator:
                 * `short s __attribute__((aligned(2), packed));` */
                {
                    size_t probe = 0;
                    while (zero_cc_parse_aligned_attr(c, &c->token_pos, &probe)) {
                        if (probe > member_align) member_align = probe;
                    }
                }

                if (is_union) {
                    if (struct_member_room(c, &st) && c_mname[0] != '\0') {
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
                        if (is_typeof) {
                        m->tag = typeof_type.tag;
                        if (typeof_type.base) m->base = zero_cc_type_alloc_base(typeof_type.base);
                    }
                    else if (mt == TK_CHAR || mt == TK_BOOL_KW) m->tag = is_unsigned ? TYPE_UINT8 : TYPE_INT8;
                        else if (mt == TK_SHORT) m->tag = is_unsigned ? TYPE_UINT16 : TYPE_INT16;
                        else if (mt == TK_FLOAT_KW) m->tag = TYPE_FLOAT;
                        else if (mt == TK_INT) m->tag = is_unsigned ? TYPE_UINT32 : TYPE_INT32;
                        else if (mt == TK_DOUBLE_KW) m->tag = TYPE_DOUBLE;
                        else if (mt == TK_LONG || mt == TK_VOID) m->tag = (mt == TK_VOID) ? TYPE_INT64 : (is_unsigned ? TYPE_UINT64 : TYPE_INT64);
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
                            dummy.tag = (mt == TK_VOID && c_ptr_depth > 1) ? TYPE_VOID : m->tag;
                            dummy.size = base_size;
                            dummy.align = base_align;
                            dummy.is_const = member_is_const;
                            dummy.is_volatile = member_is_volatile;
                            m->tag = TYPE_PTR;
                            m->base = zero_cc_type_alloc_base(&dummy);
                            zero_cc_member_wrap_ptr_levels(m, c_ptr_depth - 1);
                            if (!c_is_bitfield && (c_is_array || c_is_flexible)) zero_cc_member_make_ptr_array(m);
                        } else if (!c_is_bitfield && (c_is_array || c_is_flexible)) {
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
                            elem_t = zero_cc_member_nest_array_elem(elem_t, c_dims, c_ndims);
                            m->tag = TYPE_ARRAY;
                            m->base = zero_cc_type_alloc_base(&elem_t);
                        }
                        if (member_vec_size > 0 && !c_is_ptr && m->base == NULL &&
                            !c_is_array && !c_is_flexible && !c_is_bitfield) {
                            ZeroCCType elem;
                            memset(&elem, 0, sizeof(elem));
                            elem.tag = m->tag;
                            elem.size = base_size;
                            elem.align = base_align;
                            elem.is_const = member_is_const;
                            elem.is_volatile = member_is_volatile;
                            ZeroCCType vt = zero_cc_type_vector(&elem, member_vec_size);
                            m->tag = vt.tag;
                            m->size = vt.size;
                            m->align = vt.align;
                            m->base = vt.base;
                        }
                        if (m->size > max_size) max_size = m->size;
                    }
                } else if (c_is_bitfield) {
                    if (c_bit_width == 0) {
                        if (in_bitfield) {
                            current_offset = bitfield_storage_offset + bitfield_storage_size;
                            in_bitfield = false;
                        }
                        current_offset = (current_offset + c_effective_align - 1) & ~(c_effective_align - 1);
                    } else if (in_bitfield && bitfield_storage_size == c_base_size &&
                               (bitfield_bit_offset + c_bit_width <= c_base_size * 8)) {
                        size_t m_offset = bitfield_storage_offset;
                        uint8_t m_bit_off = (uint8_t)bitfield_bit_offset;
                        bitfield_bit_offset += c_bit_width;
                        if (c_mname[0] != '\0' && struct_member_room(c, &st)) {
                            ZeroCCStructMember *m = &st.members[st.member_count++];
                            snprintf(m->name, sizeof(m->name), "%s", c_mname);
                            m->offset = m_offset;
                            m->size = c_base_size;
                            m->align = c_effective_align;
                            m->is_flexible = false;
                            m->is_bitfield = true;
                            m->bit_offset = m_bit_off;
                            m->bit_width = c_bit_width;
                            m->is_signed = !is_unsigned;
                            m->tag = is_unsigned ? TYPE_UINT32 : TYPE_INT32;
                        }
                    } else {
                        if (in_bitfield) {
                            current_offset = bitfield_storage_offset + bitfield_storage_size;
                        }
                        current_offset = (current_offset + c_effective_align - 1) & ~(c_effective_align - 1);
                        bitfield_storage_offset = current_offset;
                        bitfield_storage_size = c_base_size;
                        bitfield_bit_offset = c_bit_width;
                        in_bitfield = true;
                        if (c_mname[0] != '\0' && struct_member_room(c, &st)) {
                            ZeroCCStructMember *m = &st.members[st.member_count++];
                            snprintf(m->name, sizeof(m->name), "%s", c_mname);
                            m->offset = bitfield_storage_offset;
                            m->size = c_base_size;
                            m->align = c_effective_align;
                            m->is_flexible = false;
                            m->is_bitfield = true;
                            m->bit_offset = 0;
                            m->bit_width = c_bit_width;
                            m->is_signed = !is_unsigned;
                            m->tag = is_unsigned ? TYPE_UINT32 : TYPE_INT32;
                        }
                    }
                } else {
                    if (in_bitfield) {
                        current_offset = bitfield_storage_offset + bitfield_storage_size;
                        in_bitfield = false;
                    }
                    current_offset = (current_offset + c_effective_align - 1) & ~(c_effective_align - 1);
                    if (c_mname[0] != '\0' && struct_member_room(c, &st)) {
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
                        if (is_typeof) {
                        m->tag = typeof_type.tag;
                        if (typeof_type.base) m->base = zero_cc_type_alloc_base(typeof_type.base);
                    }
                    else if (mt == TK_CHAR || mt == TK_BOOL_KW) m->tag = is_unsigned ? TYPE_UINT8 : TYPE_INT8;
                        else if (mt == TK_SHORT) m->tag = is_unsigned ? TYPE_UINT16 : TYPE_INT16;
                        else if (mt == TK_FLOAT_KW) m->tag = TYPE_FLOAT;
                        else if (mt == TK_INT) m->tag = is_unsigned ? TYPE_UINT32 : TYPE_INT32;
                        else if (mt == TK_DOUBLE_KW) m->tag = TYPE_DOUBLE;
                        else if (mt == TK_LONG || mt == TK_VOID) m->tag = (mt == TK_VOID) ? TYPE_INT64 : (is_unsigned ? TYPE_UINT64 : TYPE_INT64);
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
                            dummy.tag = (mt == TK_VOID && c_ptr_depth > 1) ? TYPE_VOID : m->tag;
                            dummy.size = base_size;
                            dummy.align = base_align;
                            dummy.is_const = member_is_const;
                            dummy.is_volatile = member_is_volatile;
                            m->tag = TYPE_PTR;
                            m->base = zero_cc_type_alloc_base(&dummy);
                            zero_cc_member_wrap_ptr_levels(m, c_ptr_depth - 1);
                            if (!c_is_bitfield && (c_is_array || c_is_flexible)) zero_cc_member_make_ptr_array(m);
                        } else if (c_is_array || c_is_flexible) {
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
                            elem_t = zero_cc_member_nest_array_elem(elem_t, c_dims, c_ndims);
                            m->tag = TYPE_ARRAY;
                            m->base = zero_cc_type_alloc_base(&elem_t);
                        }
                        if (member_vec_size > 0 && !c_is_ptr && m->base == NULL &&
                            !c_is_array && !c_is_flexible && !c_is_bitfield) {
                            ZeroCCType elem;
                            memset(&elem, 0, sizeof(elem));
                            elem.tag = m->tag;
                            elem.size = base_size;
                            elem.align = base_align;
                            elem.is_const = member_is_const;
                            elem.is_volatile = member_is_volatile;
                            ZeroCCType vt = zero_cc_type_vector(&elem, member_vec_size);
                            m->tag = vt.tag;
                            m->size = vt.size;
                            m->align = vt.align;
                            m->base = vt.base;
                        }
                        if (!c_is_flexible) {
                            current_offset += m->size;
                        }
                    }
                }
            }
        }
        if (c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
        if (c->token_pos == iter_start) {
            diag_report_error_at_token(c, "unexpected token in struct/union definition");
            c->token_pos++;
        }
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
        /* GNU aligned(N) between the tag keyword and the tag name:
         * `struct __attribute__((aligned(4))) S { ... };` (the alignment
         * usually matches the natural alignment; consume the marker). */
        {
            size_t probe = 0;
            while (zero_cc_parse_aligned_attr(c, &c->token_pos, &probe)) { /* skip */ }
        }
        char st_name[128] = "";
        if (c->tokens[c->token_pos].type == TK_IDENT) {
            snprintf(st_name, sizeof(st_name), "%s", c->tokens[c->token_pos].str_val);
            c->token_pos++;
        }
        if (c->tokens[c->token_pos].type == TK_LBRACE) {
            ZeroCCType st = zero_cc_parse_struct_body(c, is_union);
            /* GNU aligned(N) trailing the struct body:
             * `struct x { ... } __attribute__((aligned(32))) X;`
             * (preprocessor marker __zc_aligned__(N)). Raise the struct's
             * alignment and pad its size to a multiple of N before the
             * tag/alias/typedef registrations so every later use sees it. */
            {
                size_t probe = 0;
                if (zero_cc_parse_aligned_attr(c, &c->token_pos, &probe)) {
                    if (probe > st.align) {
                        st.align = probe;
                        if (st.size < probe) st.size = probe;
                        if ((st.size % probe) != 0) st.size = (st.size + probe - 1) / probe * probe;
                    }
                }
            }
            if (st_name[0] != '\0') {
                snprintf(st.struct_name, sizeof(st.struct_name), "%s", st_name);
                zero_cc_register_struct(c, st_name, st);
                /* Forward typedefs of this tag (typedef struct S S;) hold
                 * incomplete snapshots; refresh them with the definition. */
                zero_cc_sync_typedefs_with_struct(c, st_name, &st);
            }
            if (c->tokens[c->token_pos].type == TK_IDENT) {
                char alias[128];
                snprintf(alias, sizeof(alias), "%s", c->tokens[c->token_pos].str_val);
                c->token_pos++;
                if (is_typedef) {
                    /* typedef struct S {...} name; / typedef struct {...} name; */
                    if (st.struct_name[0] == '\0') {
                        snprintf(st.struct_name, sizeof(st.struct_name), "%s", alias);
                    }
                    zero_cc_register_struct(c, alias, st);
                    zero_cc_sync_typedefs_with_struct(c, st.struct_name, &st);
                    zero_cc_register_typedef(c, alias, st);
                } else {
                    /* C99 6.7.2.1: after a struct/union *definition* an
                     * identifier is a VARIABLE declarator (optionally
                     * comma-separated), not a tag alias:
                     *   struct S { int x; } a, b = { 42 };
                     * (Previously the name was registered as a struct tag
                     * alias and no variable was created - latent bug, the
                     * two-line form `struct S {...}; struct S a;` masked it.) */
                    ZeroCCType vt = st;
                    int vptr_depth = 0;
                    while (c->tokens[c->token_pos].type == TK_STAR) {
                        vptr_depth++;
                        c->token_pos++;
                    }
                    for (int pd = 0; pd < vptr_depth; pd++) vt = zero_cc_type_ptr(&vt);
                    int64_t varray_len = 1;
                    int vdim_count = 0;
                    int64_t vdims[8];
                    if (c->tokens[c->token_pos].type == TK_LBRACKET) {
                        while (c->tokens[c->token_pos].type == TK_LBRACKET) {
                            c->token_pos++;
                            if (c->tokens[c->token_pos].type == TK_RBRACKET) {
                                diag_report_error_at_token(c,
                                    "unsized array of %s with no initializer list is not supported",
                                    is_union ? "union" : "struct");
                                while (c->token_pos < c->token_count && c->tokens[c->token_pos].type != TK_SEMI) c->token_pos++;
                                if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
                                return;
                            }
                            int64_t dim = zero_cc_eval_const_expr(c);
                            if (c->tokens[c->token_pos].type == TK_RBRACKET) c->token_pos++;
                            if (dim < 0) dim = 0;
                            varray_len *= dim;
                            if (vdim_count < 8) vdims[vdim_count++] = dim;
                        }
                    }
                    do {
                        size_t elem_size = vt.size > 0 ? vt.size : 8;
                        size_t elem_align = vt.align > 0 ? vt.align : 8;
                        if (elem_align < 8) elem_align = 8;
                        size_t vsize = (size_t)varray_len * elem_size;
                        size_t vpad = (elem_align - (c->data_section.length % elem_align)) % elem_align;
                        for (size_t i = 0; i < vpad; i++) zero_cc_emit_u8(&c->data_section, 0);
                        size_t var_offset = c->data_section.length;
                        bool vhas_init = false;
                        ZeroCCType vreg_type = vt;
                        for (int d = vdim_count - 1; d >= 0; d--) {
                            vreg_type = zero_cc_type_array(&vreg_type, (ssize_t)vdims[d]);
                        }
                        if (c->tokens[c->token_pos].type == TK_ASSIGN) {
                            c->token_pos++;
                            vhas_init = true;
                            if (c->tokens[c->token_pos].type == TK_LBRACE) {
                                /* C99 6.7.2.1p3: a trailing flexible-array
                                 * member is initialized past the struct's fixed
                                 * size - grow the object to cover the FAM data
                                 * run (c-torture execute/20010924-1). */
                                size_t fam_extra = (vdim_count == 0 && vptr_depth == 0) ?
                                    zc_measure_fam_init_bytes(c, &vt) : 0;
                                if (fam_extra > 0) vsize += fam_extra;
                                uint8_t *vbuf = calloc(vsize > 0 ? vsize : 1, 1);
                                if (vbuf) {
                                    size_t top_cursor = 0;
                                    zero_cc_emit_brace_init_global(c, vreg_type, vbuf, 0, vsize, var_offset, &top_cursor, fam_extra);
                                    zero_cc_buf_put_bytes(&c->data_section, vbuf, vsize);
                                } else {
                                    for (size_t i = 0; i < vsize; i++) zero_cc_emit_u8(&c->data_section, 0);
                                    while (c->token_pos < c->token_count && c->tokens[c->token_pos].type != TK_RBRACE) c->token_pos++;
                                    if (c->token_pos < c->token_count) c->token_pos++;
                                }
                                free(vbuf);
                            } else {
                                bool vfl = (vt.tag == TYPE_FLOAT || vt.tag == TYPE_DOUBLE);
                                uint64_t bits;
                                if (vfl) { double dv = zero_cc_eval_const_expr_float(c); memcpy(&bits, &dv, sizeof(bits)); }
                                else { int64_t iv = zero_cc_eval_const_expr(c); bits = (uint64_t)iv; }
                                for (size_t b = 0; b < (elem_size > 8 ? 8 : elem_size); b++) {
                                    zero_cc_emit_u8(&c->data_section, (uint8_t)(bits >> (8 * b)));
                                }
                                for (size_t i = (elem_size > 8 ? 8 : elem_size); i < vsize; i++) zero_cc_emit_u8(&c->data_section, 0);
                            }
                        } else {
                            if (!(vhas_init || vsize < ZERO_CC_BSS_THRESHOLD)) {
                                /* Large zero-init -> .bss (no data-section bytes) */
                                if (vpad > 0 && c->data_section.length >= vpad) c->data_section.length -= vpad;
                                size_t bss_pad = (elem_align - (c->bss_size % elem_align)) % elem_align;
                                c->bss_size += bss_pad;
                                var_offset = c->bss_size;
                                c->bss_size += vsize;
                            } else {
                                for (size_t i = 0; i < vsize; i++) zero_cc_emit_u8(&c->data_section, 0);
                            }
                        }
                        if (add_global_var(alias, vreg_type, var_offset, vsize, false, vhas_init) < 0) {
                            diag_report_error_at_token(c, "out of memory registering global variable '%s'", alias);
                        }
                        if (c->tokens[c->token_pos].type == TK_COMMA) {
                            c->token_pos++;
                            if (c->token_pos >= c->token_count || c->tokens[c->token_pos].type != TK_IDENT) {
                                while (c->token_pos < c->token_count && c->tokens[c->token_pos].type != TK_SEMI) c->token_pos++;
                                if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
                                break;
                            }
                            snprintf(alias, sizeof(alias), "%s", c->tokens[c->token_pos].str_val);
                            c->token_pos++;
                            /* Comma declarator may carry its own pointer/array suffixes */
                            vt = st;
                            while (c->tokens[c->token_pos].type == TK_STAR) { vt = zero_cc_type_ptr(&vt); c->token_pos++; }
                            varray_len = 1; vdim_count = 0;
                            if (c->tokens[c->token_pos].type == TK_LBRACKET) {
                                while (c->tokens[c->token_pos].type == TK_LBRACKET) {
                                    c->token_pos++;
                                    if (c->tokens[c->token_pos].type == TK_RBRACKET) c->token_pos++;
                                    else {
                                        int64_t dim = zero_cc_eval_const_expr(c);
                                        if (c->tokens[c->token_pos].type == TK_RBRACKET) c->token_pos++;
                                        if (dim < 0) dim = 0;
                                        varray_len *= dim;
                                        if (vdim_count < 8) vdims[vdim_count++] = dim;
                                    }
                                }
                            }
                            continue;
                        }
                        break;
                    } while (1);
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
            zero_cc_parse_enum_members(c);
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
        zero_cc_parse_enum_members(c);
        if (en_name[0] != '\0') {
            zero_cc_register_enum(c, en_name, et);
        }
        /* After enum body, handle variable declarators (not typedef - typedef
         * case is handled by zero_cc_stmt_typedef_enum). */
        while (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_IDENT) {
            char var_name[128];
            snprintf(var_name, sizeof(var_name), "%s", c->tokens[c->token_pos].str_val);
            c->token_pos++;
            /* Register as a variable of the enum type */
            size_t var_offset = c->data_section.length;
            /* Emit zero-initialized data for the enum variable (4 bytes) */
            for (size_t i = 0; i < et.size; i++) {
                zero_cc_emit_u8(&c->data_section, 0);
            }
            if (add_global_var(var_name, et, var_offset, et.size, false, false) < 0) {
                diag_report_error_at_token(c, "out of memory registering global variable");
            }
            if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_COMMA) {
                c->token_pos++;
            } else {
                break;
            }
        }
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
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
    bool is_atomic = false;

    /* Handle _Atomic qualifier at the start: typedef _Atomic int x; */
    if (t == TK_ATOMIC) {
        is_atomic = true;
        c->token_pos++;
        t = c->tokens[c->token_pos].type;
    }

    /* GNU vector attribute leading the typedef base:
     * `typedef __attribute__((__vector_size__(16))) short V;` */
    size_t vec_attr_size = 0;
    size_t align_attr_val = 0;
    {
        size_t save = c->token_pos;
        size_t probe = 0;
        if (zero_cc_parse_vector_attr(c, &c->token_pos, &probe)) {
            ZeroCCTokType nt = c->token_pos < c->token_count ? c->tokens[c->token_pos].type : TK_EOF;
            if (nt == TK_INT || nt == TK_CHAR || nt == TK_SHORT || nt == TK_LONG ||
                nt == TK_SIGNED || nt == TK_UNSIGNED || nt == TK_FLOAT_KW ||
                nt == TK_DOUBLE_KW || nt == TK_BOOL_KW || nt == TK_VOID) {
                vec_attr_size = probe;
                t = nt;
            } else {
                c->token_pos = save;
            }
        }
        if (vec_attr_size == 0) {
            size_t save2 = c->token_pos;
            if (zero_cc_parse_aligned_attr(c, &c->token_pos, &probe)) {
                ZeroCCTokType nt = c->token_pos < c->token_count ? c->tokens[c->token_pos].type : TK_EOF;
                if (nt == TK_INT || nt == TK_CHAR || nt == TK_SHORT || nt == TK_LONG ||
                    nt == TK_SIGNED || nt == TK_UNSIGNED || nt == TK_FLOAT_KW ||
                    nt == TK_DOUBLE_KW || nt == TK_BOOL_KW || nt == TK_VOID) {
                    align_attr_val = probe;
                    t = nt;
                } else {
                    c->token_pos = save2;
                }
            }
        }
    }

    if (t == TK_UNSIGNED) { is_unsigned = true; c->token_pos++; t = c->tokens[c->token_pos].type; }
    else if (t == TK_SIGNED) { is_signed = true; c->token_pos++; t = c->tokens[c->token_pos].type; }

    if (t == TK_TYPEOF || t == TK_TYPEOF_UNQUAL) {
        ttype = zero_cc_parse_type_name_at_pos(c, &c->token_pos);
    }
    else if (t == TK_INT) { ttype = is_unsigned ? zero_cc_type_uint32() : zero_cc_type_int(); c->token_pos++; }
    else if (t == TK_CHAR) { ttype = is_unsigned ? zero_cc_type_uint8() : zero_cc_type_char(); c->token_pos++; }
    else if (t == TK_VOID) { ttype = zero_cc_type_void(); c->token_pos++; }
    else if (t == TK_FLOAT_KW) { ttype = zero_cc_type_float(); c->token_pos++; }
    else if (t == TK_DOUBLE_KW) { ttype = zero_cc_type_double(); c->token_pos++; }
    else if (t == TK_BOOL_KW) { ttype = zero_cc_type_bool(); c->token_pos++; }
    else if (t == TK_SHORT) {
        c->token_pos++;
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_INT) c->token_pos++;
        ttype = is_unsigned ? zero_cc_type_uint16() : zero_cc_type_int16();
    }
    else if (t == TK_LONG) {
        c->token_pos++;
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LONG) c->token_pos++;
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_INT) c->token_pos++;
        ttype = is_unsigned ? zero_cc_type_uint64() : zero_cc_type_int64();
    }
    else if (t == TK_IDENT) {
        const char *tname = c->tokens[c->token_pos].str_val;
        ZeroCCType *found = zero_cc_find_typedef(c, tname);
        if (found) {
            ttype = *found;
            c->token_pos++;
        } else if (!is_unsigned && !is_signed) {
            ttype = zero_cc_type_int();
            c->token_pos++;
        } else {
            ttype = is_unsigned ? zero_cc_type_uint32() : zero_cc_type_int();
        }
    } else {
        ttype = is_unsigned ? zero_cc_type_uint32() : zero_cc_type_int();
    }
    (void)is_signed;
    if (is_atomic) ttype.is_atomic = true;
    /* GNU vector attribute trailing the base: `typedef signed char V
     * __attribute__((__vector_size__(16)));` - wraps the element type in a
     * vector ZeroCCType (TYPE_USER_0) with the given total width. */
    {
        size_t probe = 0;
        if (vec_attr_size == 0 && zero_cc_parse_vector_attr(c, &c->token_pos, &probe)) {
            vec_attr_size = probe;
        }
        if (align_attr_val == 0 && zero_cc_parse_aligned_attr(c, &c->token_pos, &probe)) {
            align_attr_val = probe;
        }
    }
    if (vec_attr_size > 0 && !zero_cc_type_is_vector(&ttype)) {
        ttype = zero_cc_type_vector(&ttype, vec_attr_size);
    }
    if (align_attr_val > 0 && align_attr_val > ttype.align) {
        ttype.align = align_attr_val;
        if (ttype.size < align_attr_val) ttype.size = align_attr_val;
        if ((ttype.size % align_attr_val) != 0) {
            ttype.size = (ttype.size + align_attr_val - 1) / align_attr_val * align_attr_val;
        }
    }
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
        /* GNU vector attribute trailing the typedef name (a declarator
         * attribute): `typedef signed char V __attribute__((__vector_size__(16)));`
         * Must be consumed before the function-typedef check below so the
         * marker's `(` is not read as a parameter list. */
        {
            size_t probe = 0;
            if (vec_attr_size == 0 && zero_cc_parse_vector_attr(c, &c->token_pos, &probe)) {
                vec_attr_size = probe;
            }
        }
        /* Function-type typedef (C99 6.7): `typedef int f(void);` or
         * `typedef int binop(int, int);`.  Consume the balanced parameter
         * list so the declarator is not left dangling at file scope, and
         * mark the alias as a function type; `f *p` then yields a
         * function pointer via zero_cc_chunk_apply. */
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LPAREN) {
            int pdepth = 1;
            c->token_pos++;
            while (c->token_pos < c->token_count && pdepth > 0) {
                if (c->tokens[c->token_pos].type == TK_LPAREN) pdepth++;
                else if (c->tokens[c->token_pos].type == TK_RPAREN) pdepth--;
                c->token_pos++;
            }
            ttype.is_func = true;
        }
        if (vec_attr_size > 0 && !zero_cc_type_is_vector(&ttype)) {
            ttype = zero_cc_type_vector(&ttype, vec_attr_size);
        }
        zero_cc_register_typedef(c, alias, ttype);
    }
    if (c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
}

#endif /* ZERO_CC_STATEMENTS_DEFINITIONS_H */
