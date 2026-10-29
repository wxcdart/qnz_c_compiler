#pragma once

#include "zero_cc/common/arena.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum TypeKind {
    TYPE_KIND_VOID,
    TYPE_KIND_BOOL,
    TYPE_KIND_CHAR,
    TYPE_KIND_SHORT,
    TYPE_KIND_INT,
    TYPE_KIND_LONG,
    TYPE_KIND_FLOAT,
    TYPE_KIND_DOUBLE,
    TYPE_KIND_POINTER,
    TYPE_KIND_ARRAY,
    TYPE_KIND_FUNCTION,
    TYPE_KIND_STRUCT,
    TYPE_KIND_UNION,
    TYPE_KIND_ENUM
} TypeKind;

/* Forward declaration */
typedef struct TypeInfo TypeInfo;
typedef struct FieldInfo FieldInfo;

/* Field info for struct member access */
struct FieldInfo {
    const char *name;
    size_t      offset;
    TypeInfo   *type;
};

/* Type modifier flags */
enum TypeModifier {
    TYPE_MOD_NONE     = 0,
    TYPE_MOD_UNSIGNED = 1 << 0,
    TYPE_MOD_CONST    = 1 << 1,
    TYPE_MOD_VOLATILE = 1 << 2,
    TYPE_MOD_RESTRICT = 1 << 3,
    TYPE_MOD_ATOMIC   = 1 << 4
};

struct TypeInfo {
    TypeKind kind;
    uint32_t modifiers; // Bitmask of enum TypeModifier
    union {
        struct {
            TypeInfo *base;
            size_t    count;
            bool      is_vla;
        } array; // TYPE_ARRAY
        struct {
            TypeInfo *base;
        } pointer; // TYPE_POINTER
        struct {
            TypeInfo **params;
            size_t     param_cnt;
            TypeInfo  *ret;
            bool       is_variadic;
            /* True when the type came from a declaration with a parameter
             * list (incl. explicit (void)).  Empty-paren () declarations and
             * implicit declarations are NOT prototypes: C99 6.7.5.3 only
             * constrains argument counts against prototype types. */
            bool       is_prototype;
        } function; // TYPE_FUNCTION
        // Struct / union / enum representation extensions
        struct {
            const char *tag;
            size_t      size;
            size_t      align;
            // For structs/unions: field info (optional, for member access)
            struct StructField {
                const char *name;
                size_t      offset;
                TypeInfo   *type;
                bool        is_bitfield;
                uint8_t     bit_offset;
                uint8_t     bit_width;
                bool        is_signed;
            } *fields;
            size_t      field_count;
        } record;
    } details;
};

/** Allocate a new TypeInfo from the permanent arena */
static inline TypeInfo *type_new(Arena *perm) {
    TypeInfo *t = (TypeInfo *)arena_alloc(perm, sizeof(TypeInfo));
    if (t) memset(t, 0, sizeof(TypeInfo));
    return t;
}

/* Canonical primitive type accessors/constructors */
TypeInfo *type_void(Arena *perm);
TypeInfo *type_bool(Arena *perm);
TypeInfo *type_char(Arena *perm);
TypeInfo *type_short(Arena *perm);
TypeInfo *type_int(Arena *perm);
TypeInfo *type_long(Arena *perm);
TypeInfo *type_float(Arena *perm);
TypeInfo *type_double(Arena *perm);

/* Unsigned primitive constructors */
TypeInfo *type_uchar(Arena *perm);
TypeInfo *type_ushort(Arena *perm);
TypeInfo *type_uint(Arena *perm);
TypeInfo *type_ulong(Arena *perm);

/* C99 6.4.4.1p6: the type of an integer constant. `suffix` is the lexer
 * int_suffix bit pattern (bit0 = u, bit1 = l, bit2 = ll); a suffix fixes
 * the type even when the value fits a narrower type (1ULL is long long).
 * Unsuffixed: first of int / unsigned int / long (long long) in which the
 * value fits (hex/binary wrap past 2^63 is unsigned long long). */
TypeInfo *type_int_literal(Arena *perm, uint8_t suffix, int64_t value);

/* Derived type constructors */
TypeInfo *type_pointer_to(Arena *perm, TypeInfo *base);
TypeInfo *type_array_of(Arena *perm, TypeInfo *base, size_t count);
TypeInfo *type_function(Arena *perm, TypeInfo *ret, TypeInfo **params, size_t param_cnt, bool is_variadic);

/* Predicates */
bool type_is_void(const TypeInfo *t);
bool type_is_bool(const TypeInfo *t);
bool type_is_integer(const TypeInfo *t);
bool type_is_floating(const TypeInfo *t);
bool type_is_arithmetic(const TypeInfo *t);
bool type_is_pointer(const TypeInfo *t);
bool type_is_array(const TypeInfo *t);
bool type_is_function(const TypeInfo *t);
bool type_is_unsigned(const TypeInfo *t);

/* Comparison and metrics */
bool   type_equal(const TypeInfo *a, const TypeInfo *b);
size_t type_size(const TypeInfo *t);
size_t type_align(const TypeInfo *t);

/* Struct field access */
void type_add_field(Arena *perm, TypeInfo *struct_t, const char *name, size_t offset, TypeInfo *field_type);
void type_add_bitfield(Arena *perm, TypeInfo *struct_t, const char *name, size_t offset, TypeInfo *field_type, uint8_t bit_offset, uint8_t bit_width, bool is_signed);
TypeInfo *type_find_field(const TypeInfo *struct_t, const char *name);
size_t type_get_field_offset(const TypeInfo *struct_t, const char *name);
struct StructField *type_find_field_info(const TypeInfo *struct_t, const char *name);

#ifdef __cplusplus
}
#endif
