#ifndef ZERO_CC_TYPES_GLOBAL_VARS_H
#define ZERO_CC_TYPES_GLOBAL_VARS_H

#include <stdlib.h>
#include <string.h>

/* Global Variable Declaration */
typedef struct {
    char name[128];
    ZeroCCType type;
    size_t data_offset;       /* Offset in data_section (JIT/TLS: TLS block offset) */
    size_t size;              /* Size in bytes */
    bool is_static;           /* Internal linkage */
    bool has_initializer;     /* Has = expression */
    bool is_tls;              /* _Thread_local (static TLS) */
    bool is_bss;              /* Zero-initialized, no explicit initializer -> .bss */
    char alias_target[128];   /* GNU __attribute__((alias("tgt"))): storage lives at tgt */
} ZeroCCGlobalVar;

/* Global variable storage (M2: dynamic table - grows on demand, no fixed cap) */
#define ZERO_CC_GLOBALS_INITIAL_CAPACITY 64
#define ZERO_CC_BSS_THRESHOLD 256  /* Size above which zero-init globals go to .bss */

#ifdef ZERO_CC_GLOBAL_VARS_IMPL
ZeroCCGlobalVar *global_vars = NULL;
size_t global_var_capacity = 0;
size_t global_var_count = 0;
size_t g_name_hash_cap = 0;
size_t *g_name_hash = NULL;
uint32_t *g_name_hash_sig = NULL;
#else
extern ZeroCCGlobalVar *global_vars;
extern size_t global_var_capacity;
extern size_t global_var_count;
extern size_t g_name_hash_cap;
extern size_t *g_name_hash;
extern uint32_t *g_name_hash_sig;
#endif

/* Name -> index hash (FNV-1a, open addressing).  Without it, add_global_var's
 * dedup and find_global_var both do O(N) strcmp scans - O(N^2) for the
 * 100k-global limits tests (compile/limits-externalid.c hung). */
static inline uint32_t zero_cc_gname_hash_str(const char *s) {
    uint32_t h = 2166136261u;
    while (*s) { h ^= (unsigned char)*s; h *= 16777619u; s++; }
    return h;
}
static inline void zero_cc_gname_hash_reset(void) {
    if (g_name_hash) { free(g_name_hash); g_name_hash = NULL; }
    if (g_name_hash_sig) { free(g_name_hash_sig); g_name_hash_sig = NULL; }
    g_name_hash_cap = 0;
}
static inline void zero_cc_gname_hash_insert(const char *name, size_t idx, size_t count) {
    uint32_t h = zero_cc_gname_hash_str(name);
    if (g_name_hash_cap == 0 || count * 10 > g_name_hash_cap * 7) {
        size_t ncap = g_name_hash_cap ? g_name_hash_cap * 2 : 1024;
        while (count * 10 > ncap * 7) ncap *= 2;
        size_t *nh = (size_t *)calloc(ncap, sizeof(size_t));
        uint32_t *ns = (uint32_t *)calloc(ncap, sizeof(uint32_t));
        if (!nh || !ns) { free(nh); free(ns); return; }
        for (size_t i = 0; i < g_name_hash_cap; i++) {
            if (g_name_hash[i]) {
                size_t ti = g_name_hash[i] - 1;
                uint32_t th = zero_cc_gname_hash_str(global_vars[ti].name);
                size_t t = th % ncap;
                while (nh[t]) t = (t + 1) % ncap;
                nh[t] = g_name_hash[i];
                ns[t] = th;
            }
        }
        free(g_name_hash);
        free(g_name_hash_sig);
        g_name_hash = nh;
        g_name_hash_sig = ns;
        g_name_hash_cap = ncap;
    }
    size_t t = h % g_name_hash_cap;
    while (g_name_hash[t]) t = (t + 1) % g_name_hash_cap;
    g_name_hash[t] = idx + 1;
    g_name_hash_sig[t] = h;
}
static inline size_t zero_cc_gname_hash_find(const char *name) {
    if (g_name_hash_cap == 0 || !g_name_hash) return (size_t)-1;
    uint32_t h = zero_cc_gname_hash_str(name);
    size_t t = h % g_name_hash_cap;
    while (g_name_hash[t]) {
        if (g_name_hash_sig[t] == h &&
            strcmp(global_vars[g_name_hash[t] - 1].name, name) == 0) {
            return g_name_hash[t] - 1;
        }
        t = (t + 1) % g_name_hash_cap;
    }
    return (size_t)-1;
}

