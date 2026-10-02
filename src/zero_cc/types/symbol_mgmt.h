#ifndef ZERO_CC_TYPES_SYMBOL_MGMT_H
#define ZERO_CC_TYPES_SYMBOL_MGMT_H

#include <stdbool.h>
#include <dlfcn.h>

/* Symbol & Scope Management */
static inline ZeroCCSym* zero_cc_find_local(ZeroCCCompiler *c, const char *name) {
    for (size_t i = 0; i < c->local_count; i++) {
        if (strcmp(c->locals[i].name, name) == 0) {
            return &c->locals[i];
        }
    }
    return NULL;
}

/* Find a thread‑local variable by name. Returns the opaque handle used by the TLS backend, or NULL if not found. */
static inline void *zero_cc_find_tls(ZeroCCCompiler *c, const char *name) {
    for (size_t i = 0; i < c->thread_local_count; i++) {
        if (strcmp(c->thread_locals[i].name, name) == 0) {
            return &c->thread_locals[i];
        }
    }
    return NULL;
}

static inline ZeroCCSym* zero_cc_add_local(ZeroCCCompiler *c, const char *name, ZeroCCType type) {
    if (c->local_count >= ZERO_CC_MAX_LOCALS) return NULL;
    ZeroCCSym *sym = &c->locals[c->local_count++];
    snprintf(sym->name, sizeof(sym->name), "%s", name);
    sym->type = type;
    size_t alignment = type.align ? type.align : 8;
    if (alignment < 8) alignment = 8;
    if (decl_align_spec > 0) {
        if ((size_t)decl_align_spec > alignment) alignment = (size_t)decl_align_spec;
    }
    size_t alloc_sz = type.size ? type.size : alignment;
    if (alloc_sz < 8) alloc_sz = 8;
    c->current_stack_frame = (c->current_stack_frame + alignment - 1) & ~(alignment - 1);
    c->current_stack_frame += (int32_t)alloc_sz;
    sym->stack_offset = c->current_stack_frame;
    sym->is_param = false;
    sym->param_reg_idx = -1;
    sym->is_vla = type.is_vla;
    sym->is_atomic = type.is_atomic;
    sym->is_const = type.is_const;
    sym->is_volatile = type.is_volatile;
    decl_align_spec = 0;
    return sym;
}

static inline ZeroCCFunction* zero_cc_find_function(ZeroCCCompiler *c, const char *name) {
    for (size_t i = 0; i < c->function_count; i++) {
        if (strcmp(c->functions[i].name, name) == 0) {
            return &c->functions[i];
        }
    }
    return NULL;
}

/* Dynamic Symbol Lookup for JIT Mode */
static inline void* zero_cc_resolve_symbol(const char *name) {
#ifdef __linux__
    if (strcmp(name, "stderr") == 0) return (void*)stderr;
    if (strcmp(name, "stdout") == 0) return (void*)stdout;
    if (strcmp(name, "stdin") == 0) return (void*)stdin;
    void *sym = dlsym(RTLD_DEFAULT, name);
    return sym;
#else
    (void)name;
    return NULL;
#endif
}

static inline int zero_cc_types_compatible(ZeroCCType a, ZeroCCType b) {
    a = zero_cc_type_strip_qualifiers(a);
    b = zero_cc_type_strip_qualifiers(b);
    if (a.tag != b.tag) return 0;
    switch (a.tag) {
        case TYPE_VOID:
        case TYPE_BOOL:
        case TYPE_INT8:
        case TYPE_UINT8:
        case TYPE_INT16:
        case TYPE_UINT16:
        case TYPE_INT32:
        case TYPE_UINT32:
        case TYPE_INT64:
        case TYPE_UINT64:
        case TYPE_FLOAT:
        case TYPE_DOUBLE:
        case TYPE_NULLPTR:
            return 1;
        case TYPE_PTR:
            /* If either base pointer is NULL or TYPE_VOID, treat as compatible with any object pointer */
            if (!a.base || !b.base || a.base->tag == TYPE_VOID || b.base->tag == TYPE_VOID) {
                return 1;
            }
            return zero_cc_types_compatible(*a.base, *b.base);
        case TYPE_ARRAY:
            if (!a.base || !b.base) return 1;
            return zero_cc_types_compatible(*a.base, *b.base) && a.array_len == b.array_len;
        case TYPE_STRUCT:
        case TYPE_UNION:
            return strcmp(a.struct_name, b.struct_name) == 0;
        default:
            return 0;
    }
}



#endif /* ZERO_CC_TYPES_SYMBOL_MGMT_H */
