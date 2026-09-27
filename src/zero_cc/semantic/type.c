#include "type.h"
#include <string.h>

/* Allocate and initialize a basic type */
static TypeInfo *make_basic(Arena *perm, TypeKind kind, uint32_t modifiers) {
    TypeInfo *t = type_new(perm);
    if (!t) return NULL;
    t->kind = kind;
    t->modifiers = modifiers;
    memset(&t->details, 0, sizeof(t->details));
    return t;
}

TypeInfo *type_void(Arena *perm) {
    return make_basic(perm, TYPE_KIND_VOID, TYPE_MOD_NONE);
}

TypeInfo *type_bool(Arena *perm) {
    return make_basic(perm, TYPE_KIND_BOOL, TYPE_MOD_NONE);
}

TypeInfo *type_char(Arena *perm) {
    return make_basic(perm, TYPE_KIND_CHAR, TYPE_MOD_NONE);
}

TypeInfo *type_short(Arena *perm) {
    return make_basic(perm, TYPE_KIND_SHORT, TYPE_MOD_NONE);
}

TypeInfo *type_int(Arena *perm) {
    return make_basic(perm, TYPE_KIND_INT, TYPE_MOD_NONE);
}

TypeInfo *type_long(Arena *perm) {
    return make_basic(perm, TYPE_KIND_LONG, TYPE_MOD_NONE);
}

TypeInfo *type_float(Arena *perm) {
    return make_basic(perm, TYPE_KIND_FLOAT, TYPE_MOD_NONE);
}

TypeInfo *type_double(Arena *perm) {
    return make_basic(perm, TYPE_KIND_DOUBLE, TYPE_MOD_NONE);
}

TypeInfo *type_uchar(Arena *perm) {
    return make_basic(perm, TYPE_KIND_CHAR, TYPE_MOD_UNSIGNED);
}

TypeInfo *type_ushort(Arena *perm) {
    return make_basic(perm, TYPE_KIND_SHORT, TYPE_MOD_UNSIGNED);
}

TypeInfo *type_uint(Arena *perm) {
    return make_basic(perm, TYPE_KIND_INT, TYPE_MOD_UNSIGNED);
}

TypeInfo *type_ulong(Arena *perm) {
    return make_basic(perm, TYPE_KIND_LONG, TYPE_MOD_UNSIGNED);
}

TypeInfo *type_pointer_to(Arena *perm, TypeInfo *base) {
    TypeInfo *t = make_basic(perm, TYPE_KIND_POINTER, TYPE_MOD_NONE);
    if (!t) return NULL;
    t->details.pointer.base = base;
    return t;
}

TypeInfo *type_array_of(Arena *perm, TypeInfo *base, size_t count) {
    TypeInfo *t = make_basic(perm, TYPE_KIND_ARRAY, TYPE_MOD_NONE);
    if (!t) return NULL;
    t->details.array.base = base;
    t->details.array.count = count;
    return t;
}

TypeInfo *type_function(Arena *perm, TypeInfo *ret, TypeInfo **params, size_t param_cnt, bool is_variadic) {
    TypeInfo *t = make_basic(perm, TYPE_KIND_FUNCTION, TYPE_MOD_NONE);
    if (!t) return NULL;
    t->details.function.ret = ret;
    t->details.function.param_cnt = param_cnt;
    t->details.function.is_variadic = is_variadic;
    if (param_cnt > 0 && params) {
        TypeInfo **p_copy = (TypeInfo **)arena_alloc(perm, param_cnt * sizeof(TypeInfo *));
        if (p_copy) {
            memcpy(p_copy, params, param_cnt * sizeof(TypeInfo *));
        }
        t->details.function.params = p_copy;
    } else {
        t->details.function.params = NULL;
    }
    return t;
}

bool type_is_void(const TypeInfo *t) {
    return t && t->kind == TYPE_KIND_VOID;
}

bool type_is_bool(const TypeInfo *t) {
    return t && t->kind == TYPE_KIND_BOOL;
}

bool type_is_integer(const TypeInfo *t) {
    if (!t) return false;
    switch (t->kind) {
        case TYPE_KIND_BOOL:
        case TYPE_KIND_CHAR:
        case TYPE_KIND_SHORT:
        case TYPE_KIND_INT:
        case TYPE_KIND_LONG:
        case TYPE_KIND_ENUM:
            return true;
        default:
            return false;
    }
}

bool type_is_floating(const TypeInfo *t) {
    if (!t) return false;
    return t->kind == TYPE_KIND_FLOAT || t->kind == TYPE_KIND_DOUBLE;
}

