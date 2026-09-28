#ifndef ZERO_CC_CORE_BUFFERS_H
#define ZERO_CC_CORE_BUFFERS_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>


/* --------------------------------------------------------------------------
 *  2. STEP 1: PREPROCESSOR (CPP) WITH DYNAMIC TPHT HASH INDEXING
 *  -------------------------------------------------------------------------- */
#define ZERO_CC_MAX_INCLUDE_PATHS  256
#define ZERO_CC_MAX_MACROS         32768
#define ZERO_CC_MAX_FUNCTIONS      16384
#define ZERO_CC_MAX_LOCALS         8192
#define ZERO_CC_MAX_TYPEDEFS       8192
#define ZERO_CC_MAX_STRUCTS        8192
#define ZERO_CC_MAX_ENUMS          16384
#define ZERO_CC_MAX_RELOCS         32768
#define ZERO_CC_MAX_STR_RELOCS     16384


/* High-speed FNV-1a 64-bit string hashing */
static inline uint64_t zero_cc_hash64(const char *str) {
    uint64_t h = 14695981039346656037ULL;
    while (*str) {
        h ^= (uint64_t)(unsigned char)*str++;
        h *= 1099511628211ULL;
    }
    return h;
}

typedef struct {
    char name[128];
    char *params[32];
    int param_count;     /* -1 = object macro, >= 0 = function macro */
    bool is_variadic;
    char *replacement;
} ZeroCCMacro;

typedef struct {
    char *paths[ZERO_CC_MAX_INCLUDE_PATHS];
    size_t count;
} ZeroCCIncludeDirs;

typedef struct {
    ZeroCCMacro *macros;
    size_t macro_count;
    size_t macro_capacity;
    int32_t hash_buckets[4096];
    int32_t *hash_next;
    ZeroCCIncludeDirs include_dirs;
    int cond_depth;
    bool cond_true[64];
    bool cond_seen_true[64];
} ZeroCCPreprocessor;

static inline void zero_cc_cpp_init(ZeroCCPreprocessor *cpp) {
    memset(cpp, 0, sizeof(*cpp));
    cpp->macro_capacity = 512;
    cpp->macros = (ZeroCCMacro *)calloc(cpp->macro_capacity, sizeof(ZeroCCMacro));
    cpp->hash_next = (int32_t *)malloc(cpp->macro_capacity * sizeof(int32_t));
    for (int i = 0; i < 4096; i++) cpp->hash_buckets[i] = -1;
    cpp->cond_depth = 0;
    cpp->cond_true[0] = true;
    cpp->cond_seen_true[0] = true;
}

static inline void zero_cc_cpp_destroy(ZeroCCPreprocessor *cpp) {
    if (!cpp) return;
    for (size_t i = 0; i < cpp->macro_count; i++) {
        if (cpp->macros[i].replacement) free(cpp->macros[i].replacement);
        for (int p = 0; p < cpp->macros[i].param_count; p++) {
            if (cpp->macros[i].params[p]) free(cpp->macros[i].params[p]);
        }
    }
    if (cpp->macros) free(cpp->macros);
    if (cpp->hash_next) free(cpp->hash_next);
    for (size_t i = 0; i < cpp->include_dirs.count; i++) {
        if (cpp->include_dirs.paths[i]) free(cpp->include_dirs.paths[i]);
    }
}

static inline void zero_cc_cpp_add_include_dir(ZeroCCPreprocessor *cpp, const char *path) {
    if (cpp->include_dirs.count < ZERO_CC_MAX_INCLUDE_PATHS && path) {
        cpp->include_dirs.paths[cpp->include_dirs.count++] = strdup(path);
    }
}

static inline ZeroCCMacro* zero_cc_cpp_find_macro(ZeroCCPreprocessor *cpp, const char *name) {
    if (!cpp || !name || cpp->macro_count == 0) return NULL;
    uint32_t b = (uint32_t)(zero_cc_hash64(name) & 4095);
    int32_t idx = cpp->hash_buckets[b];
    while (idx >= 0 && (size_t)idx < cpp->macro_count) {
        if (strcmp(cpp->macros[idx].name, name) == 0) {
            return &cpp->macros[idx];
        }
        idx = cpp->hash_next[idx];
    }
    return NULL;
}

