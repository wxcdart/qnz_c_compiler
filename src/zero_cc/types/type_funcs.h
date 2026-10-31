#ifndef ZERO_CC_TYPES_TYPE_FUNCS_H
#define ZERO_CC_TYPES_TYPE_FUNCS_H

/* Base Types */
static inline ZeroCCType zero_cc_type_void(void) {
    ZeroCCType t; memset(&t, 0, sizeof(t));
    t.tag = TYPE_VOID; t.size = 0; t.align = 1;
    return t;
}
static inline ZeroCCType zero_cc_type_bool(void) {
    ZeroCCType t; memset(&t, 0, sizeof(t));
    t.tag = TYPE_BOOL; t.size = 1; t.align = 1;
    return t;
}
static inline ZeroCCType zero_cc_type_char(void) {
    ZeroCCType t; memset(&t, 0, sizeof(t));
    t.tag = TYPE_INT8; t.size = 1; t.align = 1;
    return t;
}
static inline ZeroCCType zero_cc_type_uint8(void) {
    ZeroCCType t; memset(&t, 0, sizeof(t));
    t.tag = TYPE_UINT8; t.size = 1; t.align = 1;
    return t;
}
static inline ZeroCCType zero_cc_type_int16(void) {
    ZeroCCType t; memset(&t, 0, sizeof(t));
    t.tag = TYPE_INT16; t.size = 2; t.align = 2;
    return t;
}
static inline ZeroCCType zero_cc_type_uint16(void) {
    ZeroCCType t; memset(&t, 0, sizeof(t));
    t.tag = TYPE_UINT16; t.size = 2; t.align = 2;
    return t;
}
static inline ZeroCCType zero_cc_type_int(void) {
    ZeroCCType t; memset(&t, 0, sizeof(t));
    t.tag = TYPE_INT32; t.size = 4; t.align = 4;
    return t;
}
static inline ZeroCCType zero_cc_type_uint32(void) {
    ZeroCCType t; memset(&t, 0, sizeof(t));
    t.tag = TYPE_UINT32; t.size = 4; t.align = 4;
    return t;
}
static inline ZeroCCType zero_cc_type_int64(void) {
    ZeroCCType t; memset(&t, 0, sizeof(t));
    t.tag = TYPE_INT64; t.size = 8; t.align = 8;
    return t;
}
static inline ZeroCCType zero_cc_type_uint64(void) {
    ZeroCCType t; memset(&t, 0, sizeof(t));
    t.tag = TYPE_UINT64; t.size = 8; t.align = 8;
    return t;
}
static inline ZeroCCType zero_cc_type_float(void) {
    ZeroCCType t; memset(&t, 0, sizeof(t));
    t.tag = TYPE_FLOAT; t.size = 4; t.align = 4;
    return t;
}
static inline ZeroCCType zero_cc_type_double(void) {
    ZeroCCType t; memset(&t, 0, sizeof(t));
    t.tag = TYPE_DOUBLE; t.size = 8; t.align = 8;
    return t;
}
/* C99 long double: the x86-64 SysV object is 16 bytes (80-bit extended
 * precision, 16-byte alignment); the compiler's value model lowers it
 * through the 64-bit double path. */
static inline ZeroCCType zero_cc_type_long_double(void) {
    ZeroCCType t = zero_cc_type_double();
    t.size = 16; t.align = 16; t.is_long_double = true;
    return t;
}
static inline ZeroCCType zero_cc_type_nullptr(void) {
    ZeroCCType t; memset(&t, 0, sizeof(t));
    t.tag = TYPE_NULLPTR; t.size = 8; t.align = 8;
    return t;
}
static inline ZeroCCType zero_cc_type_enum(const char *name) {
    ZeroCCType t; memset(&t, 0, sizeof(t));
    t.tag = TYPE_ENUM; t.size = 4; t.align = 4;
    if (name) snprintf(t.struct_name, sizeof(t.struct_name), "%s", name);
    return t;
}
static inline ZeroCCType zero_cc_type_user(int user_idx, size_t size, size_t align) {
    ZeroCCType t; memset(&t, 0, sizeof(t));
    t.tag = (ZeroTypeTag)(TYPE_USER_0 + (user_idx & 3));
    t.size = size ? size : 16;
    t.align = align ? align : 16;
    return t;
}

