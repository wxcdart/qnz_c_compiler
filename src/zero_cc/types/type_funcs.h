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
static inline bool zc_struct_resolve_scalar_leaf(const ZeroCCType *st, size_t target_idx, size_t *cur_idx,
                                                  size_t base_off, size_t *out_off, size_t *out_sz,
                                                  const char **out_flex_name) {
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
                return true;
            }
            *cur_idx += count;
        } else if ((m->tag == TYPE_STRUCT || m->tag == TYPE_UNION) && m->base && m->base->member_count > 0) {
            if (zc_struct_resolve_scalar_leaf(m->base, target_idx, cur_idx, base_off + m->offset, out_off, out_sz, out_flex_name)) {
                return true;
            }
            if (out_flex_name && *out_flex_name) return false;
        } else {
            if (*cur_idx == target_idx) {
                *out_off = base_off + m->offset;
                *out_sz = m->size;
                return true;
            }
            (*cur_idx)++;
        }
    }
    return false;
}

static inline size_t zc_object_count_scalar_leaves(const ZeroCCType *t) {
    if (!t) return 0;
    if (t->tag == TYPE_ARRAY) {
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
                n++;
            }
        }
        return n;
    }
    return 1;
}

static inline bool zc_object_resolve_scalar_leaf(const ZeroCCType *t, size_t target_idx,
                                                  size_t *out_off, size_t *out_sz) {
    if (!t) return false;
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
                if (!zc_object_resolve_scalar_leaf(el, wi, &sub_off, &sub_sz)) return false;
            } else {
                size_t ci = 0;
                const char *flex = NULL;
                if (!zc_struct_resolve_scalar_leaf(el, wi, &ci, 0, &sub_off, &sub_sz, &flex))
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
        return true;
    }
    if (t->tag == TYPE_STRUCT || t->tag == TYPE_UNION) {
        size_t ci = 0;
        const char *flex = NULL;
        return zc_struct_resolve_scalar_leaf(t, target_idx, &ci, 0, out_off, out_sz, &flex);
    }
    if (target_idx == 0) {
        *out_off = 0;
        *out_sz = t->size ? t->size : 4;
        return true;
    }
    return false;
}

/* Stamp encoding for NODE_FLAG_INIT_ABS initializer elements:
 * desig_idx = (absolute byte offset within the aggregate) << 3 | size code,
 * size code = log2(element size) (1->0, 2->1, 4->2, 8->3). */
#define ZC_INIT_ABS_SIZE_CODE(sz)   (((sz) >= 8) ? 3u : (((sz) >= 4) ? 2u : (((sz) >= 2) ? 1u : 0u)))
#define ZC_INIT_ABS_STAMP(off, sz)  ((uint32_t)(ZC_INIT_ABS_TAG | (((off) << 3) | ZC_INIT_ABS_SIZE_CODE((sz)))))
#define ZC_INIT_ABS_OFF(stamp)      ((size_t)(((stamp) & 0x7FFFFFFFu) >> 3))
#define ZC_INIT_ABS_SIZE(stamp)     ((size_t)1u << (size_t)((stamp) & 7u))

#endif /* ZERO_CC_TYPES_TYPE_FUNCS_H */