static inline void zero_cc_cpp_define_obj(ZeroCCPreprocessor *cpp, const char *name, const char *value) {
    if (!cpp || !name) return;
    ZeroCCMacro *existing = zero_cc_cpp_find_macro(cpp, name);
    if (existing) {
        free(existing->replacement);
        existing->replacement = strdup(value ? value : "");
        return;
    }
    if (cpp->macro_count >= cpp->macro_capacity) {
        cpp->macro_capacity *= 2;
        cpp->macros = (ZeroCCMacro *)realloc(cpp->macros, cpp->macro_capacity * sizeof(ZeroCCMacro));
        cpp->hash_next = (int32_t *)realloc(cpp->hash_next, cpp->macro_capacity * sizeof(int32_t));
    }
    size_t idx = cpp->macro_count++;
    ZeroCCMacro *m = &cpp->macros[idx];
    memset(m, 0, sizeof(*m));
    snprintf(m->name, sizeof(m->name), "%s", name);
    m->param_count = -1;
    m->is_variadic = false;
    m->replacement = strdup(value ? value : "");

    uint32_t b = (uint32_t)(zero_cc_hash64(name) & 4095);
    cpp->hash_next[idx] = cpp->hash_buckets[b];
    cpp->hash_buckets[b] = (int32_t)idx;
}

static inline void zero_cc_cpp_define_func1(ZeroCCPreprocessor *cpp, const char *name, const char *p1, const char *value) {
    if (!cpp || !name) return;
    if (cpp->macro_count >= cpp->macro_capacity) {
        cpp->macro_capacity *= 2;
        cpp->macros = (ZeroCCMacro *)realloc(cpp->macros, cpp->macro_capacity * sizeof(ZeroCCMacro));
        cpp->hash_next = (int32_t *)realloc(cpp->hash_next, cpp->macro_capacity * sizeof(int32_t));
    }
    size_t idx = cpp->macro_count++;
    ZeroCCMacro *m = &cpp->macros[idx];
    memset(m, 0, sizeof(*m));
    snprintf(m->name, sizeof(m->name), "%s", name);
    m->param_count = 1;
    m->params[0] = strdup(p1 ? p1 : "arg");
    m->is_variadic = false;
    m->replacement = strdup(value ? value : "");
    uint32_t b = (uint32_t)(zero_cc_hash64(name) & 4095);
    cpp->hash_next[idx] = cpp->hash_buckets[b];
    cpp->hash_buckets[b] = (int32_t)idx;
}

static inline void zero_cc_cpp_define_func2(ZeroCCPreprocessor *cpp, const char *name, const char *p1, const char *p2, const char *value) {
    if (!cpp || !name) return;
    if (cpp->macro_count >= cpp->macro_capacity) {
        cpp->macro_capacity *= 2;
        cpp->macros = (ZeroCCMacro *)realloc(cpp->macros, cpp->macro_capacity * sizeof(ZeroCCMacro));
        cpp->hash_next = (int32_t *)realloc(cpp->hash_next, cpp->macro_capacity * sizeof(int32_t));
    }
    size_t idx = cpp->macro_count++;
    ZeroCCMacro *m = &cpp->macros[idx];
    memset(m, 0, sizeof(*m));
    snprintf(m->name, sizeof(m->name), "%s", name);
    m->param_count = 2;
    m->params[0] = strdup(p1 ? p1 : "a1");
    m->params[1] = strdup(p2 ? p2 : "a2");
    m->is_variadic = false;
    m->replacement = strdup(value ? value : "");
    uint32_t b = (uint32_t)(zero_cc_hash64(name) & 4095);
    cpp->hash_next[idx] = cpp->hash_buckets[b];
    cpp->hash_buckets[b] = (int32_t)idx;
}

static inline void zero_cc_cpp_define_func3(ZeroCCPreprocessor *cpp, const char *name, const char *p1, const char *p2, const char *p3, const char *value) {
    if (!cpp || !name) return;
    if (cpp->macro_count >= cpp->macro_capacity) {
        cpp->macro_capacity *= 2;
        cpp->macros = (ZeroCCMacro *)realloc(cpp->macros, cpp->macro_capacity * sizeof(ZeroCCMacro));
        cpp->hash_next = (int32_t *)realloc(cpp->hash_next, cpp->macro_capacity * sizeof(int32_t));
    }
    size_t idx = cpp->macro_count++;
    ZeroCCMacro *m = &cpp->macros[idx];
    memset(m, 0, sizeof(*m));
    snprintf(m->name, sizeof(m->name), "%s", name);
    m->param_count = 3;
    m->params[0] = strdup(p1 ? p1 : "a1");
    m->params[1] = strdup(p2 ? p2 : "a2");
    m->params[2] = strdup(p3 ? p3 : "a3");
    m->is_variadic = false;
    m->replacement = strdup(value ? value : "");
    uint32_t b = (uint32_t)(zero_cc_hash64(name) & 4095);
    cpp->hash_next[idx] = cpp->hash_buckets[b];
    cpp->hash_buckets[b] = (int32_t)idx;
}

