#ifndef ZERO_CC_TYPES_REGISTRY_H
#define ZERO_CC_TYPES_REGISTRY_H

#include <stdio.h>

/* Struct Registry */
static inline void zero_cc_register_struct(ZeroCCCompiler *c, const char *name, ZeroCCType type) {
    for (size_t i = 0; i < c->struct_count; i++) {
        if (strcmp(c->structs[i].name, name) == 0) {
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

/* Typedef Registry */
static inline void zero_cc_register_typedef(ZeroCCCompiler *c, const char *alias, ZeroCCType type) {
    for (size_t i = 0; i < c->typedef_count; i++) {
        if (strcmp(c->typedefs[i].alias, alias) == 0) {
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

/* Enum Constant Registry */
static inline void zero_cc_register_enum_const(ZeroCCCompiler *c, const char *name, int64_t val) {
    for (size_t i = 0; i < c->enum_const_count; i++) {
        if (strcmp(c->enum_constants[i].name, name) == 0) {
            c->enum_constants[i].value = val;
            return;
        }
    }
    if (c->enum_const_count < ZERO_CC_MAX_ENUMS) {
        snprintf(c->enum_constants[c->enum_const_count].name, 128, "%s", name);
        c->enum_constants[c->enum_const_count].value = val;
        c->enum_const_count++;
    }
}

static inline ZeroCCEnumConst* zero_cc_find_enum_const(ZeroCCCompiler *c, const char *name) {
    for (size_t i = 0; i < c->enum_const_count; i++) {
        if (strcmp(c->enum_constants[i].name, name) == 0) {
            return &c->enum_constants[i];
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