typedef struct ZeroCCTypeNode {
    struct ZeroCCTypeNode *next;
    ZeroCCType type;
} ZeroCCTypeNode;

void zero_cc_type_pool_free(void);

/* Free every heap struct/union member array owned by `c` (registration
 * tables) plus the arrays held by the live type-pool nodes (nested
 * anonymous structs). Call at the end of a compilation, while the pool is
 * still alive — after zero_cc_type_pool_free those nested arrays are
 * unreachable. Distinct arrays are freed exactly once (aliases share one).
 * Implemented in parser.c (needs the pool list). */
void zero_cc_free_all_struct_arrays(ZeroCCCompiler *c);
ZeroCCType *zero_cc_type_alloc_base(const ZeroCCType *base);

static inline ZeroCCType zero_cc_type_ptr(ZeroCCType *base) {
    ZeroCCType t; memset(&t, 0, sizeof(t));
    t.tag = TYPE_PTR; t.size = 8; t.align = 8;
    if (base) {
        t.base = zero_cc_type_alloc_base(base);
    } else {
        t.base = NULL;
    }
    return t;
}

/* GNU vector type (`__attribute__((__vector_size__(N)))`). Reuses the
 * reserved TYPE_USER_0 tag with `base` pointing at the heap-allocated
 * element type and `array_len` holding the lane count (N / elem_size).
 * The type behaves like an opaque N-byte object for layout, but lowering
 * intercepts arithmetic, comparison, casts, and subscripting (see
 * semantic/promote.c and ir/ast_lowering_expr.c) so it keeps vector
 * semantics instead of decaying to a pointer like a plain array. */
static inline bool zero_cc_type_is_vector(const ZeroCCType *t) {
    return t && t->tag == TYPE_USER_0;
}
static inline ZeroCCType zero_cc_type_vector(const ZeroCCType *elem, size_t n_bytes) {
    ZeroCCType t; memset(&t, 0, sizeof(t));
    t.tag = TYPE_USER_0;
    t.size = n_bytes;
    t.align = (n_bytes >= 16) ? 16 : ((elem && elem->align) ? elem->align : 1);
    if (elem) {
        t.base = zero_cc_type_alloc_base(elem);
        t.array_len = (elem->size > 0) ? n_bytes / elem->size : 0;
    } else {
        t.base = NULL;
        t.array_len = 0;
    }
    return t;
}

/* C99 6.2.5: _Complex T - two contiguous objects of element type `elem`
 * (real, then imaginary).  Same tag as the element so float-ness
 * detection keeps working; is_complex marks the complex semantics. */
static inline bool zero_cc_type_is_complex(const ZeroCCType *t) {
    return t && t->is_complex;
}
/* The three C/GNU spellings of the complex type specifier. */
static inline bool zero_cc_tok_is_complex_spec(const char *s) {
    return s && (strcmp(s, "_Complex") == 0 ||
                 strcmp(s, "__complex__") == 0 ||
                 strcmp(s, "__complex") == 0);
}
static inline ZeroCCType zero_cc_type_complex(const ZeroCCType *elem) {
    ZeroCCType t;
    if (elem) {
        t = *elem;
        t.base = zero_cc_type_alloc_base(elem);
        t.size = elem->size * 2;
        t.align = elem->align > 0 ? elem->align : 8;
    } else {
        ZeroCCType d = zero_cc_type_double();
        t = d;
        t.base = zero_cc_type_alloc_base(&d);
        t.size = 16;
        t.align = 16;
    }
    t.is_complex = true;
    t.is_vla = false;
    t.is_func = false;
    t.is_func_ptr = false;
    t.is_atomic = false;
    return t;
}

