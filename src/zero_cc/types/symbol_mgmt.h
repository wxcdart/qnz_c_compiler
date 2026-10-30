#ifndef ZERO_CC_TYPES_SYMBOL_MGMT_H
#define ZERO_CC_TYPES_SYMBOL_MGMT_H

#include <stdbool.h>
#include <string.h>
#include <dlfcn.h>

/* High-speed FNV-1a 32-bit string hashing for local symbols */
static inline uint32_t zero_cc_local_hash(const char *str) {
    uint32_t h = 2166136261U;
    if (!str) return h;
    while (*str) {
        h ^= (uint8_t)(*str++);
        h *= 16777619U;
    }
    return h;
}

/* Symbol & Scope Management: O(1) hashed lookup */
/* Find a local by name, honoring the current block-scope bound
 * (c->local_lookup_bound): symbols at index >= bound were declared in a
 * later sibling block and must not shadow this scope's references.
 * bound < 0 means unbounded (see local_lookup_bound in symbols.h). */
static inline ZeroCCSym* zero_cc_find_local(ZeroCCCompiler *c, const char *name) {
    if (!c || !name || !c->locals || c->local_count == 0) return NULL;
    size_t bound = (c->local_lookup_bound >= 0) ? (size_t)c->local_lookup_bound : c->local_count;
    if (bound > c->local_count) bound = c->local_count;
    if (c->local_hash_buckets && c->local_hash_bucket_count > 0 && c->local_hash_next) {
        uint32_t h = zero_cc_local_hash(name);
        size_t b_idx = (size_t)(h & (c->local_hash_bucket_count - 1));
        int32_t idx = c->local_hash_buckets[b_idx];
        while (idx >= 0) {
            if ((size_t)idx < bound && strcmp(c->locals[idx].name, name) == 0) {
                return &c->locals[idx];
            }
            idx = c->local_hash_next[idx];
        }
        return NULL;
    }
    for (size_t i = bound; i > 0; i--) {
        if (strcmp(c->locals[i - 1].name, name) == 0) {
            return &c->locals[i - 1];
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

static inline void zero_cc_local_hash_rehash(ZeroCCCompiler *c, size_t new_bucket_count) {
    int32_t *new_buckets = (int32_t *)malloc(new_bucket_count * sizeof(int32_t));
    if (!new_buckets) return;
    for (size_t b = 0; b < new_bucket_count; b++) new_buckets[b] = -1;

    for (size_t i = 0; i < c->local_count; i++) {
        uint32_t h = zero_cc_local_hash(c->locals[i].name);
        size_t b_idx = (size_t)(h & (new_bucket_count - 1));
        c->local_hash_next[i] = new_buckets[b_idx];
        new_buckets[b_idx] = (int32_t)i;
    }

    if (c->local_hash_buckets) free(c->local_hash_buckets);
    c->local_hash_buckets = new_buckets;
    c->local_hash_bucket_count = new_bucket_count;
}

static inline ZeroCCSym* zero_cc_add_local(ZeroCCCompiler *c, const char *name, ZeroCCType type) {
    if (!c->locals || c->local_count >= c->local_capacity) {
        size_t new_cap = c->local_capacity ? c->local_capacity * 2 : 64;
        ZeroCCSym *new_locals = (ZeroCCSym *)realloc(c->locals, new_cap * sizeof(ZeroCCSym));
        if (!new_locals) return NULL;
        c->locals = new_locals;

        int32_t *new_next = (int32_t *)realloc(c->local_hash_next, new_cap * sizeof(int32_t));
        if (!new_next) return NULL;
        c->local_hash_next = new_next;
        c->local_capacity = new_cap;
    }

    /* Initialize or grow hash bucket table if needed */
    if (!c->local_hash_buckets || c->local_hash_bucket_count == 0) {
        c->local_hash_bucket_count = 64;
        c->local_hash_buckets = (int32_t *)malloc(c->local_hash_bucket_count * sizeof(int32_t));
        if (c->local_hash_buckets) {
            for (size_t b = 0; b < c->local_hash_bucket_count; b++) c->local_hash_buckets[b] = -1;
        }
    } else if (c->local_count >= (c->local_hash_bucket_count * 3) / 4) {
        zero_cc_local_hash_rehash(c, c->local_hash_bucket_count * 2);
    }

    size_t new_idx = c->local_count++;
    ZeroCCSym *sym = &c->locals[new_idx];
    /* Clear the whole slot: local_count is reset per function but the slot
     * array is persistent, so a fresh local can otherwise inherit stale
     * fields (e.g. is_static_local) from the previous function's local at
     * the same index. */
    memset(sym, 0, sizeof(*sym));
    snprintf(sym->name, sizeof(sym->name), "%s", name);
    sym->type = type;

    /* Link into hash table bucket */
    if (c->local_hash_buckets && c->local_hash_next) {
        uint32_t h = zero_cc_local_hash(name);
        size_t b_idx = (size_t)(h & (c->local_hash_bucket_count - 1));
        c->local_hash_next[new_idx] = c->local_hash_buckets[b_idx];
        c->local_hash_buckets[b_idx] = (int32_t)new_idx;
    }

    size_t alignment = type.align ? type.align : 8;
    if (alignment < 8) alignment = 8;
    if (decl_align_spec > 0) {
        if ((size_t)decl_align_spec > alignment) alignment = (size_t)decl_align_spec;
    }
    size_t alloc_sz = type.size ? type.size : alignment;
    c->current_stack_frame += (int32_t)alloc_sz;
    c->current_stack_frame = (c->current_stack_frame + (int32_t)alignment - 1) & ~((int32_t)alignment - 1);
    sym->stack_offset = c->current_stack_frame;
    sym->is_param = false;
    sym->param_reg_idx = -1;
    sym->is_vla = type.is_vla;
    sym->is_atomic = type.is_atomic;
    sym->is_const = type.is_const;
    sym->is_volatile = type.is_volatile;
    sym->is_restrict = type.is_restrict;
    sym->is_promoted = false;
    sym->promoted_vreg = -1;
    decl_align_spec = 0;
    return sym;
}

static inline Arena *zero_cc_get_fn_arena(ZeroCCCompiler *c) {
    if (!c->fn_arena) {
        c->fn_arena = arena_create(4096);
    }
    return c->fn_arena;
}

static inline ZeroCCFunction* zero_cc_find_function(ZeroCCCompiler *c, const char *name) {
    /* Prefer a DEFINITION over an earlier prototype/extern entry with the
     * same name (Sprint 41).  A function declared before its body (the
     * universal header pattern) registers two entries; the first-match
     * return bound call relocations and function pointers to the prototype
     * stub (text_offset 0, is_prototype), which the JIT then resolved via
     * dlsym() - a wrong target for internal functions. */
    ZeroCCFunction *proto = NULL;
    for (size_t i = 0; i < c->function_count; i++) {
        if (strcmp(c->functions[i].name, name) == 0) {
            if (c->functions[i].is_prototype) {
                if (!proto) proto = &c->functions[i];
            } else {
                return &c->functions[i];
            }
        }
    }
    return proto;
}

/* Dynamic Symbol Lookup for JIT Mode */
static inline void* zero_cc_resolve_symbol(const char *name) {
#ifdef __linux__
    if (strcmp(name, "stderr") == 0) return (void*)&stderr;
    if (strcmp(name, "stdout") == 0) return (void*)&stdout;
    if (strcmp(name, "stdin") == 0) return (void*)&stdin;
    if (strcmp(name, "link_error") == 0) {
        extern void abort(void);
        return (void*)&abort;
    }
    void *sym = dlsym(RTLD_DEFAULT, name);
    if (!sym) {
        static void *libm_handle = NULL;
        static int libm_tried = 0;
        if (!libm_tried) {
            libm_tried = 1;
            libm_handle = dlopen("libm.so.6", RTLD_LAZY | RTLD_GLOBAL);
        }
        if (libm_handle) {
            sym = dlsym(libm_handle, name);
        }
    }
    return sym;
#else
    (void)name;
    return NULL;
#endif
}

/* Type compatibility for _Generic selection (C11 6.5.1.1, 6.2.7).
 * The controlling expression type (first arg) has already undergone lvalue
 * conversion which strips top-level qualifiers. The association type (second
 * arg) retains its qualifiers. Two types are compatible if they match per
 * 6.2.7 - qualifiers must match identically for qualified types. */
static inline int zero_cc_types_compatible(ZeroCCType a, ZeroCCType b) {
    /* Strip qualifiers only from controlling type (a), not association (b) */
    a = zero_cc_type_strip_qualifiers(a);
    if (a.tag != b.tag) return 0;
    /* For qualified types, qualifiers must match identically (6.2.7p3).
     * Since a has no top-level qualifiers after stripping, b must also have none. */
    if (b.is_const || b.is_volatile || b.is_atomic) return 0;
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
            /* Pointer types compatible only if pointee types compatible */
            if (!a.base || !b.base) return 0;
            return zero_cc_types_compatible(*a.base, *b.base);
        case TYPE_ARRAY:
            if (!a.base || !b.base) return 0;
            return zero_cc_types_compatible(*a.base, *b.base) && a.array_len == b.array_len;
        case TYPE_USER_0:  /* vector: same width + compatible elements */
            if (!a.base || !b.base) return 0;
            return a.size == b.size && zero_cc_types_compatible(*a.base, *b.base);
        case TYPE_STRUCT:
        case TYPE_UNION:
            return strcmp(a.struct_name, b.struct_name) == 0;
        default:
            return 0;
    }
}



#endif /* ZERO_CC_TYPES_SYMBOL_MGMT_H */
