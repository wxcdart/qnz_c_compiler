#ifndef ZERO_CC_JIT_TYPE_INFO_H
#define ZERO_CC_JIT_TYPE_INFO_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include <string.h>

#include "zero_cc/core/core.h"
#include "zero_cc/types/types.h"
#include "zero_cc/semantic/decl.h"
#include "zero_cc/semantic/type.h"
#include "zero_cc/semantic/scope.h"

/* Parse and Compile a Top-Level Function */

static inline TypeInfo *zero_cc_type_to_typeinfo(Arena *perm, ZeroCCType t) {
    // FIX: Guard against invalid types (e.g., from malformed external function signatures)
    // If the type has an unreasonably large size, treat it as int to prevent stack overflow
    if (t.size > 1024) {
        return type_int(perm);
    }
    TypeInfo *ti = NULL;
    switch (t.tag) {
        case TYPE_VOID: ti = type_void(perm); break;
        case TYPE_BOOL: ti = type_bool(perm); break;
        case TYPE_INT8:
        case TYPE_UINT8: ti = type_char(perm); break;
        case TYPE_INT16:
        case TYPE_UINT16: ti = type_short(perm); break;
        case TYPE_INT32:
        case TYPE_UINT32: ti = type_int(perm); break;
        case TYPE_INT64:
        case TYPE_UINT64: ti = type_long(perm); break;
        case TYPE_FLOAT: ti = type_float(perm); break;
        case TYPE_DOUBLE: ti = type_double(perm); break;
        case TYPE_PTR: {
            // FIX: Guard against NULL base pointer (incomplete types like FILE*)
            if (!t.base) {
                ti = type_pointer_to(perm, type_void(perm));
            } else {
                TypeInfo *base = zero_cc_type_to_typeinfo(perm, *t.base);
                ti = type_pointer_to(perm, base);
            }
            break;
        }
        case TYPE_ARRAY: {
            // FIX: Guard against NULL base pointer
            if (!t.base) {
                ti = type_array_of(perm, type_void(perm), t.array_len);
            } else {
                TypeInfo *base = zero_cc_type_to_typeinfo(perm, *t.base);
                ti = type_array_of(perm, base, t.array_len);
            }
            if (ti) ti->details.array.is_vla = t.is_vla;
            break;
        }
        case TYPE_STRUCT:
        case TYPE_UNION: {
            ti = type_new(perm);
            ti->kind = (t.tag == TYPE_UNION) ? TYPE_KIND_UNION : TYPE_KIND_STRUCT;
            if (t.struct_name[0] != '\0') {
                size_t tlen = strlen(t.struct_name);
                char *tag_copy = (char *)arena_alloc(perm, tlen + 1);
                if (tag_copy) {
                    memcpy(tag_copy, t.struct_name, tlen + 1);
                    ti->details.record.tag = tag_copy;
                } else {
                    ti->details.record.tag = NULL;
                }
            } else {
                ti->details.record.tag = NULL;
            }
            ti->details.record.size = t.size;
            ti->details.record.align = t.align;
            for (size_t i = 0; i < t.member_count; i++) {
                size_t nlen = strlen(t.members[i].name);
                char *name_copy = (char *)arena_alloc(perm, nlen + 1);
                if (name_copy) {
                    memcpy(name_copy, t.members[i].name, nlen + 1);
                }
                ZeroCCType mt;
                if (t.members[i].base) {
                    mt = *t.members[i].base;
                } else {
                    memset(&mt, 0, sizeof(mt));
                    mt.tag = t.members[i].tag;
                    mt.size = t.members[i].size;
                    mt.align = t.members[i].align;
                    mt.base = NULL;
                }
                TypeInfo *mti = zero_cc_type_to_typeinfo(perm, mt);
                if (mti) {
                    if (t.members[i].is_const) mti->modifiers |= TYPE_MOD_CONST;
                    if (t.members[i].is_volatile) mti->modifiers |= TYPE_MOD_VOLATILE;
                    if (t.members[i].is_atomic) mti->modifiers |= TYPE_MOD_ATOMIC;
                }
                const char *fname = name_copy ? name_copy : t.members[i].name;
                if (t.members[i].is_bitfield) {
                    type_add_bitfield(perm, ti, fname, t.members[i].offset, mti,
                                      t.members[i].bit_offset, t.members[i].bit_width, t.members[i].is_signed);
                } else {
                    type_add_field(perm, ti, fname, t.members[i].offset, mti);
                }
            }
            break;
        }
        case TYPE_ENUM: {
            ti = type_new(perm);
            ti->kind = TYPE_KIND_ENUM;
            if (t.struct_name[0] != '\0') {
                size_t tlen = strlen(t.struct_name);
                char *tag_copy = (char *)arena_alloc(perm, tlen + 1);
                if (tag_copy) {
                    memcpy(tag_copy, t.struct_name, tlen + 1);
                    ti->details.record.tag = tag_copy;
                }
            }
            ti->details.record.size = 4;
            ti->details.record.align = 4;
            break;
        }
        default: ti = type_int(perm); break;
    }
    if (ti) {
        if (t.is_const) ti->modifiers |= TYPE_MOD_CONST;
        if (t.is_volatile) ti->modifiers |= TYPE_MOD_VOLATILE;
        if (t.is_atomic) ti->modifiers |= TYPE_MOD_ATOMIC;
    }
    return ti;
}


#endif /* ZERO_CC_JIT_TYPE_INFO_H */