static inline ZeroCCType zero_cc_type_array(ZeroCCType *base, ssize_t length) {
    ZeroCCType t;
    memset(&t, 0, sizeof(t));
    t.tag = TYPE_ARRAY;
    if (base) {
        t.base = zero_cc_type_alloc_base(base);
        if (length > 0) {
            t.size = base->size * length;
        } else {
            t.size = 0;
        }
        t.align = base->align;
    } else {
        t.base = NULL;
        t.size = 0;
        t.align = 8;
    }
    t.array_len = length;
    return t;
}

static inline ZeroCCType zero_cc_type_strip_qualifiers(ZeroCCType ty) {
    ty.is_const = false;
    ty.is_volatile = false;
    ty.is_atomic = false;
    return ty;
}

/* ---------------------------------------------------------------------------
 * Aggregate scalar-leaf walking (shared by the brace-initializer parser in
 * parser/ast_expr.h and the compound-literal lowering in ir/ast_lowering_expr.c).
 * A "scalar leaf" is the smallest initializable unit in C layout order: a
 * scalar member, one element of an array member, or (recursively) the leaves
 * of a nested struct/union member.  Padding is not a leaf.
 * ------------------------------------------------------------------------- */
static inline bool zc_struct_resolve_scalar_leaf_bf(const ZeroCCType *st, size_t target_idx, size_t *cur_idx,
                                                     size_t base_off, size_t *out_off, size_t *out_sz,
                                                     bool *out_is_bf, uint8_t *out_boff, uint8_t *out_bw,
                                                     const char **out_flex_name, bool *out_is_float) {
    if (out_is_float) *out_is_float = false;
    if (!st || !st->members) return false;
    for (size_t i = 0; i < st->member_count; i++) {
        const ZeroCCStructMember *m = &st->members[i];
        if (m->is_flexible) {
            if (*cur_idx == target_idx) {
                if (out_flex_name) *out_flex_name = m->name;
                return false;
            }
            continue;
        }
        if (m->tag == TYPE_ARRAY && m->base && m->base->size > 0) {
            size_t count = m->size / m->base->size;
            if (target_idx < *cur_idx + count) {
                size_t a_idx = target_idx - *cur_idx;
                *out_off = base_off + m->offset + a_idx * m->base->size;
                *out_sz = m->base->size;
                if (out_is_bf) *out_is_bf = false;
                if (out_is_float) *out_is_float = (m->base->tag == TYPE_FLOAT || m->base->tag == TYPE_DOUBLE);
                return true;
            }
            *cur_idx += count;
        } else if ((m->tag == TYPE_STRUCT || m->tag == TYPE_UNION) && m->base && m->base->member_count > 0) {
            if (zc_struct_resolve_scalar_leaf_bf(m->base, target_idx, cur_idx, base_off + m->offset,
                                                  out_off, out_sz, out_is_bf, out_boff, out_bw, out_flex_name, out_is_float)) {
                return true;
            }
            if (out_flex_name && *out_flex_name) return false;
        } else if (m->is_complex) {
            /* C99 6.7.9: complex member = two scalar leaves (re, im). */
            size_t esz = (m->size >= 2) ? m->size / 2 : 8;
            if (target_idx < *cur_idx + 2) {
                size_t half = target_idx - *cur_idx;
                *out_off = base_off + m->offset + half * esz;
                *out_sz = esz;
                if (out_is_bf) *out_is_bf = false;
                if (out_is_float) *out_is_float = true;
                return true;
            }
            *cur_idx += 2;
        } else {
            if (*cur_idx == target_idx) {
                *out_off = base_off + m->offset;
                *out_sz = m->size;
                if (out_is_bf) *out_is_bf = m->is_bitfield;
                if (out_boff) *out_boff = m->bit_offset;
                if (out_bw) *out_bw = m->bit_width;
                if (out_is_float) *out_is_float = (m->tag == TYPE_FLOAT || m->tag == TYPE_DOUBLE);
                return true;
            }
            (*cur_idx)++;
        }
    }
    return false;
}

