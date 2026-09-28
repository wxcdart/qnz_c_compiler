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
static inline ZeroCCType zero_cc_type_user(int user_idx, size_t size, size_t align) {
    ZeroCCType t; memset(&t, 0, sizeof(t));
    t.tag = (ZeroTypeTag)(TYPE_USER_0 + (user_idx & 3));
    t.size = size ? size : 16;
    t.align = align ? align : 16;
    return t;
}
static inline ZeroCCType zero_cc_type_ptr(ZeroCCType *base) {
    ZeroCCType t; memset(&t, 0, sizeof(t));
    t.tag = TYPE_PTR; t.size = 8; t.align = 8;
    if (base) {
        ZeroCCType *heap_base = (ZeroCCType *)malloc(sizeof(ZeroCCType));
        *heap_base = *base;
        t.base = heap_base;
    } else {
        t.base = NULL;
    }
    return t;
}

static inline ZeroCCType zero_cc_type_array(ZeroCCType *base, ssize_t length) {
    ZeroCCType t; memset(&t, 0, sizeof(t)); t.tag = TYPE_ARRAY; t.base = base; t.array_len = length; if (length > 0) { t.size = base->size * length; } else { t.size = 0; } t.align = base->align; return t;
}

static inline ZeroCCType zero_cc_type_strip_qualifiers(ZeroCCType ty) {
    ty.is_const = false;
    ty.is_volatile = false;
    ty.is_atomic = false;
    return ty;
}


#endif /* ZERO_CC_TYPES_TYPE_FUNCS_H */
