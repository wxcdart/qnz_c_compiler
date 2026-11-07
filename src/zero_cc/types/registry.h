#ifndef ZERO_CC_TYPES_REGISTRY_H
#define ZERO_CC_TYPES_REGISTRY_H

#include <stdio.h>
#include <string.h>
#include "zero_cc/semantic/diagnostics.h"

/* Struct Registry */
static inline void zero_cc_register_struct(ZeroCCCompiler *c, const char *name, ZeroCCType type) {
    for (size_t i = 0; i < c->struct_count; i++) {
        if (strcmp(c->structs[i].name, name) == 0) {
            ZeroCCType *prev = &c->structs[i].type;
            /* Block-scope tag redefinition (C11 6.7.2.1 / GCC extension):
             * identical redefinitions and incomplete->complete transitions
             * are fine. A DIFFERENT layout under the same tag in sibling
             * scopes cannot be represented by the global registry (both
             * definitions would alias one entry, corrupting offsets and
             * sizes) - reject cleanly instead of miscompiling. */
            bool prev_incomplete = (prev->member_count == 0);
            bool type_incomplete = (type.member_count == 0);
            bool same_layout = (prev->size == type.size &&
                                prev->member_count == type.member_count &&
                                prev->align == type.align &&
                                prev->is_vla_struct == type.is_vla_struct);
            if (same_layout && prev->members && type.members &&
                prev->member_count == type.member_count && prev->member_count > 0) {
                for (size_t mi = 0; mi < type.member_count; mi++) {
                    if (strcmp(prev->members[mi].name, type.members[mi].name) != 0 ||
                        prev->members[mi].offset != type.members[mi].offset) {
                        same_layout = false;
                        break;
                    }
                }
            }
            if (!prev_incomplete && type_incomplete) {
                /* Complete definition followed by a bare forward
                 * declaration (struct S;) - keep the complete entry. */
                return;
            }
            if (!same_layout && !prev_incomplete) {
                diag_report_error_at_token(c,
                    "redefinition of '%s %s' with a different layout is not supported",
                    type.tag == TYPE_UNION ? "union" : "struct", name);
            }
            /* The overwritten array may no longer be referenced by any
             * table or pool node (function-local struct definitions are
             * not in c->locals) - record it in the compiler ownership
             * list so zero_cc_compiler_free releases it (make asan / LSan:
             * the redefinition-rejection path leaked 1664 B). */
            zero_cc_note_members_array(c, prev->members);
            c->structs[i].type = type;
            return;
        }
    }
    if (c->struct_count < ZERO_CC_MAX_STRUCTS) {
        snprintf(c->structs[c->struct_count].name, 128, "%s", name);
        c->structs[c->struct_count].type = type;
        c->struct_count++;
    }
}

static inline ZeroCCType* zero_cc_find_struct(ZeroCCCompiler *c, const char *name) {
    for (size_t i = 0; i < c->struct_count; i++) {
        if (strcmp(c->structs[i].name, name) == 0) {
            return &c->structs[i].type;
        }
    }
    return NULL;
}

/* Propagate a completed struct/union definition into a ZeroCCType tree:
 * replace any node whose tag matches `name` (a forward-declaration
 * snapshot), then recurse into the pointee/element base.  ZeroCCType
 * copies share no state by value, so a typedef registered from an
 * opaque declaration (e.g. `typedef struct S S;`) keeps the incomplete
 * snapshot forever unless something like this runs when the definition
 * arrives. */
static inline void zero_cc_type_sync_struct(ZeroCCType *t, const char *name, const ZeroCCType *complete) {
    if (!t || !name || !complete) return;
    if ((t->tag == TYPE_STRUCT || t->tag == TYPE_UNION) &&
        t->struct_name[0] != '\0' && strcmp(t->struct_name, name) == 0) {
        /* Replace with the complete definition, but PRESERVE the qualifiers
         * carried by this instance: a forward-reference pointee may be
         * qualified per-use (`const struct S *p;` records is_const on the
         * pointee base) and the definition itself never carries them -
         * a bare `*t = *complete` would wipe the const and turn every
         * later `->p = const_ptr` into a spurious qualifier-discard error
         * (sqlite3_file.pMethods = `const struct sqlite3_io_methods *`). */
        ZeroCCType merged = *complete;
        merged.is_const = t->is_const;
        merged.is_volatile = t->is_volatile;
        merged.is_restrict = t->is_restrict;
        merged.is_atomic = t->is_atomic;
        *t = merged;
        return;
    }
    if (t->base) zero_cc_type_sync_struct(t->base, name, complete);
}