/* Reset global variable state at compilation start (table kept for reuse) */
static inline void reset_global_vars(void) {
    global_var_count = 0;
    zero_cc_gname_hash_reset();
}

/* Ensure capacity for `global_var_count + extra` entries (multi-TU driver
 * re-appends snapshot entries after each per-TU compile wipe). */
static inline int zero_cc_global_vars_reserve(size_t extra) {
    if (global_var_count + extra <= global_var_capacity) return 0;
    size_t new_cap = global_var_capacity ? global_var_capacity : ZERO_CC_GLOBALS_INITIAL_CAPACITY;
    while (new_cap < global_var_count + extra) new_cap *= 2;
    ZeroCCGlobalVar *grown = (ZeroCCGlobalVar *)realloc(global_vars, new_cap * sizeof(ZeroCCGlobalVar));
    if (!grown) return -1;
    global_vars = grown;
    global_var_capacity = new_cap;
    return 0;
}

/* Add global variable, return index or -1 on allocation failure */
static int add_global_var(const char *name, ZeroCCType type, size_t offset, size_t size, bool is_static, bool has_init) __attribute__((unused));
static int add_global_var(const char *name, ZeroCCType type, size_t offset, size_t size, bool is_static, bool has_init) {
    /* A definition following a tentative/extern declaration of the same
     * name must UPDATE that entry, not create a duplicate: find_global_var
     * returns the first match, so a stale extern entry (size 0, offset 0)
     * would shadow the real storage and `*aa` would dereference address 0
     * (c-torture execute/20000403-1). */
    size_t existing = zero_cc_gname_hash_find(name);
    if (existing != (size_t)-1) {
        global_vars[existing].type = type;
        global_vars[existing].data_offset = offset;
        global_vars[existing].size = size;
        global_vars[existing].is_static = is_static;
        global_vars[existing].has_initializer = has_init;
        global_vars[existing].is_bss = (!has_init && size >= ZERO_CC_BSS_THRESHOLD);
        return (int)existing;
    }
    if (global_var_count >= global_var_capacity) {
        size_t new_cap = global_var_capacity ? global_var_capacity * 2 : ZERO_CC_GLOBALS_INITIAL_CAPACITY;
        ZeroCCGlobalVar *grown = (ZeroCCGlobalVar *)realloc(global_vars, new_cap * sizeof(ZeroCCGlobalVar));
        if (!grown) return -1; /* allocation failure only */
        global_vars = grown;
        global_var_capacity = new_cap;
    }
    ZeroCCGlobalVar *gv = &global_vars[global_var_count++];
    strncpy(gv->name, name, sizeof(gv->name) - 1);
    gv->name[sizeof(gv->name) - 1] = '\0';
    gv->type = type;
    gv->data_offset = offset;
    gv->size = size;
    gv->is_static = is_static;
    gv->has_initializer = has_init;
    gv->is_tls = false;
    gv->is_bss = (!has_init && size >= ZERO_CC_BSS_THRESHOLD);
    gv->alias_target[0] = '\0';
    zero_cc_gname_hash_insert(name, global_var_count - 1, global_var_count);
    return global_var_count - 1;
}

/* Find global variable by name, return pointer or NULL.  GNU
 * __attribute__((alias("tgt"))) entries are resolved transparently to the
 * target's entry so every use of the alias name (value load/store,
 * address-of, data relocs) hits the target's storage and symbol.  The
 * target must already be registered (file-scope globals are all
 * registered before function bodies lower); if it is not yet, the alias's
 * own entry is returned as a fallback. */
static inline ZeroCCGlobalVar *find_global_var(const char *name) {
    size_t hit = zero_cc_gname_hash_find(name);
    if (hit != (size_t)-1) {
        ZeroCCGlobalVar *gv = &global_vars[hit];
        int guard = 0;
        while (gv->alias_target[0] && guard++ < 8) {
            size_t t = zero_cc_gname_hash_find(gv->alias_target);
            if (t == (size_t)-1) break;
            gv = &global_vars[t];
        }
        return gv;
    }
    return NULL;
}

#endif /* ZERO_CC_TYPES_GLOBAL_VARS_H */