static inline bool zc_struct_resolve_scalar_leaf(const ZeroCCType *st, size_t target_idx, size_t *cur_idx,
                                                  size_t base_off, size_t *out_off, size_t *out_sz,
                                                  const char **out_flex_name, bool *out_is_float) {
    return zc_struct_resolve_scalar_leaf_bf(st, target_idx, cur_idx, base_off, out_off, out_sz,
                                             NULL, NULL, NULL, out_flex_name, out_is_float);
}

static inline size_t zc_object_count_scalar_leaves(const ZeroCCType *t) {
    if (!t) return 0;
    if (zero_cc_type_is_complex(t)) {
        /* C99 6.7.9: a complex scalar has two scalar leaves (re, im). */
        return 2;
    }
    if (t->tag == TYPE_ARRAY) {
        if (!t->base || t->array_len == 0) return 0;
        return t->array_len * zc_object_count_scalar_leaves(t->base);
    }
    if (t->tag == TYPE_USER_0 && zero_cc_type_is_vector(t)) {
        /* GNU vector: N scalar lanes (element in base, count in array_len) */
        if (!t->base || t->array_len == 0) return 0;
        return t->array_len * zc_object_count_scalar_leaves(t->base);
    }
    if (t->tag == TYPE_STRUCT || t->tag == TYPE_UNION) {
        if (!t->members) return 0;
        size_t n = 0;
        for (size_t i = 0; i < t->member_count; i++) {
            const ZeroCCStructMember *m = &t->members[i];
            if (m->is_flexible) continue;
            if (m->tag == TYPE_ARRAY && m->base && m->base->size > 0) {
                n += (m->size / m->base->size) * zc_object_count_scalar_leaves(m->base);
            } else if (m->tag == TYPE_STRUCT || m->tag == TYPE_UNION) {
                n += m->base ? zc_object_count_scalar_leaves(m->base) : 1;
            } else {
                n += m->is_complex ? 2 : 1;
            }
        }
        return n;
    }
    return 1;
}

static inline bool zc_object_resolve_scalar_leaf_bf(const ZeroCCType *t, size_t target_idx,
                                                     size_t *out_off, size_t *out_sz,
                                                     bool *out_is_bf, uint8_t *out_boff, uint8_t *out_bw,
                                                     bool *out_is_float) {
    if (out_is_bf) *out_is_bf = false;
    if (out_boff) *out_boff = 0;
    if (out_bw) *out_bw = 0;
    if (out_is_float) *out_is_float = false;
    if (!t) return false;
    if (t->tag == TYPE_USER_0 && zero_cc_type_is_vector(t)) {
        /* GNU vector: lane i lives at i * elem_size */
        const ZeroCCType *el = t->base;
        if (!el || el->size == 0 || t->array_len == 0) return false;
        if (target_idx >= t->array_len) return false;
        *out_off = target_idx * el->size;
        *out_sz = el->size;
        if (out_is_float) *out_is_float = (el->tag == TYPE_FLOAT || el->tag == TYPE_DOUBLE);
        return true;
    }
    if (t->tag == TYPE_ARRAY) {
        const ZeroCCType *el = t->base;
        if (!el || el->size == 0 || t->array_len == 0) return false;
        if (el->tag == TYPE_ARRAY || el->tag == TYPE_STRUCT || el->tag == TYPE_UNION) {
            size_t per = zc_object_count_scalar_leaves(el);
            if (per == 0 || target_idx >= t->array_len * per) return false;
            size_t ei = target_idx / per;
            size_t wi = target_idx % per;
            size_t sub_off, sub_sz;
            if (el->tag == TYPE_ARRAY) {
                if (!zc_object_resolve_scalar_leaf_bf(el, wi, &sub_off, &sub_sz, out_is_bf, out_boff, out_bw, out_is_float)) return false;
            } else {
                size_t ci = 0;
                const char *flex = NULL;
                if (!zc_struct_resolve_scalar_leaf_bf(el, wi, &ci, 0, &sub_off, &sub_sz, out_is_bf, out_boff, out_bw, &flex, out_is_float))
                    return false;
            }
            *out_off = ei * el->size + sub_off;
            *out_sz = sub_sz;
            return true;
        }
        /* scalar elements */
        if (target_idx >= t->array_len) return false;
        *out_off = target_idx * el->size;
        *out_sz = el->size;
        if (out_is_float) *out_is_float = (el->tag == TYPE_FLOAT || el->tag == TYPE_DOUBLE);
        return true;
    }
    if (t->tag == TYPE_STRUCT || t->tag == TYPE_UNION) {
        size_t ci = 0;
        const char *flex = NULL;
        return zc_struct_resolve_scalar_leaf_bf(t, target_idx, &ci, 0, out_off, out_sz, out_is_bf, out_boff, out_bw, &flex, out_is_float);
    }
    if (target_idx == 0) {
        *out_off = 0;
        *out_sz = t->size ? t->size : 4;
        if (out_is_float) *out_is_float = (t->tag == TYPE_FLOAT || t->tag == TYPE_DOUBLE);
        return true;
    }
    return false;
}

