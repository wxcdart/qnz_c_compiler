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
} ZeroCCGlobalVar;

/* Global variable storage (M2: dynamic table - grows on demand, no fixed cap) */
#define ZERO_CC_GLOBALS_INITIAL_CAPACITY 64
#define ZERO_CC_BSS_THRESHOLD 256  /* Size above which zero-init globals go to .bss */

#ifdef ZERO_CC_GLOBAL_VARS_IMPL
ZeroCCGlobalVar *global_vars = NULL;
size_t global_var_capacity = 0;
size_t global_var_count = 0;
#else
extern ZeroCCGlobalVar *global_vars;
extern size_t global_var_capacity;
extern size_t global_var_count;
#endif

/* Reset global variable state at compilation start (table kept for reuse) */
static inline void reset_global_vars(void) {
    global_var_count = 0;
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
    return global_var_count - 1;
}

/* Find global variable by name, return pointer or NULL */
static inline ZeroCCGlobalVar *find_global_var(const char *name) {
    for (size_t i = 0; i < global_var_count; i++) {
        if (strcmp(global_vars[i].name, name) == 0) {
            return &global_vars[i];
        }
    }
    return NULL;
}

#endif /* ZERO_CC_TYPES_GLOBAL_VARS_H */