static inline void zero_cc_cpp_define_builtin_macros(ZeroCCPreprocessor *cpp) {
    zero_cc_cpp_define_obj(cpp, "__STDC__", "1");
    zero_cc_cpp_define_obj(cpp, "__STDC_VERSION__", "199901L");
    zero_cc_cpp_define_obj(cpp, "__GNUC__", "4");
    zero_cc_cpp_define_obj(cpp, "__GNUC_MINOR__", "2");
    zero_cc_cpp_define_obj(cpp, "__linux__", "1");
    zero_cc_cpp_define_obj(cpp, "__unix__", "1");
    zero_cc_cpp_define_obj(cpp, "NULL", "((void*)0)");
    zero_cc_cpp_define_obj(cpp, "true", "1");
    zero_cc_cpp_define_obj(cpp, "false", "0");
    zero_cc_cpp_define_obj(cpp, "va_list", "char*");
    /* Self-hosted atomic builtins for Milestone 3 */
    zero_cc_cpp_define_func2(cpp, "__atomic_load_n", "p", "m", "(0)" );
    zero_cc_cpp_define_func3(cpp, "__atomic_store_n", "p", "v", "m", "((void)0)" );
    zero_cc_cpp_define_func3(cpp, "__atomic_add_fetch", "p", "v", "m", "(0)" );
    zero_cc_cpp_define_obj(cpp, "__ATOMIC_SEQ_CST", "5");
    zero_cc_cpp_define_obj(cpp, "__ATOMIC_RELAXED", "0");
    zero_cc_cpp_define_obj(cpp, "__ATOMIC_ACQUIRE", "2");
    zero_cc_cpp_define_obj(cpp, "__ATOMIC_RELEASE", "3");
    zero_cc_cpp_define_obj(cpp, "__ATOMIC_ACQ_REL", "4");
    zero_cc_cpp_define_func2(cpp, "__builtin_va_start", "ap", "last", "(ap = (char*)(&(last)) - 8)");
    zero_cc_cpp_define_func2(cpp, "__builtin_va_arg", "ap", "type", "(*(type*)(ap -= 8, ap + 8))");
    zero_cc_cpp_define_func2(cpp, "va_start", "ap", "last", "(ap = (char*)(&(last)) - 8)");
    zero_cc_cpp_define_func2(cpp, "va_arg", "ap", "type", "(*(type*)(ap -= 8, ap + 8))");
    zero_cc_cpp_define_func1(cpp, "va_end", "ap", "((void)0)");
    zero_cc_cpp_define_func1(cpp, "__attribute__", "x", "");
    zero_cc_cpp_define_func1(cpp, "__attribute", "x", "");
    zero_cc_cpp_define_obj(cpp, "__extension__", "");
    zero_cc_cpp_define_obj(cpp, "__restrict", "");
    zero_cc_cpp_define_obj(cpp, "__restrict__", "");
    zero_cc_cpp_define_obj(cpp, "__inline__", "inline");
    zero_cc_cpp_define_obj(cpp, "__inline", "inline");
    zero_cc_cpp_define_obj(cpp, "__volatile__", "");
    zero_cc_cpp_define_obj(cpp, "volatile", "");
    zero_cc_cpp_define_func3(cpp, "__builtin_memcpy", "dest", "src", "n", "memcpy(dest, src, n)");
    zero_cc_cpp_define_func1(cpp, "__builtin_memset", "s", "memset(s, 0, 0)");
#if defined(ZERO_CC_TARGET_X86_64)
    zero_cc_cpp_define_obj(cpp, "__x86_64__", "1");
    zero_cc_cpp_define_obj(cpp, "__LP64__", "1");
#elif defined(ZERO_CC_TARGET_ARM64)
    zero_cc_cpp_define_obj(cpp, "__aarch64__", "1");
    zero_cc_cpp_define_obj(cpp, "__LP64__", "1");
#endif
}

#endif /* ZERO_CC_CORE_BUFFERS_H */