static inline bool zc_object_resolve_scalar_leaf(const ZeroCCType *t, size_t target_idx,
                                                  size_t *out_off, size_t *out_sz, bool *out_is_float) {
    return zc_object_resolve_scalar_leaf_bf(t, target_idx, out_off, out_sz, NULL, NULL, NULL, out_is_float);
}

/* Stamp encoding for NODE_FLAG_INIT_ABS initializer elements:
 * desig_idx = (absolute byte offset within the aggregate) << 3 | size code,
 * size code = log2(element size) (1->0, 2->1, 4->2, 8->3).
 * When ZC_INIT_BF_TAG (bit 30) is set, bits 18..23 hold bit_offset and
 * bits 24..29 hold bit_width, while bits 3..17 hold the byte offset. */
#define ZC_INIT_BF_TAG              (1u << 30)
#define ZC_INIT_ABS_SIZE_CODE(sz)   (((sz) >= 8) ? 3u : (((sz) >= 4) ? 2u : (((sz) >= 2) ? 1u : 0u)))
#define ZC_INIT_ABS_STAMP(off, sz)  ((uint32_t)(ZC_INIT_ABS_TAG | (((off) << 3) | ZC_INIT_ABS_SIZE_CODE((sz)))))
#define ZC_INIT_BF_STAMP(off, sz, boff, bw) \
    ((uint32_t)(ZC_INIT_ABS_TAG | ZC_INIT_BF_TAG | \
                (((uint32_t)(bw) & 0x3Fu) << 24) | \
                (((uint32_t)(boff) & 0x3Fu) << 18) | \
                ((((uint32_t)(off) & 0x7FFFu) << 3) | ZC_INIT_ABS_SIZE_CODE((sz)))))
#define ZC_INIT_IS_BF(stamp)        (((stamp) & ZC_INIT_BF_TAG) != 0)
#define ZC_INIT_BF_BOFF(stamp)      ((uint8_t)(((stamp) >> 18) & 0x3Fu))
#define ZC_INIT_BF_BW(stamp)        ((uint8_t)(((stamp) >> 24) & 0x3Fu))
#define ZC_INIT_ABS_OFF(stamp)      (ZC_INIT_IS_BF(stamp) ? (size_t)(((stamp) >> 3) & 0x7FFFu) : (size_t)(((stamp) & 0x7FFFFFFFu) >> 3))
#define ZC_INIT_ABS_SIZE(stamp)     ((size_t)1u << (size_t)((stamp) & 7u))

#endif /* ZERO_CC_TYPES_TYPE_FUNCS_H */