static inline void zero_cc_sync_typedefs_with_struct(ZeroCCCompiler *c, const char *name, const ZeroCCType *complete) {
    if (!c || !name || name[0] == '\0' || !complete) return;
    for (size_t i = 0; i < c->typedef_count; i++) {
        zero_cc_type_sync_struct(&c->typedefs[i].type, name, complete);
    }
}

/* Typedef Registry */
static inline void zero_cc_register_typedef(ZeroCCCompiler *c, const char *alias, ZeroCCType type) {
    for (size_t i = 0; i < c->typedef_count; i++) {
        if (strcmp(c->typedefs[i].alias, alias) == 0) {
            /* Retired-array recording: see zero_cc_register_struct. */
            zero_cc_note_members_array(c, c->typedefs[i].type.members);
            c->typedefs[i].type = type;
            return;
        }
    }
    if (c->typedef_count < ZERO_CC_MAX_TYPEDEFS) {
        snprintf(c->typedefs[c->typedef_count].alias, 128, "%s", alias);
        c->typedefs[c->typedef_count].type = type;
        c->typedef_count++;
    }
}

static inline ZeroCCType* zero_cc_find_typedef(ZeroCCCompiler *c, const char *alias) {
    for (size_t i = 0; i < c->typedef_count; i++) {
        if (strcmp(c->typedefs[i].alias, alias) == 0) {
            return &c->typedefs[i].type;
        }
    }
    return NULL;
}

/* C99 6.7.2.3 / GNU: after a leading `_Complex` / `__complex__` specifier,
 * the next token is an explicit inner real type only if it is itself a type
 * specifier (a basic keyword, struct/union/enum, or a known typedef).  A
 * pointer star or a plain identifier is the declarator, meaning `_Complex`
 * stands alone and denotes `_Complex double` (GNU extension; strict ISO C99
 * requires the real type to be named).  Used so a plain `_Complex x;` does
 * not swallow `x` as the (defaulted-int) inner type. */
static inline bool zero_cc_token_is_complex_inner_base(ZeroCCCompiler *c, size_t pos) {
    if (!c || pos >= c->token_count) return false;
    ZeroCCTokType t = c->tokens[pos].type;
    if (t == TK_INT || t == TK_CHAR || t == TK_SHORT || t == TK_LONG ||
        t == TK_FLOAT_KW || t == TK_DOUBLE_KW || t == TK_SIGNED ||
        t == TK_UNSIGNED || t == TK_BOOL_KW || t == TK_VOID ||
        t == TK_ENUM || t == TK_STRUCT || t == TK_UNION)
        return true;
    if (t == TK_IDENT && zero_cc_find_typedef(c, c->tokens[pos].str_val))
        return true;
    return false;
}

/* Enum Type Registry */
static inline void zero_cc_register_enum(ZeroCCCompiler *c, const char *name, ZeroCCType type) {
    for (size_t i = 0; i < c->enum_count; i++) {
        if (strcmp(c->enums[i].name, name) == 0) {
            c->enums[i].type = type;
            return;
        }
    }
    if (c->enum_count < ZERO_CC_MAX_ENUM_DEFS) {
        snprintf(c->enums[c->enum_count].name, 128, "%s", name);
        c->enums[c->enum_count].type = type;
        c->enum_count++;
    }
}

static inline ZeroCCType* zero_cc_find_enum(ZeroCCCompiler *c, const char *name) {
    for (size_t i = 0; i < c->enum_count; i++) {
        if (strcmp(c->enums[i].name, name) == 0) {
            return &c->enums[i].type;
        }
    }
    return NULL;
}

/* Enum Constant Registry — open-addressing hash table (string -> value) */

/* Forward declarations to break cycles between grow <-> register */
static inline void zero_cc_register_enum_const(ZeroCCCompiler *c, const char *name, int64_t val);
static inline ZeroCCEnumConst* zero_cc_find_enum_const(ZeroCCCompiler *c, const char *name);

static inline size_t enum_const_hash(const char *name, size_t capacity) {
    uint64_t h = zero_cc_hash64(name);
    return (size_t)(h & (capacity - 1));
}

static inline void enum_const_table_init(ZeroCCCompiler *c) {
    if (c->enum_const_capacity) return; /* already initialized */
    c->enum_const_capacity = 256;
    c->enum_constants = (ZeroCCEnumConst *)calloc(c->enum_const_capacity,
                                                 sizeof(ZeroCCEnumConst));
    c->enum_const_count = 0;
}

static inline void enum_const_table_reset(ZeroCCCompiler *c) {
    /* Reuse allocated table, just clear entries */
    if (c->enum_constants) {
        for (size_t i = 0; i < c->enum_const_capacity; i++) {
            c->enum_constants[i].name[0] = '\0';
        }
    }
    c->enum_const_count = 0;
}

static inline void enum_const_table_grow(ZeroCCCompiler *c);

