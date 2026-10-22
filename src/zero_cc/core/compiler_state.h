#ifndef ZERO_CC_CORE_COMPILER_STATE_H
#define ZERO_CC_CORE_COMPILER_STATE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#define ZERO_CC_MAX_INCLUDE_PATHS  256
#define ZERO_CC_MAX_MACROS         32768
#define ZERO_CC_MAX_FUNCTIONS      16384
#define ZERO_CC_MAX_LOCALS         8192
#define ZERO_CC_MAX_TYPEDEFS       8192
#define ZERO_CC_MAX_STRUCTS        8192
#define ZERO_CC_MAX_ENUMS          16384
#define ZERO_CC_MAX_ENUM_DEFS      8192
#define ZERO_CC_MAX_RELOCS         32768
#define ZERO_CC_MAX_STR_RELOCS     16384
#define ZERO_CC_MAX_DATA_RELOCS    16384
#define ONCE_FILES_MAX 64


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
    char *filename;
    size_t line_num;
    int expanding_macro_index; /* index of macro currently being expanded, -1 if none */
    char *once_files[ONCE_FILES_MAX];
    size_t once_file_count;

    /* Stack of the nearest REAL-file include directories (C99 6.10.2p2:
     * a quoted include is searched first in the directory of the file
     * containing the directive).  Virtual (built-in) headers have no
     * directory and leave the stack untouched. */
    char include_file_dirs[32][PATH_MAX];
    int include_dir_depth;

    /* First missing-include diagnostic (C99 6.10.2: a diagnostic is
     * required if the named file cannot be found).  Empty if every
     * include resolved; zeroed by zero_cc_cpp_init's memset. */
    char include_error[PATH_MAX + 96];

    /* Pragma state */
    int pack_stack[32];
    int pack_stack_depth;
    int default_pack;

    /* GCC diagnostic pragma state */
    struct {
        bool warning_as_error;
        char *disabled_warnings[64];
        int disabled_warning_count;
        int stack_depth;
        bool warning_as_error_stack[16];
        char *disabled_warnings_stack[16][64];
        int disabled_warning_count_stack[16];
    } diagnostic_state;
} ZeroCCPreprocessor;

#endif /* ZERO_CC_CORE_COMPILER_STATE_H */
