#ifndef ZERO_CC_JIT_TYPE_INFO_H
#define ZERO_CC_JIT_TYPE_INFO_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "zero_cc/core.h"
#include "zero_cc/types_lexer.h"
#include "zero_cc/semantic/decl.h"
#include "zero_cc/semantic/type.h"
#include "zero_cc/semantic/scope.h"

/* Parse and Compile a Top-Level Function */

static inline TypeInfo *zero_cc_type_to_typeinfo(Arena *perm, ZeroCCType t) {
    switch (t.tag) {
        case TYPE_VOID: return type_void(perm);
        case TYPE_BOOL: return type_bool(perm);
        case TYPE_INT8:
        case TYPE_UINT8: return type_char(perm);
        case TYPE_INT16:
        case TYPE_UINT16: return type_short(perm);
        case TYPE_INT32:
        case TYPE_UINT32: return type_int(perm);
        case TYPE_INT64:
        case TYPE_UINT64: return type_long(perm);
        case TYPE_FLOAT: return type_float(perm);
        case TYPE_DOUBLE: return type_double(perm);
        case TYPE_PTR: {
            TypeInfo *base = zero_cc_type_to_typeinfo(perm, *t.base);
            return type_pointer_to(perm, base);
        }
        case TYPE_ARRAY: {
            TypeInfo *base = zero_cc_type_to_typeinfo(perm, *t.base);
            return type_array_of(perm, base, t.array_len);
        }
        case TYPE_STRUCT: {
            TypeInfo *ti = type_new(perm);
            ti->kind = TYPE_KIND_STRUCT;
            ti->details.record.tag = t.struct_name;
            ti->details.record.size = t.size;
            ti->details.record.align = t.align;
            return ti;
        }
        default: return type_int(perm);
    }
}


#endif /* ZERO_CC_JIT_TYPE_INFO_H */
