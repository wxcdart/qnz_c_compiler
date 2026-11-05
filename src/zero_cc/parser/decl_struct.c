#include "zero_cc/parser/decl_struct.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "zero_cc/types/types.h"
#include "zero_cc/types/global_vars.h"
#include "zero_cc/semantic/diagnostics.h"
#include "zero_cc/parser/const_eval.h"
#include "zero_cc/parser/decl_enum.h"
#include "zero_cc/parser/type_parser.h"
#include "zero_cc/parser/ast_expr.h"
#include "zero_cc/parser/vla_struct.h"

void zero_cc_sync_globals_with_struct(ZeroCCCompiler *c, const char *name, const ZeroCCType *complete) {
    if (!c || !name || name[0] == '\0' || !complete) return;
    for (size_t i = 0; i < c->global_var_count; i++) {
        zero_cc_type_sync_struct(&c->global_vars[i].type, name, complete);
    }
    for (size_t i = 0; i < c->function_count; i++) {
        zero_cc_type_sync_struct(&c->functions[i].return_type, name, complete);
        for (size_t p = 0; p < c->functions[i].param_count; p++) {
            zero_cc_type_sync_struct(&c->functions[i].param_types[p], name, complete);
        }
    }
    /* Struct member tables: a self-referential pointer member
     * (`struct S { ...; struct S *next; }`) recorded the pointee as an
     * INCOMPLETE snapshot while the body was being parsed.  Without this
     * refresh the pointee stays fieldless forever, member access through
     * it (`t.next->d`) falls back to the lenient int default, and the
     * lowering loads a scalar from the garbage pointer (segfault). */
    for (size_t i = 0; i < c->struct_count; i++) {
        ZeroCCType *st2 = &c->structs[i].type;
        for (size_t mi = 0; mi < st2->member_count; mi++) {
            if (st2->members[mi].base) {
                zero_cc_type_sync_struct(st2->members[mi].base, name, complete);
            }
        }
    }
}



/* M3: the struct/union member table is heap-backed (it was a fixed 64-entry
 * array and the 65th+ member was silently dropped). Reserve one slot for the
 * next member; on OOM report a clean fatal diagnostic (had_error => no binary). */
