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
    // Guard against uninitialized/corrupted types while allowing valid large arrays and structs.
    // NOTE: the threshold must comfortably exceed the largest legitimate struct.  zerocc's
    // own state struct (ZeroCCCompiler) is ~62 MB because it embeds large fixed arrays
    // (structs[], typedefs[], buffers, ...), so a 16 MB bound mis-flagged it as corrupt
    // and silently degraded pointers to it into pointer_to(int), breaking every member
    // access on `c->...`.  A genuinely uninitialized size is a random 64-bit value
    // (typically >1 TB), so a 1 GB bound still catches corruption while admitting real
    // structs and large user arrays.
    if (t.size > (1ULL * 1024 * 1024 * 1024)) {
        return type_int(perm);
    }
    TypeInfo *ti = NULL;
    switch (t.tag) {
        case TYPE_VOID: ti = type_void(perm); break;
        case TYPE_BOOL: ti = type_bool(perm); break;
        case TYPE_INT8: ti = type_char(perm); break;
        case TYPE_UINT8: ti = type_uchar(perm); break;
        case TYPE_INT16: ti = type_short(perm); break;
        case TYPE_UINT16: ti = type_ushort(perm); break;
        case TYPE_INT32: ti = type_int(perm); break;
        case TYPE_UINT32: ti = type_uint(perm); break;
        case TYPE_INT64: ti = type_long(perm); break;
        case TYPE_UINT64: ti = type_ulong(perm); break;
        case TYPE_FLOAT: ti = type_float(perm); break;
        case TYPE_DOUBLE: ti = type_double(perm); break;
        case TYPE_PTR: {
            // FIX: Guard against NULL base pointer (incomplete types like FILE*)
            if (!t.base) {
                ti = type_pointer_to(perm, type_void(perm));
            } else if (t.is_func_ptr) {
                TypeInfo *ret_ti = zero_cc_type_to_typeinfo(perm, *t.base);
                TypeInfo *fn_ti = type_function(perm, ret_ti, NULL, 0, true);
                ti = type_pointer_to(perm, fn_ti);
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
                if (t.members[i].tag == TYPE_ARRAY) {
                    memset(&mt, 0, sizeof(mt));
                    mt.tag = TYPE_ARRAY;
                    mt.size = t.members[i].size;
                    mt.align = t.members[i].align;
                    mt.base = t.members[i].base;
                    mt.array_len = (t.members[i].base && t.members[i].base->size > 0)
                                       ? (t.members[i].size / t.members[i].base->size)
                                       : 0;
                } else if (t.members[i].tag == TYPE_PTR) {
                    /* Pointer member: the member's own type is the pointer
                     * (C5). m->base is the pointee (may be NULL → ptr→void). */
                    memset(&mt, 0, sizeof(mt));
                    mt.tag = TYPE_PTR;
                    mt.size = t.members[i].size ? t.members[i].size : 8;
                    mt.align = t.members[i].align ? t.members[i].align : 8;
                    mt.base = t.members[i].base;
                } else if (t.members[i].base) {
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
        if (t.is_restrict) ti->modifiers |= TYPE_MOD_RESTRICT;
        if (t.is_atomic) ti->modifiers |= TYPE_MOD_ATOMIC;
    }
    return ti;
}

/* Convert a semantic TypeInfo* back into a canonical ZeroCCType */
static inline ZeroCCType typeinfo_to_zero_cc_type(const TypeInfo *ti) {
    ZeroCCType t;
    memset(&t, 0, sizeof(t));
    if (!ti) {
        return zero_cc_type_int();
    }
    bool is_u = (ti->modifiers & TYPE_MOD_UNSIGNED) != 0;
    switch (ti->kind) {
        case TYPE_KIND_VOID:    t = zero_cc_type_void(); break;
        case TYPE_KIND_BOOL:    t = zero_cc_type_bool(); break;
        case TYPE_KIND_CHAR:    t = is_u ? zero_cc_type_uint8()  : zero_cc_type_char(); break;
        case TYPE_KIND_SHORT:   t = is_u ? zero_cc_type_uint16() : zero_cc_type_int16(); break;
        case TYPE_KIND_INT:     t = is_u ? zero_cc_type_uint32() : zero_cc_type_int(); break;
        case TYPE_KIND_LONG:    t = is_u ? zero_cc_type_uint64() : zero_cc_type_int64(); break;
        case TYPE_KIND_FLOAT:   t = zero_cc_type_float(); break;
        case TYPE_KIND_DOUBLE:  t = zero_cc_type_double(); break;
        case TYPE_KIND_ENUM:    t = zero_cc_type_enum(ti->details.record.tag); break;
        case TYPE_KIND_POINTER: {
            if (ti->details.pointer.base && ti->details.pointer.base->kind == TYPE_KIND_FUNCTION) {
                ZeroCCType ret_t = typeinfo_to_zero_cc_type(ti->details.pointer.base->details.function.ret);
                t = zero_cc_type_ptr(&ret_t);
                t.is_func_ptr = true;
            } else if (ti->details.pointer.base) {
                ZeroCCType base_t = typeinfo_to_zero_cc_type(ti->details.pointer.base);
                t = zero_cc_type_ptr(&base_t);
            } else {
                t = zero_cc_type_ptr(NULL);
            }
            break;
        }
        case TYPE_KIND_ARRAY: {
            ZeroCCType base_t = typeinfo_to_zero_cc_type(ti->details.array.base);
            t = zero_cc_type_array(&base_t, (ssize_t)ti->details.array.count);
            if (ti->details.array.is_vla) {
                t.is_vla = true;
                t.size = 8;
                t.align = 8;
            }
            break;
        }
        case TYPE_KIND_STRUCT:
        case TYPE_KIND_UNION: {
            t.tag = (ti->kind == TYPE_KIND_UNION) ? TYPE_UNION : TYPE_STRUCT;
            t.size = ti->details.record.size;
            t.align = ti->details.record.align ? ti->details.record.align : 1;
            if (ti->details.record.tag) {
                snprintf(t.struct_name, sizeof(t.struct_name), "%s", ti->details.record.tag);
            }
            break;
        }
        default:
            t = zero_cc_type_int();
            break;
    }
    t.is_const    = (ti->modifiers & TYPE_MOD_CONST) != 0;
    t.is_volatile = (ti->modifiers & TYPE_MOD_VOLATILE) != 0;
    t.is_restrict = (ti->modifiers & TYPE_MOD_RESTRICT) != 0;
    t.is_atomic   = (ti->modifiers & TYPE_MOD_ATOMIC) != 0;
    return t;
}

/* Canonical accessor to retrieve TypeInfo* attached to an AST node */
static inline TypeInfo *ast_node_get_typeinfo(ZeroCCCompiler *c, const ZeroCCASTNode *node) {
    if (!c || !c->sem_ctx || !c->sem_ctx->perm_arena || !node || node->aux.extra == 0) {
        return NULL;
    }
    return (TypeInfo *)arena_index_to_ptr(c->sem_ctx->perm_arena, (size_t)(node->aux.extra - 1));
}

/* Canonical accessor to attach TypeInfo* to an AST node */
static inline void ast_node_set_typeinfo(ZeroCCCompiler *c, ZeroCCASTNode *node, TypeInfo *ti) {
    if (!c || !c->sem_ctx || !c->sem_ctx->perm_arena || !node || !ti) {
        return;
    }
    size_t offset = arena_index_of(c->sem_ctx->perm_arena, ti);
    if (offset != (size_t)-1) {
        node->aux.extra = (uint32_t)(offset + 1);
    }
}

/* Canonical base type resolver for declarations, parameters, and return types */
static inline ZeroCCType zero_cc_resolve_base_type(ZeroCCCompiler *c, ZeroCCTokType type_tok, const char *type_name) {
    if (type_tok == TK_CHAR || type_tok == TK_BOOL_KW) return zero_cc_type_char();
    if (type_tok == TK_SHORT) return zero_cc_type_int16();
    if (type_tok == TK_INT) return zero_cc_type_int();
    if (type_tok == TK_FLOAT_KW) return zero_cc_type_float();
    if (type_tok == TK_DOUBLE_KW) return zero_cc_type_double();
    if (type_tok == TK_VOID) return zero_cc_type_void();
    if (type_tok == TK_STRUCT || type_tok == TK_UNION) {
        ZeroCCType *st = type_name ? zero_cc_find_struct(c, type_name) : NULL;
        if (!st && type_name) st = zero_cc_find_typedef(c, type_name);
        return st ? *st : zero_cc_type_int64();
    }
    if (type_tok == TK_ENUM) {
        ZeroCCType *et = type_name ? zero_cc_find_enum(c, type_name) : NULL;
        if (!et && type_name) et = zero_cc_find_typedef(c, type_name);
        return et ? *et : zero_cc_type_enum(type_name);
    }
    if (type_name) {
        ZeroCCType *tdef = zero_cc_find_typedef(c, type_name);
        if (!tdef) tdef = zero_cc_find_struct(c, type_name);
        if (!tdef) tdef = zero_cc_find_enum(c, type_name);
        if (tdef) return *tdef;
    }
    return zero_cc_type_int64();
}

#endif /* ZERO_CC_JIT_TYPE_INFO_H */