static inline void zero_cc_register_enum_const(ZeroCCCompiler *c, const char *name, int64_t val) {
    enum_const_table_init(c);
    /* Check load factor before insertion */
    if (c->enum_const_count * 10 >= c->enum_const_capacity * 7) {
        enum_const_table_grow(c);
    }
    size_t idx = enum_const_hash(name, c->enum_const_capacity);
    for (size_t probe = 0; probe < c->enum_const_capacity; probe++) {
        size_t pos = (idx + probe) & (c->enum_const_capacity - 1);
        if (c->enum_constants[pos].name[0] == '\0' ||
            strcmp(c->enum_constants[pos].name, name) == 0) {
            if (c->enum_constants[pos].name[0] == '\0') {
                c->enum_const_count++;
            }
            snprintf(c->enum_constants[pos].name, 128, "%s", name);
            c->enum_constants[pos].value = val;
            return;
        }
    }
    /* Should not reach here if grow works correctly */
    enum_const_table_grow(c);
    zero_cc_register_enum_const(c, name, val);
}

static inline void enum_const_table_grow(ZeroCCCompiler *c) {
    size_t old_cap = c->enum_const_capacity;
    ZeroCCEnumConst *old_table = c->enum_constants;
    size_t new_cap = old_cap ? old_cap * 2 : 256;
    c->enum_constants = (ZeroCCEnumConst *)calloc(new_cap, sizeof(ZeroCCEnumConst));
    c->enum_const_capacity = new_cap;
    c->enum_const_count = 0;
    if (old_table) {
        for (size_t i = 0; i < old_cap; i++) {
            if (old_table[i].name[0] != '\0') {
                zero_cc_register_enum_const(c, old_table[i].name, old_table[i].value);
            }
        }
        free(old_table);
    }
}

static inline ZeroCCEnumConst* zero_cc_find_enum_const(ZeroCCCompiler *c, const char *name) {
    if (!c->enum_constants || c->enum_const_count == 0) return NULL;
    size_t idx = enum_const_hash(name, c->enum_const_capacity);
    for (size_t probe = 0; probe < c->enum_const_capacity; probe++) {
        size_t pos = (idx + probe) & (c->enum_const_capacity - 1);
        if (c->enum_constants[pos].name[0] == '\0') return NULL; /* empty slot */
        if (strcmp(c->enum_constants[pos].name, name) == 0) {
            return &c->enum_constants[pos];
        }
    }
    return NULL;
}

static inline void zero_cc_init_standard_types(ZeroCCCompiler *c) {
    zero_cc_register_typedef(c, "void", zero_cc_type_void());
    zero_cc_register_typedef(c, "bool", zero_cc_type_bool());
    zero_cc_register_typedef(c, "_Bool", zero_cc_type_bool());
    zero_cc_register_typedef(c, "char", zero_cc_type_char());
    zero_cc_register_typedef(c, "int8_t", zero_cc_type_char());
    zero_cc_register_typedef(c, "uint8_t", zero_cc_type_uint8());
    zero_cc_register_typedef(c, "unsigned char", zero_cc_type_uint8());
    zero_cc_register_typedef(c, "short", zero_cc_type_int16());
    zero_cc_register_typedef(c, "int16_t", zero_cc_type_int16());
    zero_cc_register_typedef(c, "uint16_t", zero_cc_type_uint16());
    zero_cc_register_typedef(c, "unsigned short", zero_cc_type_uint16());
    zero_cc_register_typedef(c, "int", zero_cc_type_int());
    zero_cc_register_typedef(c, "int32_t", zero_cc_type_int());
    zero_cc_register_typedef(c, "uint32_t", zero_cc_type_uint32());
    zero_cc_register_typedef(c, "unsigned", zero_cc_type_uint32());
    zero_cc_register_typedef(c, "unsigned int", zero_cc_type_uint32());
    zero_cc_register_typedef(c, "long", zero_cc_type_int64());
    zero_cc_register_typedef(c, "int64_t", zero_cc_type_int64());
    zero_cc_register_typedef(c, "long long", zero_cc_type_int64());
    zero_cc_register_typedef(c, "uint64_t", zero_cc_type_uint64());
    zero_cc_register_typedef(c, "unsigned long", zero_cc_type_uint64());
    zero_cc_register_typedef(c, "unsigned long long", zero_cc_type_uint64());
    zero_cc_register_typedef(c, "size_t", zero_cc_type_uint64());
    zero_cc_register_typedef(c, "uintptr_t", zero_cc_type_uint64());
    zero_cc_register_typedef(c, "intptr_t", zero_cc_type_int64());
    zero_cc_register_typedef(c, "float", zero_cc_type_float());
    zero_cc_register_typedef(c, "double", zero_cc_type_double());
}


#endif /* ZERO_CC_TYPES_REGISTRY_H */
