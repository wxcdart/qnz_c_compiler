#ifndef ZERO_CC_CORE_COMPILER_STATE_H
#define ZERO_CC_CORE_COMPILER_STATE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

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

#endif /* ZERO_CC_CORE_COMPILER_STATE_H */
