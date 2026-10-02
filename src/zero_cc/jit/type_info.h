#ifndef ZERO_CC_JIT_TYPE_INFO_H
#define ZERO_CC_JIT_TYPE_INFO_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include <string.h>

#include "zero_cc/core.h"
#include "zero_cc/types_lexer.h"
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
            // FIX: Guard against NULL base pointer (incomplete types like FILE*)
            if (!t.base) {
                return type_pointer_to(perm, type_void(perm));
            }
            TypeInfo *base = zero_cc_type_to_typeinfo(perm, *t.base);
            return type_pointer_to(perm, base);
        }
        case TYPE_ARRAY: {
            // FIX: Guard against NULL base pointer
            TypeInfo *ti;
            if (!t.base) {
                ti = type_array_of(perm, type_void(perm), t.array_len);
            } else {
                TypeInfo *base = zero_cc_type_to_typeinfo(perm, *t.base);
                ti = type_array_of(perm, base, t.array_len);
            }
            if (ti) ti->details.array.is_vla = t.is_vla;
            return ti;
        }
        case TYPE_STRUCT:
        case TYPE_UNION: {
            TypeInfo *ti = type_new(perm);
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
                const char *fname = name_copy ? name_copy : t.members[i].name;
                if (t.members[i].is_bitfield) {
                    type_add_bitfield(perm, ti, fname, t.members[i].offset, mti,
                                      t.members[i].bit_offset, t.members[i].bit_width, t.members[i].is_signed);
                } else {
                    type_add_field(perm, ti, fname, t.members[i].offset, mti);
                }
            }
            return ti;
        }
        default: return type_int(perm);
    }
}


#endif /* ZERO_CC_JIT_TYPE_INFO_H */