bool type_is_arithmetic(const TypeInfo *t) {
    return type_is_integer(t) || type_is_floating(t);
}

bool type_is_pointer(const TypeInfo *t) {
    return t && t->kind == TYPE_KIND_POINTER;
}

bool type_is_array(const TypeInfo *t) {
    return t && t->kind == TYPE_KIND_ARRAY;
}

bool type_is_function(const TypeInfo *t) {
    return t && t->kind == TYPE_KIND_FUNCTION;
}

bool type_is_unsigned(const TypeInfo *t) {
    return t && ((t->modifiers & TYPE_MOD_UNSIGNED) != 0);
}

bool type_equal(const TypeInfo *a, const TypeInfo *b) {
    if (a == b) return true;
    if (!a || !b) return false;
    if (a->kind != b->kind) return false;
    /* Basic unsigned vs signed check */
    if ((a->modifiers & TYPE_MOD_UNSIGNED) != (b->modifiers & TYPE_MOD_UNSIGNED)) {
        return false;
    }

    switch (a->kind) {
        case TYPE_KIND_VOID:
        case TYPE_KIND_BOOL:
        case TYPE_KIND_CHAR:
        case TYPE_KIND_SHORT:
        case TYPE_KIND_INT:
        case TYPE_KIND_LONG:
        case TYPE_KIND_FLOAT:
        case TYPE_KIND_DOUBLE:
            return true;

        case TYPE_KIND_POINTER:
            return type_equal(a->details.pointer.base, b->details.pointer.base);

        case TYPE_KIND_ARRAY:
            if (a->details.array.count != b->details.array.count) return false;
            return type_equal(a->details.array.base, b->details.array.base);

        case TYPE_KIND_FUNCTION: {
            if (a->details.function.is_variadic != b->details.function.is_variadic) return false;
            if (a->details.function.param_cnt != b->details.function.param_cnt) return false;
            if (!type_equal(a->details.function.ret, b->details.function.ret)) return false;
            for (size_t i = 0; i < a->details.function.param_cnt; ++i) {
                if (!type_equal(a->details.function.params[i], b->details.function.params[i])) {
                    return false;
                }
            }
            return true;
        }

        case TYPE_KIND_STRUCT:
        case TYPE_KIND_UNION:
        case TYPE_KIND_ENUM:
            if (a->details.record.tag && b->details.record.tag) {
                return strcmp(a->details.record.tag, b->details.record.tag) == 0;
            }
            return false;

        default:
            return false;
    }
}

size_t type_size(const TypeInfo *t) {
    if (!t) return 0;
    switch (t->kind) {
        case TYPE_KIND_VOID:
            return 0; // In standard C, sizeof(void) is incomplete / 0 (GNU C allows 1)
        case TYPE_KIND_BOOL:
            return 1;
        case TYPE_KIND_CHAR:
            return 1;
        case TYPE_KIND_SHORT:
            return 2;
        case TYPE_KIND_INT:
            return 4;
        case TYPE_KIND_LONG:
            return 8;
        case TYPE_KIND_FLOAT:
            return 4;
        case TYPE_KIND_DOUBLE:
            return 8;
        case TYPE_KIND_POINTER:
            return 8; // x86_64 target
        case TYPE_KIND_ARRAY:
            return t->details.array.count * type_size(t->details.array.base);
        case TYPE_KIND_FUNCTION:
            return 0;
        case TYPE_KIND_STRUCT:
        case TYPE_KIND_UNION:
            return t->details.record.size;
        case TYPE_KIND_ENUM:
            return 4;
        default:
            return 0;
    }
}

size_t type_align(const TypeInfo *t) {
    if (!t) return 0;
    switch (t->kind) {
        case TYPE_KIND_VOID:
            return 1;
        case TYPE_KIND_BOOL:
            return 1;
        case TYPE_KIND_CHAR:
            return 1;
        case TYPE_KIND_SHORT:
            return 2;
        case TYPE_KIND_INT:
            return 4;
        case TYPE_KIND_LONG:
            return 8;
        case TYPE_KIND_FLOAT:
            return 4;
        case TYPE_KIND_DOUBLE:
            return 8;
        case TYPE_KIND_POINTER:
            return 8;
        case TYPE_KIND_ARRAY:
            return type_align(t->details.array.base);
        case TYPE_KIND_FUNCTION:
            return 1;
        case TYPE_KIND_STRUCT:
        case TYPE_KIND_UNION:
            return t->details.record.align ? t->details.record.align : 1;
        case TYPE_KIND_ENUM:
            return 4;
        default:
            return 1;
    }
}