static bool struct_member_room(ZeroCCCompiler *c, ZeroCCType *st) {
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
static ZeroCCType zero_cc_member_nest_array_elem(ZeroCCType elem, const size_t *dims, int ndims) {
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


/* A pointer member `T **p` records only one TYPE_PTR level (tag=PTR, base=T).
 * Wrap the pointee `extra` more times so the member's type is T with the
 * full star count, matching how locals/params are typed. */
static void zero_cc_member_wrap_ptr_levels(ZeroCCStructMember *m, int extra) {
    for (int i = 0; i < extra && m->base; i++) {
        ZeroCCType pt = zero_cc_type_ptr(m->base);
        m->base = zero_cc_type_alloc_base(&pt);
    }
}

/* `T *a[N]` is an array of N pointers.  The member parser builds the pointer
 * (tag=PTR, base=pointee); turn it into TYPE_ARRAY-of-pointer when the
 * declarator has an array suffix (previously the array dimension was dropped
 * for pointer members, so `a[1]` indexed past a single pointer). */
static void zero_cc_member_make_ptr_array(ZeroCCStructMember *m) {
    ZeroCCType pt;
    memset(&pt, 0, sizeof(pt));
    pt.tag = TYPE_PTR;
    pt.size = 8;
    pt.align = 8;
    pt.base = m->base;
    m->tag = TYPE_ARRAY;
    m->base = zero_cc_type_alloc_base(&pt);
}

static void zero_cc_setup_bitfield_type(ZeroCCCompiler *c, ZeroCCStructMember *m,
                                               ZeroCCTokType mt, const char *type_ident, bool is_unsigned) {
    if (mt == TK_CHAR || mt == TK_BOOL_KW) {
        m->tag = is_unsigned ? TYPE_UINT8 : TYPE_INT8;
        m->is_signed = !is_unsigned;
    } else if (mt == TK_SHORT) {
        m->tag = is_unsigned ? TYPE_UINT16 : TYPE_INT16;
        m->is_signed = !is_unsigned;
    } else if (mt == TK_INT) {
        m->tag = is_unsigned ? TYPE_UINT32 : TYPE_INT32;
        m->is_signed = !is_unsigned;
    } else if (mt == TK_LONG || mt == TK_VOID) {
        m->tag = is_unsigned ? TYPE_UINT64 : TYPE_INT64;
        m->is_signed = !is_unsigned;
    } else if (mt == TK_IDENT) {
        int k128 = zc_name_int128_kind(type_ident);
        if (k128) {
            m->tag = (k128 == 2 || is_unsigned) ? TYPE_UINT128 : TYPE_INT128;
            m->is_signed = (k128 != 2 && !is_unsigned);
            return;
        }
        ZeroCCType *td = zero_cc_find_typedef(c, type_ident);
        if (!td) td = zero_cc_find_enum(c, type_ident);
        if (td && td->tag == TYPE_BOOL) {
            m->tag = TYPE_BOOL;
            m->is_signed = false;
        } else if ((td && td->tag == TYPE_ENUM) || zero_cc_find_enum(c, type_ident)) {
            m->tag = TYPE_UINT32;
            m->is_signed = false;
        } else if (td && (td->tag == TYPE_UINT8 || td->tag == TYPE_UINT16 ||
                          td->tag == TYPE_UINT32 || td->tag == TYPE_UINT64)) {
            m->tag = td->tag;
            m->is_signed = false;
        } else if (td) {
            m->tag = td->tag;
            m->is_signed = !is_unsigned;
        } else {
            m->tag = is_unsigned ? TYPE_UINT32 : TYPE_INT32;
            m->is_signed = !is_unsigned;
        }
    } else {
        m->tag = is_unsigned ? TYPE_UINT32 : TYPE_INT32;
        m->is_signed = !is_unsigned;
    }
}

ZeroCCType zero_cc_parse_struct_body(ZeroCCCompiler *c, bool is_union) {
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

        /* Nested struct/union/enum or anonymous member.  A run of leading
         * qualifiers may precede the keyword (`const struct S *p;`): probe
         * past them so the branch fires, then consume them here. */
        {
            size_t st_probe = c->token_pos;
            bool st_lead_const = false;
            bool st_lead_volatile = false;
            while (st_probe < c->token_count &&
                   (c->tokens[st_probe].type == TK_CONST ||
                    c->tokens[st_probe].type == TK_VOLATILE ||
                    c->tokens[st_probe].type == TK_RESTRICT)) st_probe++;
            mt = (st_probe != c->token_pos && st_probe < c->token_count &&
                  (c->tokens[st_probe].type == TK_STRUCT ||
                   c->tokens[st_probe].type == TK_UNION ||
                   c->tokens[st_probe].type == TK_ENUM)) ?
                 c->tokens[st_probe].type : mt;
            if (mt == TK_STRUCT || mt == TK_UNION || mt == TK_ENUM) {
            bool is_enum = (mt == TK_ENUM);
            bool inner_is_union = (mt == TK_UNION);
            /* Consume the leading qualifiers probed above (they precede
             * the struct keyword when present). */
            while (c->token_pos < c->token_count &&
                   (c->tokens[c->token_pos].type == TK_CONST ||
                    c->tokens[c->token_pos].type == TK_VOLATILE ||
                    c->tokens[c->token_pos].type == TK_RESTRICT)) {
                if (c->tokens[c->token_pos].type == TK_CONST) st_lead_const = true;
                if (c->tokens[c->token_pos].type == TK_VOLATILE) st_lead_volatile = true;
                c->token_pos++;
            }
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
            int ptr_depth = 0;
            while (c->tokens[c->token_pos].type == TK_STAR) {
                is_ptr = true;
                ptr_depth++;
                c->token_pos++;
            }
            /* Sprint 72: function-pointer member with a struct/union/enum
             * return type (`struct B *(*chunkfun)(void *, long);`) - the
             * `(` after the star run is a funcptr declarator (same shape
             * as the generic member path).  The member is a plain 8-byte
             * pointer; is_func_ptr marks that the pointee is a function
             * returning the member's own type (`struct B *`). */
            bool dcl_is_fnptr = false;
            char mname[128] = "";
            if (c->tokens[c->token_pos].type == TK_LPAREN &&
                c->token_pos + 1 < c->token_count &&
                c->tokens[c->token_pos + 1].type == TK_STAR) {
                c->token_pos++; /* ( */
                c->token_pos++; /* * */
                dcl_is_fnptr = true;
                is_ptr = true;
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
            } else if (c->tokens[c->token_pos].type == TK_IDENT) {
                snprintf(mname, sizeof(mname), "%s", c->tokens[c->token_pos].str_val);
                c->token_pos++;
            }
            size_t elem_count = 1;
            size_t dims[8] = {0};
            int ndims = 0;
            bool is_flexible = false;
            bool is_array = false;
            /* GNU variably-modified member (`char b[argc];`): the dimension
             * is a runtime expression parsed into the TU arena. At most one
             * runtime dimension per member (c-torture nested-1.c). */
            bool member_vla = false;
            size_t member_vla_expr = 0;
            int member_vla_count = 0;
            /* Multi-dimensional arrays: T a[d1][d2]... (C99 6.7.2.1).
             * Dimensions are constant expressions (6.7.2.1p3). */
            while (c->tokens[c->token_pos].type == TK_LBRACKET) {
                is_array = true;
                c->token_pos++;
                size_t dim = 1;
                if (c->tokens[c->token_pos].type == TK_RBRACKET) {
                    is_flexible = true;
                    dim = 0;
                } else if (zc_typedef_dim_is_const(c, c->token_pos)) {
                    int64_t cd = zero_cc_eval_const_expr(c);
                    if (cd >= 0) { dim = (size_t)cd; if (cd == 0) is_flexible = true; }
                } else if (is_union) {
                    diag_report_error_at_token(c,
                        "union member with variably-modified type is not supported");
                    member_vla_count = 2; /* suppress later handling */
                    c->token_pos++;
                    dim = 1;
                } else {
                    member_vla = true;
                    member_vla_count++;
                    if (member_vla_count == 1)
                        member_vla_expr = parse_expr_ast_internal(c, (ZeroCCASTArena *)c->tu_ast_arena);
                    else
                        diag_report_error_at_token(c,
                            "multiple variable dimensions in a struct member are not supported");
                    dim = 2; /* GCC placeholder for the static layout */
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
                zero_cc_skip_mode_attr(c);
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
                    bool t_signed = !is_enum && (inner_type.tag != TYPE_BOOL && inner_type.tag != TYPE_UINT8 &&
                                                 inner_type.tag != TYPE_UINT16 && inner_type.tag != TYPE_UINT32 &&
                                                 inner_type.tag != TYPE_UINT64);
                    m->is_signed = t_signed;
                    m->is_func_ptr = dcl_is_fnptr;
                    if (is_bitfield) {
                        m->tag = (inner_type.tag == TYPE_BOOL) ? TYPE_BOOL : (t_signed ? TYPE_INT32 : TYPE_UINT32);
                    } else {
                        m->tag = is_ptr ? TYPE_PTR : (is_enum ? TYPE_ENUM : (inner_is_union ? TYPE_UNION : TYPE_STRUCT));
                    }
                    /* A leading qualifier on a struct/union/enum member
                     * attaches to the BASE type (C99 6.7.3): `const struct S *p`
                     * is a pointer to const S, so the pointee must carry it
                     * (previously dropped - `t.p = &const_global` REJECTed as
                     * an incompatible assignment, c-torture 20040708-1). */
                    ZeroCCType pt = inner_type;
                    if (is_ptr) {
                        if (st_lead_const) pt.is_const = true;
                        if (st_lead_volatile) pt.is_volatile = true;
                        if (dcl_is_fnptr) {
                            /* Function-pointer member: the member's base is
                             * the function's RETURN type.  For
                             * `struct B *(*f)()` the star belongs to the
                             * return (`struct B *`), so wrap the pointee in
                             * a pointer (c-torture 20010102-1: CALL_CHUNKFUN
                             * assigned the call result to a
                             * `struct _obstack_chunk *`). */
                            ZeroCCType rt = zero_cc_type_ptr(&pt);
                            m->base = zero_cc_type_alloc_base(&rt);
                        } else {
                            m->base = zero_cc_type_alloc_base(&pt);
                        }
                    } else {
                        m->base = zero_cc_type_alloc_base(&inner_type);
                        m->is_const = st_lead_const;
                        m->is_volatile = st_lead_volatile;
                    }
                    if (!is_ptr && !is_bitfield && (is_array || is_flexible)) {
                        m->tag = TYPE_ARRAY; /* array-of-struct member in union */
                        if (ndims > 1) {
                            ZeroCCType et = inner_type;
                            et = zero_cc_member_nest_array_elem(et, dims, ndims);
                            m->base = zero_cc_type_alloc_base(&et);
                        }
                    }
                    if (is_ptr && is_array && !is_flexible && !is_bitfield) {
                        /* Array of pointers to struct/union/enum in a union
                         * (`struct S *arr[N]`): element is the pointer type. */
                        ZeroCCType ptelem;
                        memset(&ptelem, 0, sizeof(ptelem));
                        ptelem.tag = TYPE_PTR;
                        ptelem.size = 8;
                        ptelem.align = 8;
                        ptelem.base = m->base;
                        if (ndims > 1) {
                            ptelem = zero_cc_member_nest_array_elem(ptelem, dims, ndims);
                        }
                        m->tag = TYPE_ARRAY;
                        m->base = zero_cc_type_alloc_base(&ptelem);
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
                } else if (in_bitfield && base_size <= bitfield_storage_size &&
                           (bitfield_bit_offset + bit_width <= bitfield_storage_size * 8)) {
                    size_t m_offset = bitfield_storage_offset;
                    uint8_t m_bit_off = (uint8_t)bitfield_bit_offset;
                    bitfield_bit_offset += bit_width;
                    if (mname[0] != '\0' && struct_member_room(c, &st)) {
                        ZeroCCStructMember *m = &st.members[st.member_count++];
                        snprintf(m->name, sizeof(m->name), "%s", mname);
                        m->offset = m_offset;
                        /* A narrower base type packs into the remaining bits of
                         * the wider allocation unit (GCC layout): the member's
                         * size is the UNIT size so bit-field load/store and
                         * leaf resolution operate on the whole unit. */
                        m->size = bitfield_storage_size;
                        m->align = effective_align;
                        m->is_flexible = false;
                        m->is_bitfield = true;
                        m->bit_offset = m_bit_off;
                        m->bit_width = bit_width;
                        bool t_signed = !is_enum && (inner_type.tag != TYPE_BOOL && inner_type.tag != TYPE_UINT8 &&
                                                     inner_type.tag != TYPE_UINT16 && inner_type.tag != TYPE_UINT32 &&
                                                     inner_type.tag != TYPE_UINT64);
                        m->is_signed = t_signed;
                        m->tag = (inner_type.tag == TYPE_BOOL) ? TYPE_BOOL : (t_signed ? TYPE_INT32 : TYPE_UINT32);
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
                        bool t_signed = !is_enum && (inner_type.tag != TYPE_BOOL && inner_type.tag != TYPE_UINT8 &&
                                                     inner_type.tag != TYPE_UINT16 && inner_type.tag != TYPE_UINT32 &&
                                                     inner_type.tag != TYPE_UINT64);
                        m->is_signed = t_signed;
                        m->tag = (inner_type.tag == TYPE_BOOL) ? TYPE_BOOL : (t_signed ? TYPE_INT32 : TYPE_UINT32);
                    }
                }
            } else {
                /* Align offset based on effective_align (already pack-capped) */
                current_offset = (current_offset + effective_align - 1) & ~(effective_align - 1);
                if (mname[0] != '\0' && struct_member_room(c, &st)) {
                    ZeroCCStructMember *m = &st.members[st.member_count++];
                    snprintf(m->name, sizeof(m->name), "%s", mname);
                    m->offset = current_offset;
                    m->size = is_flexible ? 0 : (member_vla ? 0 : (base_size * elem_count));
                    m->align = effective_align;
                    m->is_flexible = is_flexible;
                    m->is_bitfield = false;
                    m->is_func_ptr = dcl_is_fnptr;
                    if (member_vla) {
                        m->is_vla = true;
                        m->vla_dim = member_vla_expr;
                        m->vla_elem_size = base_size * (elem_count / 2);
                    }
                    m->tag = is_ptr ? TYPE_PTR : (is_enum ? TYPE_ENUM : (inner_is_union ? TYPE_UNION : TYPE_STRUCT));
                    /* Leading qualifier on a struct/union/enum member attaches
                     * to the base type: `const struct S *p` = pointer to
                     * const S (see the union branch above). */
                    ZeroCCType pt = inner_type;
                    if (is_ptr) {
                        if (st_lead_const) pt.is_const = true;
                        if (st_lead_volatile) pt.is_volatile = true;
                        if (dcl_is_fnptr) {
                            /* Function-pointer member: base = the function's
                             * RETURN type; `struct B *(*f)()` returns
                             * `struct B *` (see the union branch above). */
                            ZeroCCType rt = zero_cc_type_ptr(&pt);
                            m->base = zero_cc_type_alloc_base(&rt);
                        } else {
                            m->base = zero_cc_type_alloc_base(&pt);
                        }
                    } else {
                        m->base = zero_cc_type_alloc_base(&inner_type);
                        m->is_const = st_lead_const;
                        m->is_volatile = st_lead_volatile;
                    }
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
                    if (is_ptr && is_array && !is_flexible && !is_bitfield) {
                        /* Array of pointers to struct/union/enum
                         * (`struct S *arr[N]`): the member is an array whose
                         * element is the pointer type (c-torture 20020402-3,
                         * pr39041).  m->base is the pointed-to type from the
                         * is_ptr branch; wrap it in a pointer to form the
                         * array element, and nest any further dims. */
                        ZeroCCType ptelem;
                        memset(&ptelem, 0, sizeof(ptelem));
                        ptelem.tag = TYPE_PTR;
                        ptelem.size = 8;
                        ptelem.align = 8;
                        ptelem.base = m->base;
                        if (ndims > 1) {
                            ptelem = zero_cc_member_nest_array_elem(ptelem, dims, ndims);
                        }
                        m->tag = TYPE_ARRAY;
                        m->base = zero_cc_type_alloc_base(&ptelem);
                    }
                    if (is_ptr && !is_array && !dcl_is_fnptr && ptr_depth > 1) {
                        /* Multi-level pointer member (`struct A **a`): apply
                         * the extra pointer levels (c-torture 20010409-1). */
                        zero_cc_member_wrap_ptr_levels(m, ptr_depth - 1);
                    }
                    if (!is_flexible) {
                        /* A VLA member occupies its 2-element placeholder in
                         * the static layout (GCC); the runtime size is
                         * re-computed from the real dimension. */
                        current_offset += m->size + (member_vla ? 2 * m->vla_elem_size : 0);
                    }
                }
            }
            if (c->tokens[c->token_pos].type == TK_SEMI) {
                c->token_pos++;
            } else if (c->tokens[c->token_pos].type == TK_COMMA) {
                /* Sprint 72: comma declarators of struct/union/enum
                 * members (`struct s5 x, y;`, `struct qhdr *link, *rlink;`;
                 * c-torture 20011229-1, 920428-2).  Each declarator keeps
                 * the member's base type; its own star run decides
                 * pointer-ness (C99 6.7.5). */
                while (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_COMMA) {
                    c->token_pos++;
                    int cptr = 0;
                    while (c->token_pos < c->token_count &&
                           (c->tokens[c->token_pos].type == TK_STAR ||
                            c->tokens[c->token_pos].type == TK_CONST ||
                            c->tokens[c->token_pos].type == TK_VOLATILE)) {
                        if (c->tokens[c->token_pos].type == TK_STAR) cptr++;
                        c->token_pos++;
                    }
                    if (c->token_pos >= c->token_count ||
                        c->tokens[c->token_pos].type != TK_IDENT) {
                        while (c->token_pos < c->token_count &&
                               c->tokens[c->token_pos].type != TK_SEMI) c->token_pos++;
                        if (c->token_pos < c->token_count) c->token_pos++;
                        break;
                    }
                    char cname[128];
                    snprintf(cname, sizeof(cname), "%s", c->tokens[c->token_pos].str_val);
                    c->token_pos++;
                    /* Array suffixes on the comma declarator: take the
                     * first declarator's dimensions (the common case is
                     * no arrays at all). */
                    while (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LBRACKET) {
                        c->token_pos++;
                        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type != TK_RBRACKET) {
                            while (c->token_pos < c->token_count && c->tokens[c->token_pos].type != TK_RBRACKET) c->token_pos++;
                        }
                        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RBRACKET) c->token_pos++;
                    }
                    bool c_ptr = (cptr > 0) || dcl_is_fnptr;
                    size_t c_align = c_ptr ? 8 : effective_align;
                    size_t c_size = c_ptr ? 8 : base_size;
                    if (!c_ptr && is_array) c_size *= elem_count;
                    if (is_union) {
                        if (struct_member_room(c, &st)) {
                            ZeroCCStructMember *m = &st.members[st.member_count++];
                            snprintf(m->name, sizeof(m->name), "%s", cname);
                            m->offset = 0;
                            m->size = c_size;
                            m->align = c_align;
                            m->is_flexible = false;
                            m->is_bitfield = false;
                            m->is_signed = true;
                            m->tag = c_ptr ? TYPE_PTR : (is_enum ? TYPE_ENUM : (inner_is_union ? TYPE_UNION : TYPE_STRUCT));
                            m->base = zero_cc_type_alloc_base(&inner_type);
                            if (m->size > max_size) max_size = m->size;
                        }
                    } else {
                        current_offset = (current_offset + c_align - 1) & ~(c_align - 1);
                        if (struct_member_room(c, &st)) {
                            ZeroCCStructMember *m = &st.members[st.member_count++];
                            snprintf(m->name, sizeof(m->name), "%s", cname);
                            m->offset = current_offset;
                            m->size = c_size;
                            m->align = c_align;
                            m->is_flexible = false;
                            m->is_bitfield = false;
                            m->tag = c_ptr ? TYPE_PTR : (is_enum ? TYPE_ENUM : (inner_is_union ? TYPE_UNION : TYPE_STRUCT));
                            m->base = zero_cc_type_alloc_base(&inner_type);
                            if (!c_ptr && is_array) m->tag = TYPE_ARRAY;
                            current_offset += c_size;
                        }
                    }
                }
                if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
            }
            continue;
            }
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
        /* C99 6.7.2.3 prefix complex member: `_Complex double a;` /
         * `__complex__ float f;` - the specifier only counts when a type
         * follows (an identifier member merely named `_Complex` is valid). */
        bool member_is_complex = false;
        if (c->tokens[c->token_pos].type == TK_IDENT &&
            c->token_pos + 1 < c->token_count &&
            zero_cc_tok_is_complex_spec(c->tokens[c->token_pos].str_val) &&
            zero_cc_tok_is_type_start(c, c->token_pos + 1)) {
            c->token_pos++;
            member_is_complex = true;
        }
        mt = c->tokens[c->token_pos].type;

        if (mt == TK_IDENT || mt == TK_INT || mt == TK_CHAR || mt == TK_VOID ||
            mt == TK_FLOAT_KW || mt == TK_DOUBLE_KW || mt == TK_BOOL_KW ||
            mt == TK_SIGNED || mt == TK_UNSIGNED || mt == TK_SHORT || mt == TK_LONG || mt == TK_ATOMIC ||
            mt == TK_TYPEOF || mt == TK_TYPEOF_UNQUAL) {
            bool is_unsigned = (mt == TK_UNSIGNED || mt == TK_BOOL_KW);
            bool member_is_long_double = false;
            bool member_is_float128 = false;
            bool member_is_func_ptr = false;
            int member_int128_kind = 0; /* zc_name_int128_kind(): 1 signed, 2 __uint128_t */
            char type_ident[128] = "";
            if (mt == TK_IDENT) {
                snprintf(type_ident, sizeof(type_ident), "%s", c->tokens[c->token_pos].str_val);
                /* GNU __float128 member (Sprint 112). */
                if (strcmp(type_ident, "__float128") == 0) member_is_float128 = true;
                /* GNU __int128 / __uint128_t member (Sprint 113): 16-byte
                 * scalar; __uint128_t is always unsigned. */
                member_int128_kind = zc_name_int128_kind(type_ident);
                if (member_int128_kind == 2) is_unsigned = true;
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
            /* C99 6.7.2.3 suffix complex member: `double _Complex a;` /
             * `float __complex__ f;` - the specifier directly follows the
             * base type name. */
            if (!member_is_complex && c->token_pos < c->token_count &&
                c->tokens[c->token_pos].type == TK_IDENT &&
                zero_cc_tok_is_complex_spec(c->tokens[c->token_pos].str_val)) {
                c->token_pos++;
                member_is_complex = true;
            }
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
                else if (c->tokens[c->token_pos].type == TK_IDENT &&
                         zc_name_int128_kind(c->tokens[c->token_pos].str_val)) {
                    /* `unsigned __int128` / `signed __int128` member. */
                    snprintf(type_ident, sizeof(type_ident), "%s", c->tokens[c->token_pos].str_val);
                    member_int128_kind = zc_name_int128_kind(c->tokens[c->token_pos].str_val);
                    if (member_int128_kind == 2) is_unsigned = true;
                    mt = TK_IDENT;
                    c->token_pos++;
                }
                else { mt = TK_INT; }
            } else if (mt == TK_LONG) {
                if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LONG) c->token_pos++;
                if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_DOUBLE_KW) {
                    /* `long double` member: 16-byte object, value in low 8B
                     * (c-torture execute/ieee/copysign1/2). */
                    member_is_long_double = true;
                    c->token_pos++;
                }
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
            /* GNU aligned(N) after the base type: `int __attribute__((aligned(8))) a;`
             * (preprocessor marker __zc_aligned__(N); c-torture execute/pr23467). */
            {
                size_t probe = 0;
                while (zero_cc_parse_aligned_attr(c, &c->token_pos, &probe)) {
                    if (probe > member_align) member_align = probe;
                }
                zero_cc_skip_mode_attr(c);
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
            else if (member_is_long_double) { base_align = 16; base_size = 16; }
            else if (mt == TK_LONG || mt == TK_DOUBLE_KW || mt == TK_VOID) { base_align = 8; base_size = 8; }
            else if (mt == TK_IDENT) {
                if (member_is_float128) { base_align = 16; base_size = 16; }
                if (member_int128_kind) { base_align = 16; base_size = 16; }
                ZeroCCType *td = zero_cc_find_typedef(c, type_ident);
                if (!td) td = zero_cc_find_struct(c, type_ident);
                if (td) {
                    base_align = td->align > 0 ? td->align : 8;
                    base_size = td->size;
                }
            }
            /* C99 6.7.2.3: _Complex T is a two-part object of element type T
             * (GCC: sizeof(_Complex double) == 16, align 8; float: 8, align 4).
             * Non-pointer scalar float members only - the pointer/array
             * forms keep the pointer or array of complex element. */
            bool member_cx_scalar = member_is_complex && !is_ptr &&
                (mt == TK_DOUBLE_KW || mt == TK_FLOAT_KW);
            if (is_ptr) { base_align = 8; base_size = 8; }
            if (member_cx_scalar) { base_size *= 2; }
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
                member_is_func_ptr = true;
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
            /* GNU variably-modified member (`char b[argc];`): the dimension
             * is a runtime expression parsed into the TU arena. At most one
             * runtime dimension per member. */
            bool member_vla = false;
            size_t member_vla_expr = 0;
            int member_vla_count = 0;
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
                } else if (zc_typedef_dim_is_const(c, c->token_pos)) {
                    int64_t cd = zero_cc_eval_const_expr(c);
                    if (cd >= 0) { dim = (size_t)cd; if (cd == 0) is_flexible = true; }
                } else if (is_union) {
                    diag_report_error_at_token(c,
                        "union member with variably-modified type is not supported");
                    member_vla_count = 2; /* suppress later handling */
                    c->token_pos++;
                    dim = 1;
                } else {
                    member_vla = true;
                    member_vla_count++;
                    if (member_vla_count == 1)
                        member_vla_expr = parse_expr_ast_internal(c, (ZeroCCASTArena *)c->tu_ast_arena);
                    else
                        diag_report_error_at_token(c,
                            "multiple variable dimensions in a struct member are not supported");
                    dim = 2; /* GCC placeholder for the static layout */
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
                zero_cc_skip_mode_attr(c);
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
                    m->is_complex = member_cx_scalar;
                    m->is_func_ptr = member_is_func_ptr;
                    if (is_bitfield) {
                        zero_cc_setup_bitfield_type(c, m, mt, type_ident, is_unsigned);
                    } else if (is_typeof) {
                        m->tag = typeof_type.tag;
                        if (typeof_type.base) m->base = zero_cc_type_alloc_base(typeof_type.base);
                    }
                    else if (mt == TK_CHAR || mt == TK_BOOL_KW) m->tag = is_unsigned ? TYPE_UINT8 : TYPE_INT8;
                    else if (mt == TK_SHORT) m->tag = is_unsigned ? TYPE_UINT16 : TYPE_INT16;
                    else if (mt == TK_FLOAT_KW) m->tag = TYPE_FLOAT;
                    else if (mt == TK_INT) m->tag = is_unsigned ? TYPE_UINT32 : TYPE_INT32;
                    else if (mt == TK_DOUBLE_KW) m->tag = TYPE_DOUBLE;
                    else if (mt == TK_LONG || mt == TK_VOID) m->tag = (mt == TK_VOID) ? TYPE_INT64 : (member_is_long_double ? TYPE_DOUBLE : (is_unsigned ? TYPE_UINT64 : TYPE_INT64));
                    else if (mt == TK_IDENT && member_is_float128) { m->tag = TYPE_DOUBLE; m->is_float128 = true; }
                    else if (mt == TK_IDENT && member_int128_kind) {
                        m->tag = (member_int128_kind == 2 || is_unsigned) ? TYPE_UINT128 : TYPE_INT128;
                    }
                    else if (mt == TK_IDENT) {
                        ZeroCCType *td = zero_cc_find_typedef(c, type_ident);
                        if (!td) td = zero_cc_find_struct(c, type_ident);
                        if (td && td->tag == TYPE_UNION) m->tag = TYPE_UNION;
                        else m->tag = TYPE_STRUCT;
                        /* By-value member: base = the struct itself. Pointer
                         * member (`t *p`): base = the pointee (C5) so the
                         * named struct's fields are available. */
                        if (td) {
                            {
                            ZeroCCType pt = *td;
                            /* Leading qualifier attaches to the base type:
                             * `const T *p` = pointer to const T (c-torture
                             * 20040708-1, 961004-1). */
                            if (is_ptr) {
                                if (member_is_const) pt.is_const = true;
                                if (member_is_volatile) pt.is_volatile = true;
                            } else {
                                m->is_const = member_is_const;
                                m->is_volatile = member_is_volatile;
                            }
                            m->base = zero_cc_type_alloc_base(&pt);
                        }
                        }
                    }
                    else m->tag = TYPE_INT32;
                    if (is_ptr) {
                        if (!m->base) {
                            ZeroCCType dummy;
                            memset(&dummy, 0, sizeof(dummy));
                            dummy.tag = (mt == TK_VOID) ? TYPE_VOID : m->tag;
                            /* `void *` (depth 1) has a VOID pointee too -
                             * typing it int64 made `&g.pv` incompatible
                             * with `void **` (c-torture pr42730/pta-1). */
                            dummy.size = base_size;
                            dummy.align = base_align;
                            dummy.is_const = member_is_const;
                            dummy.is_volatile = member_is_volatile;
                            if (member_is_complex &&
                                (mt == TK_DOUBLE_KW || mt == TK_FLOAT_KW)) {
                                dummy.size = (mt == TK_DOUBLE_KW) ? 16 : 8;
                                dummy.is_complex = true;
                            }
                            m->base = zero_cc_type_alloc_base(&dummy);
                        }
                        m->tag = TYPE_PTR;
                        if (m->is_func_ptr) {
                            /* Function-pointer member: base = the function's
                             * RETURN type (declarator stars belong to it). */
                            zero_cc_member_wrap_ptr_levels(m, ptr_depth);
                        } else {
                            zero_cc_member_wrap_ptr_levels(m, ptr_depth - 1);
                        }
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
                        if (in_bitfield && base_size <= bitfield_storage_size &&
                            (bitfield_bit_offset + bit_width <= bitfield_storage_size * 8)) {
                            /* Fits in current storage unit (a narrower base
                             * type packs into the wider unit - GCC layout); the
                             * member size is the unit size. */
                            size_t m_offset = bitfield_storage_offset;
                            uint8_t m_bit_off = (uint8_t)bitfield_bit_offset;
                            bitfield_bit_offset += bit_width;
                            if (mname[0] != '\0' && struct_member_room(c, &st)) {
                                ZeroCCStructMember *m = &st.members[st.member_count++];
                                snprintf(m->name, sizeof(m->name), "%s", mname);
                                m->offset = m_offset;
                                m->size = bitfield_storage_size;
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
                                } else {
                                    zero_cc_setup_bitfield_type(c, m, mt, type_ident, is_unsigned);
                                }
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
                                } else {
                                    zero_cc_setup_bitfield_type(c, m, mt, type_ident, is_unsigned);
                                }
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
                        m->size = is_flexible ? 0 : (member_vla ? 0 : (base_size * elem_count));
                        m->align = effective_align;
                        m->is_flexible = is_flexible;
                        m->is_bitfield = false;
                        m->bit_offset = 0;
                        m->bit_width = 0;
                        if (member_vla) {
                            m->is_vla = true;
                            m->vla_dim = member_vla_expr;
                            m->vla_elem_size = base_size * (elem_count / 2);
                        }
                        m->is_signed = !is_unsigned;
                        m->is_const = is_ptr ? member_ptr_is_const : member_is_const;
                        m->is_volatile = is_ptr ? member_ptr_is_volatile : member_is_volatile;
                        m->is_atomic = member_is_atomic;
                        m->is_complex = member_cx_scalar;
                        m->is_func_ptr = member_is_func_ptr;
                        if (is_typeof) {
                            m->tag = typeof_type.tag;
                            if (typeof_type.base) m->base = zero_cc_type_alloc_base(typeof_type.base);
                        }
                        else if (mt == TK_CHAR || mt == TK_BOOL_KW) m->tag = is_unsigned ? TYPE_UINT8 : TYPE_INT8;
                        else if (mt == TK_SHORT) m->tag = is_unsigned ? TYPE_UINT16 : TYPE_INT16;
                        else if (mt == TK_FLOAT_KW) m->tag = TYPE_FLOAT;
                        else if (mt == TK_INT) m->tag = is_unsigned ? TYPE_UINT32 : TYPE_INT32;
                        else if (mt == TK_DOUBLE_KW) m->tag = TYPE_DOUBLE;
                        else if (mt == TK_LONG || mt == TK_VOID) m->tag = (mt == TK_VOID) ? TYPE_INT64 : (member_is_long_double ? TYPE_DOUBLE : (is_unsigned ? TYPE_UINT64 : TYPE_INT64));
                        else if (mt == TK_IDENT && member_is_float128) { m->tag = TYPE_DOUBLE; m->is_float128 = true; }
                        else if (mt == TK_IDENT && member_int128_kind) {
                            m->tag = (member_int128_kind == 2 || is_unsigned) ? TYPE_UINT128 : TYPE_INT128;
                        }
                        else if (mt == TK_IDENT) {
                            ZeroCCType *td = zero_cc_find_typedef(c, type_ident);
                            if (!td) td = zero_cc_find_struct(c, type_ident);
                            if (td && td->tag == TYPE_UNION) m->tag = TYPE_UNION;
                            else m->tag = TYPE_STRUCT;
                            /* By-value member: base = the struct itself. Pointer
                             * member (`t *p`): base = the pointee (C5). */
                            if (td) {
                                {
                            ZeroCCType pt = *td;
                            /* Leading qualifier attaches to the base type:
                             * `const T *p` = pointer to const T (c-torture
                             * 20040708-1, 961004-1). */
                            if (is_ptr) {
                                if (member_is_const) pt.is_const = true;
                                if (member_is_volatile) pt.is_volatile = true;
                            } else {
                                m->is_const = member_is_const;
                                m->is_volatile = member_is_volatile;
                            }
                            m->base = zero_cc_type_alloc_base(&pt);
                        }
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
                                dummy.tag = (mt == TK_VOID) ? TYPE_VOID : m->tag;
                            /* `void *` (depth 1) has a VOID pointee too -
                             * typing it int64 made `&g.pv` incompatible
                             * with `void **` (c-torture pr42730/pta-1). */
                                dummy.size = base_size;
                                dummy.align = base_align;
                                dummy.is_const = member_is_const;
                                dummy.is_volatile = member_is_volatile;
                                if (member_is_complex &&
                                    (mt == TK_DOUBLE_KW || mt == TK_FLOAT_KW)) {
                                    dummy.size = (mt == TK_DOUBLE_KW) ? 16 : 8;
                                    dummy.is_complex = true;
                                }
                                m->base = zero_cc_type_alloc_base(&dummy);
                            }
                            m->tag = TYPE_PTR;
                            if (m->is_func_ptr) {
                                /* Function-pointer member: base = the
                                 * function's RETURN type (see struct branch). */
                                zero_cc_member_wrap_ptr_levels(m, ptr_depth);
                            } else {
                                zero_cc_member_wrap_ptr_levels(m, ptr_depth - 1);
                            }
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
                            current_offset += m->size + (member_vla ? 2 * m->vla_elem_size : 0);
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
                /* GNU variably-modified member (see the paths above). */
                bool c_member_vla = false;
                size_t c_member_vla_expr = 0;
                int c_member_vla_count = 0;
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
                    } else if (zc_typedef_dim_is_const(c, c->token_pos)) {
                        int64_t cd = zero_cc_eval_const_expr(c);
                        if (cd >= 0) { dim = (size_t)cd; if (cd == 0) c_is_flexible = true; }
                    } else if (is_union) {
                        diag_report_error_at_token(c,
                            "union member with variably-modified type is not supported");
                        c_member_vla_count = 2; /* suppress later handling */
                        c->token_pos++;
                        dim = 1;
                    } else {
                        c_member_vla = true;
                        c_member_vla_count++;
                        if (c_member_vla_count == 1)
                            c_member_vla_expr = parse_expr_ast_internal(c, (ZeroCCASTArena *)c->tu_ast_arena);
                        else
                            diag_report_error_at_token(c,
                                "multiple variable dimensions in a struct member are not supported");
                        dim = 2; /* GCC placeholder for the static layout */
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
                        m->is_complex = member_cx_scalar;
                        m->is_func_ptr = member_is_func_ptr;
                        if (is_typeof) {
                            m->tag = typeof_type.tag;
                            if (typeof_type.base) m->base = zero_cc_type_alloc_base(typeof_type.base);
                        }
                        else if (mt == TK_CHAR || mt == TK_BOOL_KW) m->tag = is_unsigned ? TYPE_UINT8 : TYPE_INT8;
                        else if (mt == TK_SHORT) m->tag = is_unsigned ? TYPE_UINT16 : TYPE_INT16;
                        else if (mt == TK_FLOAT_KW) m->tag = TYPE_FLOAT;
                        else if (mt == TK_INT) m->tag = is_unsigned ? TYPE_UINT32 : TYPE_INT32;
                        else if (mt == TK_DOUBLE_KW) m->tag = TYPE_DOUBLE;
                        else if (mt == TK_LONG || mt == TK_VOID) m->tag = (mt == TK_VOID) ? TYPE_INT64 : (member_is_long_double ? TYPE_DOUBLE : (is_unsigned ? TYPE_UINT64 : TYPE_INT64));
                        else if (mt == TK_IDENT && member_is_float128) { m->tag = TYPE_DOUBLE; m->is_float128 = true; }
                        else if (mt == TK_IDENT) {
                            ZeroCCType *td = zero_cc_find_typedef(c, type_ident);
                            if (!td) td = zero_cc_find_struct(c, type_ident);
                            if (td && td->tag == TYPE_UNION) m->tag = TYPE_UNION;
                            else m->tag = TYPE_STRUCT;
                            if (td) {
                                {
                            ZeroCCType pt = *td;
                            /* Leading qualifier attaches to the base type:
                             * `const T *p` = pointer to const T (c-torture
                             * 20040708-1, 961004-1).  Keyed off c_is_ptr:
                             * in a comma list the CURRENT declarator's
                             * pointer-ness decides (not the first one's). */
                            if (c_is_ptr) {
                                if (member_is_const) pt.is_const = true;
                                if (member_is_volatile) pt.is_volatile = true;
                            } else {
                                m->is_const = member_is_const;
                                m->is_volatile = member_is_volatile;
                            }
                            m->base = zero_cc_type_alloc_base(&pt);
                        }
                            }
                        }
                        else m->tag = TYPE_INT32;
                        if (c_is_ptr) {
                            /* Only synthesize a dummy pointee when the type
                             * wasn't a named struct/typedef (m->base already
                             * holds the real pointee then - clobbering it
                             * with an anonymous struct broke `A *e, *f;`
                             * comma lists, c-torture pr113623). */
                            if (!m->base) {
                                ZeroCCType dummy;
                                memset(&dummy, 0, sizeof(dummy));
                                dummy.tag = (mt == TK_VOID) ? TYPE_VOID : m->tag;
                                dummy.size = base_size;
                                dummy.align = base_align;
                                dummy.is_const = member_is_const;
                                dummy.is_volatile = member_is_volatile;
                                m->base = zero_cc_type_alloc_base(&dummy);
                            }
                            m->tag = TYPE_PTR;
                            if (m->is_func_ptr) {
                                /* Function-pointer member: the member's base
                                 * is the function's RETURN type, and the
                                 * declarator's star run belongs to that
                                 * return (`int *(*f)(long)` returns `int *`;
                                 * c-torture 20010102-1).  Wrap the base with
                                 * all c_ptr_depth stars - the member's own
                                 * PTR tag is the funcptr layer itself. */
                                zero_cc_member_wrap_ptr_levels(m, c_ptr_depth);
                            } else {
                                zero_cc_member_wrap_ptr_levels(m, c_ptr_depth - 1);
                            }
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
                    } else if (in_bitfield && c_base_size <= bitfield_storage_size &&
                               (bitfield_bit_offset + c_bit_width <= bitfield_storage_size * 8)) {
                        size_t m_offset = bitfield_storage_offset;
                        uint8_t m_bit_off = (uint8_t)bitfield_bit_offset;
                        bitfield_bit_offset += c_bit_width;
                        if (c_mname[0] != '\0' && struct_member_room(c, &st)) {
                            ZeroCCStructMember *m = &st.members[st.member_count++];
                            snprintf(m->name, sizeof(m->name), "%s", c_mname);
                            m->offset = m_offset;
                            m->size = bitfield_storage_size;
                            m->align = c_effective_align;
                            m->is_flexible = false;
                            m->is_bitfield = true;
                            m->bit_offset = m_bit_off;
                            m->bit_width = c_bit_width;
                            zero_cc_setup_bitfield_type(c, m, mt, type_ident, is_unsigned);
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
                            zero_cc_setup_bitfield_type(c, m, mt, type_ident, is_unsigned);
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
                        m->size = c_is_flexible ? 0 : (c_member_vla ? 0 : (c_base_size * c_elem_count));
                        m->align = c_effective_align;
                        m->is_flexible = c_is_flexible;
                        m->is_bitfield = false;
                        m->bit_offset = 0;
                        m->bit_width = 0;
                        if (c_member_vla) {
                            m->is_vla = true;
                            m->vla_dim = c_member_vla_expr;
                            m->vla_elem_size = c_base_size * (c_elem_count / 2);
                        }
                        m->is_signed = !is_unsigned;
                        m->is_const = c_is_ptr ? c_member_ptr_is_const : member_is_const;
                        m->is_volatile = c_is_ptr ? c_member_ptr_is_volatile : member_is_volatile;
                        m->is_atomic = member_is_atomic;
                        m->is_complex = member_cx_scalar;
                        m->is_func_ptr = member_is_func_ptr;
                        if (is_typeof) {
                            m->tag = typeof_type.tag;
                            if (typeof_type.base) m->base = zero_cc_type_alloc_base(typeof_type.base);
                        }
                        else if (mt == TK_CHAR || mt == TK_BOOL_KW) m->tag = is_unsigned ? TYPE_UINT8 : TYPE_INT8;
                        else if (mt == TK_SHORT) m->tag = is_unsigned ? TYPE_UINT16 : TYPE_INT16;
                        else if (mt == TK_FLOAT_KW) m->tag = TYPE_FLOAT;
                        else if (mt == TK_INT) m->tag = is_unsigned ? TYPE_UINT32 : TYPE_INT32;
                        else if (mt == TK_DOUBLE_KW) m->tag = TYPE_DOUBLE;
                        else if (mt == TK_LONG || mt == TK_VOID) m->tag = (mt == TK_VOID) ? TYPE_INT64 : (member_is_long_double ? TYPE_DOUBLE : (is_unsigned ? TYPE_UINT64 : TYPE_INT64));
                        else if (mt == TK_IDENT && member_is_float128) { m->tag = TYPE_DOUBLE; m->is_float128 = true; }
                        else if (mt == TK_IDENT) {
                            ZeroCCType *td = zero_cc_find_typedef(c, type_ident);
                            if (!td) td = zero_cc_find_struct(c, type_ident);
                            if (td && td->tag == TYPE_UNION) m->tag = TYPE_UNION;
                            else m->tag = TYPE_STRUCT;
                            if (td) {
                                {
                            ZeroCCType pt = *td;
                            /* Leading qualifier attaches to the base type:
                             * `const T *p` = pointer to const T (c-torture
                             * 20040708-1, 961004-1).  Keyed off c_is_ptr:
                             * in a comma list the CURRENT declarator's
                             * pointer-ness decides (not the first one's). */
                            if (c_is_ptr) {
                                if (member_is_const) pt.is_const = true;
                                if (member_is_volatile) pt.is_volatile = true;
                            } else {
                                m->is_const = member_is_const;
                                m->is_volatile = member_is_volatile;
                            }
                            m->base = zero_cc_type_alloc_base(&pt);
                        }
                            }
                        }
                        else m->tag = TYPE_INT32;
                        if (c_is_ptr) {
                            /* Only synthesize a dummy pointee when the type
                             * wasn't a named struct/typedef (m->base already
                             * holds the real pointee then - clobbering it
                             * with an anonymous struct broke `A *e, *f;`
                             * comma lists, c-torture pr113623). */
                            if (!m->base) {
                                ZeroCCType dummy;
                                memset(&dummy, 0, sizeof(dummy));
                                dummy.tag = (mt == TK_VOID) ? TYPE_VOID : m->tag;
                                dummy.size = base_size;
                                dummy.align = base_align;
                                dummy.is_const = member_is_const;
                                dummy.is_volatile = member_is_volatile;
                                m->base = zero_cc_type_alloc_base(&dummy);
                            }
                            m->tag = TYPE_PTR;
                            if (m->is_func_ptr) {
                                /* Function-pointer member: base = the
                                 * function's RETURN type (see the union
                                 * branch above). */
                                zero_cc_member_wrap_ptr_levels(m, c_ptr_depth);
                            } else {
                                zero_cc_member_wrap_ptr_levels(m, c_ptr_depth - 1);
                            }
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
                            current_offset += m->size + (c_member_vla ? 2 * m->vla_elem_size : 0);
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
    /* GNU variably-modified struct: mark it so the TypeInfo conversion,
     * sizeof, allocation, and the by-value (ptr, size) parameter ABI can
     * find the VLA members. */
    if (!is_union) {
        st.is_vla_struct = false;
        for (size_t mi = 0; mi < st.member_count; mi++) {
            if (st.members[mi].is_vla) { st.is_vla_struct = true; break; }
        }
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


