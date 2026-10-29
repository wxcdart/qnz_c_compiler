#ifndef ZERO_CC_CORE_PREPROCESSOR_H
#define ZERO_CC_CORE_PREPROCESSOR_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

/* Global pragma state - defined in pragma_state.c */
extern int zero_cc_global_pack;
extern struct {
    bool warning_as_error;
    char *disabled_warnings[64];
    int disabled_warning_count;
} zero_cc_global_diag_state;

/* Forward declaration: this header's functions take `ZeroCCCompiler *`
 * (member access like c->diag_buf) but are typically included before
 * zero_cc/types/symbols.h defines the struct.  The forward typedef makes
 * the parameter type a pointer-to-incomplete-struct instead of an
 * unknown-identifier fallback; semantic analysis resolves members against
 * the completed registry. */
typedef struct ZeroCCCompiler ZeroCCCompiler;

/* Get the pack value after preprocessing */
static inline int zero_cc_get_pack_value(void) {
    return zero_cc_global_pack;
}

/* Set the pack value (for use by tokenizer) */
static inline void zero_cc_set_pack_value(int pack) {
    zero_cc_global_pack = pack;
}

#include "zero_cc/lexer/scan.h"

static inline void zero_cc_cpp_init(ZeroCCPreprocessor *cpp) {
    memset(cpp, 0, sizeof(*cpp));
    cpp->macro_capacity = 512;
    cpp->macros = (ZeroCCMacro *)calloc(cpp->macro_capacity, sizeof(ZeroCCMacro));
    cpp->hash_next = (int32_t *)malloc(cpp->macro_capacity * sizeof(int32_t));
    for (int i = 0; i < 4096; i++) cpp->hash_buckets[i] = -1;
    cpp->cond_depth = 0;
    cpp->cond_true[0] = true;
    cpp->cond_seen_true[0] = true;
    cpp->expanding_macro_index = -1;
    cpp->default_pack = 0;
    cpp->pack_stack_depth = 0;
    cpp->diagnostic_state.warning_as_error = false;
    cpp->diagnostic_state.disabled_warning_count = 0;
    cpp->diagnostic_state.stack_depth = 0;
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
    for (size_t i = 0; i < cpp->once_file_count; i++) {
        if (cpp->once_files[i]) free(cpp->once_files[i]);
    }
    /* Free pragma state */
    for (int d = 0; d < cpp->pack_stack_depth; d++) {
        /* pack stack doesn't need cleanup */
    }
    for (int i = 0; i < cpp->diagnostic_state.disabled_warning_count; i++) {
        if (cpp->diagnostic_state.disabled_warnings[i]) free(cpp->diagnostic_state.disabled_warnings[i]);
    }
    for (int d = 0; d < cpp->diagnostic_state.stack_depth; d++) {
        for (int i = 0; i < cpp->diagnostic_state.disabled_warning_count_stack[d]; i++) {
            if (cpp->diagnostic_state.disabled_warnings_stack[d][i]) free(cpp->diagnostic_state.disabled_warnings_stack[d][i]);
        }
    }
    for (int d = 0; d < cpp->macro_stack_depth; d++) {
        if (cpp->macro_stack[d].is_defined) {
            if (cpp->macro_stack[d].macro.replacement) free(cpp->macro_stack[d].macro.replacement);
            for (int p = 0; p < cpp->macro_stack[d].macro.param_count; p++) {
                if (cpp->macro_stack[d].macro.params[p]) free(cpp->macro_stack[d].macro.params[p]);
            }
        }
    }
    for (size_t i = 0; i < cpp->dep_file_count; i++) {
        if (cpp->dep_files[i]) free(cpp->dep_files[i]);
    }
    if (cpp->dep_files) free(cpp->dep_files);
    if (cpp->dep_target) free(cpp->dep_target);
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
            if (cpp->expanding_macro_index == idx) {
                /* Inhibit expansion of the macro currently being expanded */
                return NULL;
            }
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

/* Remove a macro (command-line -U).  The hash chains hold indices into
 * the macros array, so after compacting the array the whole table is
 * rebuilt.  Only valid before text processing starts (an in-flight
 * expansion may hold an index into the old layout). */
static inline void zero_cc_cpp_undefine(ZeroCCPreprocessor *cpp, const char *name) {
    if (!cpp || !name || !name[0]) return;
    for (size_t i = 0; i < cpp->macro_count; i++) {
        if (strcmp(cpp->macros[i].name, name) == 0) {
            if (cpp->macros[i].replacement) free(cpp->macros[i].replacement);
            for (int p = 0; p < cpp->macros[i].param_count; p++) {
                if (cpp->macros[i].params[p]) free(cpp->macros[i].params[p]);
            }
            memmove(&cpp->macros[i], &cpp->macros[i + 1],
                    (cpp->macro_count - i - 1) * sizeof(cpp->macros[0]));
            cpp->macro_count--;
            for (int bkt = 0; bkt < 4096; bkt++) cpp->hash_buckets[bkt] = -1;
            for (size_t j = 0; j < cpp->macro_count; j++) {
                uint32_t b = (uint32_t)(zero_cc_hash64(cpp->macros[j].name) & 4095);
                cpp->hash_next[j] = cpp->hash_buckets[b];
                cpp->hash_buckets[b] = (int32_t)j;
            }
            return;
        }
    }
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
    /* C11 (201112L): ZeroCC accepts the full C11 keyword set (_Generic,
     * _Static_assert, _Alignas, _Noreturn, _Atomic, _Thread_local, anonymous
     * struct/union members, compound literals, <stdatomic.h>/<threads.h>). */
    zero_cc_cpp_define_obj(cpp, "__STDC_VERSION__", "201112L");
    zero_cc_cpp_define_obj(cpp, "__STDC_NO_COMPLEX__", "1");
    zero_cc_cpp_define_obj(cpp, "__zerocc__", "1");
    zero_cc_cpp_define_obj(cpp, "__ZEROCC__", "1");
    zero_cc_cpp_define_obj(cpp, "__GNUC__", "4");
    zero_cc_cpp_define_obj(cpp, "__GNUC_MINOR__", "2");
    zero_cc_cpp_define_obj(cpp, "__linux__", "1");
    zero_cc_cpp_define_obj(cpp, "__unix__", "1");
    zero_cc_cpp_define_obj(cpp, "NULL", "((void*)0)");
    zero_cc_cpp_define_obj(cpp, "__INT_MAX__", "2147483647");
    /* Atomic memory orders */
    zero_cc_cpp_define_obj(cpp, "__ATOMIC_SEQ_CST", "5");
    zero_cc_cpp_define_obj(cpp, "__ATOMIC_RELAXED", "0");
    zero_cc_cpp_define_obj(cpp, "__ATOMIC_ACQUIRE", "2");
    zero_cc_cpp_define_obj(cpp, "__ATOMIC_RELEASE", "3");
    zero_cc_cpp_define_obj(cpp, "__ATOMIC_ACQ_REL", "4");
    zero_cc_cpp_define_func2(cpp, "__builtin_va_start", "ap", "last", "__builtin_va_start(ap, last)");
    zero_cc_cpp_define_func2(cpp, "__builtin_va_arg", "ap", "type", "__builtin_va_arg(ap, type)");
    zero_cc_cpp_define_func1(cpp, "__builtin_va_end", "ap", "((void)0)");
    zero_cc_cpp_define_func2(cpp, "__builtin_va_copy", "dest", "src", "((void)((dest) = (src)))");
    zero_cc_cpp_define_func2(cpp, "__va_copy", "dest", "src", "((void)((dest) = (src)))");
    zero_cc_cpp_define_func2(cpp, "va_start", "ap", "last", "__builtin_va_start(ap, last)");
    zero_cc_cpp_define_func2(cpp, "va_arg", "ap", "type", "__builtin_va_arg(ap, type)");
    zero_cc_cpp_define_func1(cpp, "va_end", "ap", "((void)0)");
    zero_cc_cpp_define_func2(cpp, "va_copy", "dest", "src", "((void)((dest) = (src)))");
    zero_cc_cpp_define_func1(cpp, "__attribute__", "x", "");
    zero_cc_cpp_define_func1(cpp, "__attribute", "x", "");
    zero_cc_cpp_define_obj(cpp, "__extension__", "");
    zero_cc_cpp_define_obj(cpp, "__restrict", "restrict");
    zero_cc_cpp_define_obj(cpp, "__restrict__", "restrict");
    zero_cc_cpp_define_obj(cpp, "__inline__", "inline");
    zero_cc_cpp_define_obj(cpp, "__inline", "inline");
    zero_cc_cpp_define_obj(cpp, "__volatile__", "volatile");
    zero_cc_cpp_define_obj(cpp, "__volatile", "volatile");
    zero_cc_cpp_define_obj(cpp, "__const", "const");
    zero_cc_cpp_define_obj(cpp, "__const__", "const");
#if defined(ZERO_CC_TARGET_X86_64)
    zero_cc_cpp_define_obj(cpp, "__x86_64__", "1");
    zero_cc_cpp_define_obj(cpp, "__LP64__", "1");
#elif defined(ZERO_CC_TARGET_ARM64)
    zero_cc_cpp_define_obj(cpp, "__aarch64__", "1");
    zero_cc_cpp_define_obj(cpp, "__LP64__", "1");
#endif
    zero_cc_cpp_define_obj(cpp, "__ORDER_LITTLE_ENDIAN__", "1234");
    zero_cc_cpp_define_obj(cpp, "__ORDER_BIG_ENDIAN__", "4321");
    zero_cc_cpp_define_obj(cpp, "__ORDER_PDP_ENDIAN__", "3412");
    zero_cc_cpp_define_obj(cpp, "__BYTE_ORDER__", "1234");
    zero_cc_cpp_define_obj(cpp, "__FLOAT_WORD_ORDER__", "1234");
    zero_cc_cpp_define_obj(cpp, "__SIZE_TYPE__", "unsigned long");
    zero_cc_cpp_define_obj(cpp, "__PTRDIFF_TYPE__", "long");
    zero_cc_cpp_define_obj(cpp, "__INTPTR_TYPE__", "long");
    zero_cc_cpp_define_obj(cpp, "__UINTPTR_TYPE__", "unsigned long");
    zero_cc_cpp_define_obj(cpp, "__INTMAX_TYPE__", "long");
    zero_cc_cpp_define_obj(cpp, "__UINTMAX_TYPE__", "unsigned long");
    zero_cc_cpp_define_obj(cpp, "__WCHAR_TYPE__", "int");
    zero_cc_cpp_define_obj(cpp, "__WCHAR_WIDTH__", "32");
    zero_cc_cpp_define_obj(cpp, "__SIZEOF_WCHAR_T__", "4");
    zero_cc_cpp_define_obj(cpp, "__WCHAR_MAX__", "2147483647");
    zero_cc_cpp_define_obj(cpp, "__WCHAR_MIN__", "(-2147483647 - 1)");
    zero_cc_cpp_define_obj(cpp, "__WINT_TYPE__", "unsigned int");
    zero_cc_cpp_define_obj(cpp, "__WINT_WIDTH__", "32");
    zero_cc_cpp_define_obj(cpp, "__SIZEOF_WINT_T__", "4");
    zero_cc_cpp_define_obj(cpp, "__INT8_TYPE__", "signed char");
    zero_cc_cpp_define_obj(cpp, "__INT16_TYPE__", "short");
    zero_cc_cpp_define_obj(cpp, "__INT32_TYPE__", "int");
    zero_cc_cpp_define_obj(cpp, "__INT64_TYPE__", "long");
    zero_cc_cpp_define_obj(cpp, "__UINT8_TYPE__", "unsigned char");
    zero_cc_cpp_define_obj(cpp, "__UINT16_TYPE__", "unsigned short");
    zero_cc_cpp_define_obj(cpp, "__UINT32_TYPE__", "unsigned int");
    zero_cc_cpp_define_obj(cpp, "__UINT64_TYPE__", "unsigned long");
}

static inline void zero_cc_cpp_undef(ZeroCCPreprocessor *cpp, const char *name) {
    if (!cpp || !name) return;
    ZeroCCMacro *m = zero_cc_cpp_find_macro(cpp, name);
    if (m) {
        if (m->replacement) { free(m->replacement); m->replacement = NULL; }
        for (int p = 0; p < m->param_count; p++) {
            if (m->params[p]) { free(m->params[p]); m->params[p] = NULL; }
        }
        m->name[0] = '\0';
    }
}

static inline void zero_cc_cpp_restore_macro(ZeroCCPreprocessor *cpp, const ZeroCCMacro *src) {
    if (!cpp || !src || !src->name[0]) return;
    if (cpp->macro_count >= cpp->macro_capacity) {
        cpp->macro_capacity *= 2;
        cpp->macros = (ZeroCCMacro *)realloc(cpp->macros, cpp->macro_capacity * sizeof(ZeroCCMacro));
        cpp->hash_next = (int32_t *)realloc(cpp->hash_next, cpp->macro_capacity * sizeof(int32_t));
    }
    size_t idx = cpp->macro_count++;
    ZeroCCMacro *m = &cpp->macros[idx];
    memset(m, 0, sizeof(*m));
    snprintf(m->name, sizeof(m->name), "%s", src->name);
    m->param_count = src->param_count;
    m->is_variadic = src->is_variadic;
    m->replacement = src->replacement ? strdup(src->replacement) : NULL;
    for (int p = 0; p < src->param_count && p < 32; p++) {
        m->params[p] = src->params[p] ? strdup(src->params[p]) : NULL;
    }
    uint32_t b = (uint32_t)(zero_cc_hash64(src->name) & 4095);
    cpp->hash_next[idx] = cpp->hash_buckets[b];
    cpp->hash_buckets[b] = (int32_t)idx;
}

static inline void zero_cc_cpp_push_macro(ZeroCCPreprocessor *cpp, const char *name) {
    if (!cpp || !name || cpp->macro_stack_depth >= 128) return;
    ZeroCCMacroStackEntry *entry = &cpp->macro_stack[cpp->macro_stack_depth++];
    memset(entry, 0, sizeof(*entry));
    snprintf(entry->name, sizeof(entry->name), "%s", name);
    ZeroCCMacro *m = zero_cc_cpp_find_macro(cpp, name);
    if (m) {
        entry->is_defined = true;
        snprintf(entry->macro.name, sizeof(entry->macro.name), "%s", m->name);
        entry->macro.param_count = m->param_count;
        entry->macro.is_variadic = m->is_variadic;
        entry->macro.replacement = m->replacement ? strdup(m->replacement) : NULL;
        for (int p = 0; p < m->param_count && p < 32; p++) {
            entry->macro.params[p] = m->params[p] ? strdup(m->params[p]) : NULL;
        }
    } else {
        entry->is_defined = false;
    }
}

static inline void zero_cc_cpp_pop_macro(ZeroCCPreprocessor *cpp, const char *name) {
    if (!cpp || !name || cpp->macro_stack_depth <= 0) return;
    int found_idx = -1;
    for (int i = cpp->macro_stack_depth - 1; i >= 0; i--) {
        if (strcmp(cpp->macro_stack[i].name, name) == 0) {
            found_idx = i;
            break;
        }
    }
    if (found_idx < 0) return;

    ZeroCCMacroStackEntry entry = cpp->macro_stack[found_idx];
    for (int i = found_idx; i < cpp->macro_stack_depth - 1; i++) {
        cpp->macro_stack[i] = cpp->macro_stack[i + 1];
    }
    cpp->macro_stack_depth--;

    zero_cc_cpp_undef(cpp, name);

    if (entry.is_defined) {
        zero_cc_cpp_restore_macro(cpp, &entry.macro);
        if (entry.macro.replacement) free(entry.macro.replacement);
        for (int p = 0; p < entry.macro.param_count; p++) {
            if (entry.macro.params[p]) free(entry.macro.params[p]);
        }
    }
}


static inline char* zero_cc_read_file(const char *filepath) {
    FILE *f = fopen(filepath, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc(sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t read_bytes = fread(buf, 1, sz, f);
    buf[read_bytes] = '\0';
    fclose(f);
    return buf;
}

static inline char* zero_cc_cpp_get_virtual_header(const char *name) {
    if (!name) return NULL;
    if (strcmp(name, "stddef.h") == 0) {
        return strdup(
            "#ifndef _STDDEF_H\n#define _STDDEF_H 1\n"
            "typedef unsigned long size_t;\n"
            "typedef long ptrdiff_t;\n"
            "typedef __WCHAR_TYPE__ wchar_t;\n"
            "#define NULL ((void*)0)\n"
            "#define offsetof(type, member) __builtin_offsetof(type, member)\n"
            "#endif\n"
        );
    }
    if (strcmp(name, "stdint.h") == 0) {
        return strdup(
            "#ifndef _STDINT_H\n#define _STDINT_H 1\n"
            "typedef signed char int8_t;\n"
            "typedef unsigned char uint8_t;\n"
            "typedef short int16_t;\n"
            "typedef unsigned short uint16_t;\n"
            "typedef int int32_t;\n"
            "typedef unsigned int uint32_t;\n"
            "typedef long int64_t;\n"
            "typedef unsigned long uint64_t;\n"
            "typedef long intptr_t;\n"
            "typedef unsigned long uintptr_t;\n"
            "#define INT8_MAX 127\n#define INT16_MAX 32767\n#define INT32_MAX 2147483647\n#define INT64_MAX 9223372036854775807LL\n"
            "#define UINT8_MAX 255\n#define UINT16_MAX 65535\n#define UINT32_MAX 4294967295U\n#define UINT64_MAX 18446744073709551615ULL\n"
            "#endif\n"
        );
    }
    if (strcmp(name, "stdbool.h") == 0) {
        return strdup(
            "#ifndef _STDBOOL_H\n#define _STDBOOL_H 1\n"
            "#define bool _Bool\n#define true 1\n#define false 0\n#define __bool_true_false_are_defined 1\n"
            "#endif\n"
        );
    }
    if (strcmp(name, "inttypes.h") == 0) {
        /* Freestanding inttypes: the stdint types plus PRI* formatting
         * macros (the only addition over stdint.h). */
        return strdup(
            "#ifndef _INTTYPES_H\n#define _INTTYPES_H 1\n"
            "#include <stdint.h>\n"
            "#define PRId8 \"hd\"\n#define PRId16 \"hd\"\n#define PRId32 \"d\"\n#define PRId64 \"ld\"\n"
            "#define PRIu8 \"hu\"\n#define PRIu16 \"hu\"\n#define PRIu32 \"u\"\n#define PRIu64 \"lu\"\n"
            "#define PRIx8 \"hx\"\n#define PRIx16 \"hx\"\n#define PRIx32 \"x\"\n#define PRIx64 \"lx\"\n"
            "#define PRIX8 \"hX\"\n#define PRIX16 \"hX\"\n#define PRIX32 \"X\"\n#define PRIX64 \"lX\"\n"
            "#define SCNd8 \"hd\"\n#define SCNd16 \"hd\"\n#define SCNd32 \"d\"\n#define SCNd64 \"ld\"\n"
            "#define SCNu8 \"hu\"\n#define SCNu16 \"hu\"\n#define SCNu32 \"u\"\n#define SCNu64 \"lu\"\n"
            "#define SCNx8 \"hx\"\n#define SCNx16 \"hx\"\n#define SCNx32 \"x\"\n#define SCNx64 \"lx\"\n"
            "#endif\n"
        );
    }
    if (strcmp(name, "stdlib.h") == 0) {
        return strdup(
            "#ifndef _STDLIB_H\n#define _STDLIB_H 1\n"
            "typedef unsigned long size_t;\n"
            "void* malloc(size_t size);\nvoid free(void* ptr);\nvoid* calloc(size_t nmemb, size_t size);\n"
            "void* realloc(void* ptr, size_t size);\nvoid exit(int status);\nint atexit(void (*func)(void));\nint system(const char *command);\n"
            "long strtoll(const char *nptr, char **endptr, int base);\nlong strtol(const char *nptr, char **endptr, int base);\n"
            "unsigned long strtoul(const char *nptr, char **endptr, int base);\ndouble strtod(const char *nptr, char **endptr);\n"
            "unsigned long long strtoull(const char *nptr, char **endptr, int base);\nlong long atoll(const char *nptr);\n"
            "double atof(const char *nptr);\nvoid abort(void);\n"
            "char* getenv(const char *name);\n"
            "void* qsort(void *base, size_t nmemb, size_t size, int (*compar)(const void *, const void *));\n"
            "void* bsearch(const void *key, const void *base, size_t nmemb, size_t size, int (*compar)(const void *, const void *));\n"
            "int atoi(const char *nptr);\nint abs(int j);\n"
            "#endif\n"
        );
    }
    if (strcmp(name, "string.h") == 0) {
        return strdup(
            "#ifndef _STRING_H\n#define _STRING_H 1\n"
            "typedef unsigned long size_t;\n"
            "void* memcpy(void *dest, const void *src, size_t n);\n"
            "void* memmove(void *dest, const void *src, size_t n);\n"
            "void* memset(void *s, int c, size_t n);\n"
            "int memcmp(const void *s1, const void *s2, size_t n);\n"
            "size_t strlen(const char *s);\n"
            "char* strcpy(char *dest, const char *src);\n"
            "char* strncpy(char *dest, const char *src, size_t n);\n"
            "char* strcat(char *dest, const char *src);\n"
            "char* strncat(char *dest, const char *src, size_t n);\n"
            "int strcmp(const char *s1, const char *s2);\n"
            "int strncmp(const char *s1, const char *s2, size_t n);\n"
            "char* strdup(const char *s);\n"
            "char* strchr(const char *s, int c);\n"
            "char* strrchr(const char *s, int c);\n"
            "char* strstr(const char *haystack, const char *needle);\n"
            "#endif\n"
        );
    }
    if (strcmp(name, "limits.h") == 0) {
        return strdup(
            "#ifndef _LIMITS_H\n#define _LIMITS_H 1\n"
            "#define PATH_MAX 4096\n"
            "#define NAME_MAX 255\n"
            "#define CHAR_BIT 8\n"
            "#define SCHAR_MIN (-128)\n#define SCHAR_MAX 127\n"
            "#define UCHAR_MAX 255\n"
            "#define CHAR_MIN 0\n#define CHAR_MAX 127\n"
            "#define SHRT_MIN (-32768)\n#define SHRT_MAX 32767\n"
            "#define USHRT_MAX 65535\n"
            "#define INT_MIN (-2147483647-1)\n#define INT_MAX 2147483647\n"
            "#define UINT_MAX 4294967295U\n"
            "#define LONG_MIN (-9223372036854775807L-1)\n#define LONG_MAX 9223372036854775807L\n"
            "#define ULONG_MAX 18446744073709551615UL\n"
            "#define LLONG_MIN (-9223372036854775807LL-1)\n#define LLONG_MAX 9223372036854775807LL\n"
            "#define ULLONG_MAX 18446744073709551615ULL\n"
            "#define SIZE_MAX ((size_t)-1)\n"
            "#define PTRDIFF_MIN (-9223372036854775807L-1)\n#define PTRDIFF_MAX 9223372036854775807L\n"
            "#define SIZE_T_MAX ((size_t)-1)\n"
            "#define INT8_MIN (-128)\n#define INT8_MAX 127\n"
            "#define UINT8_MAX 255\n"
            "#define INT16_MIN (-32768)\n#define INT16_MAX 32767\n"
            "#define UINT16_MAX 65535\n"
            "#define INT32_MIN (-2147483647-1)\n#define INT32_MAX 2147483647\n"
            "#define UINT32_MAX 4294967295U\n"
            "#define INT64_MIN (-9223372036854775807LL-1)\n#define INT64_MAX 9223372036854775807LL\n"
            "#define UINT64_MAX 18446744073709551615ULL\n"
            "#endif\n"
        );
    }
    if (strcmp(name, "assert.h") == 0) {
        /* Freestanding assert: evaluated in every build (no NDEBUG knob in
         * Phase 0) - a failed assertion calls abort(). */
        return strdup(
            "#ifndef _ASSERT_H\n#define _ASSERT_H 1\n"
            "#include <stdlib.h>\n"
            "#define assert(expr) ((expr) ? (void)0 : abort())\n"
            "#endif\n"
        );
    }
    if (strcmp(name, "errno.h") == 0) {
        return strdup(
            "#ifndef _ERRNO_H\n#define _ERRNO_H 1\n"
            "extern int errno;\n"
            "#define EPERM 1\n#define ENOENT 2\n#define EINTR 4\n#define EIO 5\n#define EBADF 9\n"
            "#define EAGAIN 11\n#define ENOMEM 12\n#define EACCES 13\n#define EEXIST 17\n"
            "#define EINVAL 22\n#define ENFILE 23\n#define EMFILE 24\n#define ENOSPC 28\n"
            "#define EPIPE 32\n#define EROFS 30\n#define ENOSYS 38\n#define ENOTDIR 20\n#define EISDIR 21\n"
            "#define ERANGE 34\n#define ENAMETOOLONG 36\n#define ETIMEDOUT 110\n#define ECONNREFUSED 111\n"
            "#endif\n"
        );
    }
    if (strcmp(name, "ctype.h") == 0) {
        return strdup(
            "#ifndef _CTYPE_H\n#define _CTYPE_H 1\n"
            "int isalnum(int c);\nint isalpha(int c);\nint isdigit(int c);\nint isxdigit(int c);\n"
            "int isspace(int c);\nint ispunct(int c);\nint isupper(int c);\nint islower(int c);\n"
            "int isprint(int c);\nint isgraph(int c);\nint iscntrl(int c);\n"
            "int tolower(int c);\nint toupper(int c);\n"
            "#endif\n"
        );
    }
    if (strcmp(name, "stdarg.h") == 0) {
        return strdup(
            "#ifndef _STDARG_H\n#define _STDARG_H 1\n"
            /* ABI-correct va_list (Sprint 49): the callee prologue spills all
             * argument registers into a 16-slot save area (6 integer regs,
             * then 8 FP regs) above the locals. va_start points the two
             * streams (gp/fp) just past the named parameters' slots; when a
             * stream is exhausted (ptr == end) reads continue from ovf, the
             * shared stack-overflow area (fp+16), in argument order. */
            "typedef struct { unsigned long long *gp; unsigned long long *gp_end; unsigned long long *fp; unsigned long long *fp_end; unsigned long long *ovf; } va_list;\n"
            "#define va_start(ap, last) __builtin_va_start(ap, last)\n"
            "#define va_arg(ap, type) __builtin_va_arg(ap, type)\n"
            "#define va_end(ap) ((void)0)\n"
            "#define va_copy(dest, src) ((void)((dest) = (src)))\n"
            "#define __va_copy(dest, src) ((void)((dest) = (src)))\n"
            "#define __builtin_va_copy(dest, src) ((void)((dest) = (src)))\n"
            "#endif\n"
        );
    }
    if (strcmp(name, "time.h") == 0) {
        return strdup(
            "#ifndef _TIME_H\n#define _TIME_H 1\n"
            "typedef long time_t;\n"
            "struct tm { int tm_sec; int tm_min; int tm_hour; int tm_mday; int tm_mon; int tm_year; int tm_wday; int tm_yday; int tm_isdst; };\n"
            "time_t time(time_t *t);\n"
            "double difftime(time_t stop, time_t start);\n"
            "struct tm *gmtime(const time_t *t);\n"
            "struct tm *localtime(const time_t *t);\n"
            "char *ctime(const time_t *t);\n"
            "#endif\n"
        );
    }
    if (strcmp(name, "unistd.h") == 0) {
        return strdup(
            "#ifndef _UNISTD_H\n#define _UNISTD_H 1\n"
            "typedef unsigned long size_t;\n"
            "int write(int fd, const void *buf, size_t count);\n"
            "int read(int fd, void *buf, size_t count);\n"
            "int close(int fd);\n"
            "long getpid(void);\n"
            "int access(const char *path, int mode);\n"
            "int pipe(int pipefd[2]);\n"
            "int dup(int oldfd);\n"
            "int dup2(int oldfd, int newfd);\n"
            "void *sbrk(int incr);\n"
            "#define F_OK 0\n#define R_OK 4\n#define W_OK 2\n#define X_OK 1\n"
            "#define STDIN_FILENO 0\n#define STDOUT_FILENO 1\n#define STDERR_FILENO 2\n"
            "#endif\n"
        );
    }
    if (strcmp(name, "fcntl.h") == 0) {
        return strdup(
            "#ifndef _FCNTL_H\n#define _FCNTL_H 1\n"
            "typedef unsigned long size_t;\n"
            "int open(const char *path, int flags, ...);\n"
            "int creat(const char *path, int mode);\n"
            "#define O_RDONLY 0\n#define O_WRONLY 1\n#define O_RDWR 2\n"
            "#define O_CREAT 64\n#define O_EXCL 128\n#define O_TRUNC 512\n#define O_APPEND 1024\n"
            "#define O_NONBLOCK 2048\n"
            "#endif\n"
        );
    }
    if (strcmp(name, "sys/types.h") == 0) {
        return strdup(
            "#ifndef _SYS_TYPES_H\n#define _SYS_TYPES_H 1\n"
            "typedef unsigned long size_t;\n"
            "typedef long ssize_t;\n"
            "typedef unsigned int uid_t;\n"
            "typedef unsigned int gid_t;\n"
            "typedef long off_t;\n"
            "typedef unsigned int mode_t;\n"
            "typedef long pid_t;\n"
            "#endif\n"
        );
    }
    if (strcmp(name, "sys/stat.h") == 0) {
        return strdup(
            "#ifndef _SYS_STAT_H\n#define _SYS_STAT_H 1\n"
            "typedef unsigned long size_t;\n"
            "struct stat { unsigned long st_mode; unsigned long st_size; long st_mtime; };\n"
            "int stat(const char *path, struct stat *buf);\n"
            "int lstat(const char *path, struct stat *buf);\n"
            "int mkdir(const char *path, int mode);\n"
            "int chmod(const char *path, int mode);\n"
            "#define S_ISREG(m) (((m) & 61440) == 32768)\n"
            "#define S_ISDIR(m) (((m) & 61440) == 16384)\n"
            "#define S_IRWXU 493\n#define S_IRUSR 256\n#define S_IWUSR 128\n#define S_IXUSR 64\n"
            "#define S_IRWXG 56\n#define S_IRGRP 32\n#define S_IWGRP 16\n#define S_IXGRP 8\n"
            "#define S_IRWXO 7\n#define S_IROTH 4\n#define S_IWOTH 2\n#define S_IXOTH 1\n"
            "#endif\n"
        );
    }
    if (strcmp(name, "sys/mman.h") == 0) {
        return strdup(
            "#ifndef _SYS_MMAN_H\n#define _SYS_MMAN_H 1\n"
            "typedef unsigned long size_t;\n"
            "#define PROT_NONE 0\n#define PROT_READ 1\n#define PROT_WRITE 2\n#define PROT_EXEC 4\n"
            "#define MAP_PRIVATE 2\n#define MAP_SHARED 1\n#define MAP_ANONYMOUS 32\n#define MAP_ANON 32\n"
            "#define MAP_FAILED ((void*)-1)\n"
            "void *mmap(void *addr, size_t length, int prot, int flags, int fd, long offset);\n"
            "int munmap(void *addr, size_t length);\n"
            "int mprotect(void *addr, size_t len, int prot);\n"
            "#endif\n"
        );
    }
    if (strcmp(name, "dlfcn.h") == 0) {
        return strdup(
            "#ifndef _DLFCN_H\n#define _DLFCN_H 1\n"
            "#define RTLD_LAZY 1\n#define RTLD_NOW 2\n#define RTLD_DEFAULT ((void*)0)\n"
            "void* dlopen(const char *filename, int flags);\n"
            "void* dlsym(void *handle, const char *symbol);\n"
            "int dlclose(void *handle);\n"
            "char* dlerror(void);\n"
            "#endif\n"
        );
    }
    if (strcmp(name, "stdio.h") == 0) {
        return strdup(
            "#ifndef _STDIO_H\n#define _STDIO_H 1\n"
            "#include <stdarg.h>\n"
            "typedef unsigned long size_t;\n"
            "typedef struct FILE FILE;\n"
            "extern FILE *stdin, *stdout, *stderr;\n"
            "#define SEEK_SET 0\n#define SEEK_CUR 1\n#define SEEK_END 2\n#define EOF (-1)\n"
            "int printf(const char *format, ...);\n"
            "int fprintf(FILE *stream, const char *format, ...);\n"
            "int sprintf(char *str, const char *format, ...);\n"
            "int snprintf(char *str, size_t size, const char *format, ...);\n"
            "int puts(const char *s);\nint putchar(int c);\n"
            "int fputs(const char *s, FILE *stream);\nint fgetc(FILE *stream);\n"
            "int getchar(void);\n"
            "FILE* fopen(const char *pathname, const char *mode);\n"
            "int fclose(FILE *stream);\n"
            "size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream);\n"
            "size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream);\n"
            "int fseek(FILE *stream, long offset, int whence);\n"
            "long ftell(FILE *stream);\nint fflush(FILE *stream);\n"
            "#endif\n"
        );
    }
    if (strcmp(name, "ctype.h") == 0) {
        return strdup(
            "#ifndef _CTYPE_H\n#define _CTYPE_H 1\n"
            "int isspace(int c);\nint isdigit(int c);\nint isalpha(int c);\nint isalnum(int c);\n"
            "int isxdigit(int c);\nint toupper(int c);\nint tolower(int c);\n"
            "#endif\n"
        );
    }
    if (strcmp(name, "fcntl.h") == 0) {
        return strdup(
            "#ifndef _FCNTL_H\n#define _FCNTL_H 1\n"
            "#define O_RDONLY 0\n#define O_WRONLY 1\n#define O_RDWR 2\n#define O_CREAT 0100\n#define O_TRUNC 01000\n"
            "int open(const char *pathname, int flags, ...);\n"
            "#endif\n"
        );
    }
    if (strcmp(name, "unistd.h") == 0) {
        return strdup(
            "#ifndef _UNISTD_H\n#define _UNISTD_H 1\n"
            "#define R_OK 4\n#define W_OK 2\n#define X_OK 1\n#define F_OK 0\n"
            "int access(const char *pathname, int mode);\nint close(int fd);\n"
            "long read(int fd, void *buf, size_t count);\nlong write(int fd, const void *buf, size_t count);\n"
            "int unlink(const char *pathname);\n"
            "#endif\n"
        );
    }
    if (strcmp(name, "sys/stat.h") == 0) {
        return strdup(
            "#ifndef _SYS_STAT_H\n#define _SYS_STAT_H 1\n"
            "typedef unsigned int mode_t;\nint chmod(const char *pathname, mode_t mode);\n"
            "#endif\n"
        );
    }
    if (strcmp(name, "sys/mman.h") == 0) {
        return strdup(
            "#ifndef _SYS_MMAN_H\n#define _SYS_MMAN_H 1\n"
            "typedef unsigned long size_t;\n"
            "#define PROT_READ 0x1\n#define PROT_WRITE 0x2\n#define PROT_EXEC 0x4\n#define PROT_NONE 0x0\n"
            "#define MAP_SHARED 0x01\n#define MAP_PRIVATE 0x02\n#define MAP_ANONYMOUS 0x20\n#define MAP_ANON 0x20\n"
            "#define MAP_FAILED ((void*)-1)\n"
            "void* mmap(void *addr, size_t length, int prot, int flags, int fd, long offset);\n"
            "int munmap(void *addr, size_t length);\nint mprotect(void *addr, size_t len, int prot);\n"
            "#endif\n"
        );
    }
    if (strcmp(name, "dlfcn.h") == 0) {
        return strdup(
            "#ifndef _DLFCN_H\n#define _DLFCN_H 1\n"
            "#define RTLD_LAZY 1\n#define RTLD_NOW 2\n#define RTLD_GLOBAL 0x100\n#define RTLD_LOCAL 0\n#define RTLD_DEFAULT ((void*)0)\n"
            "void* dlopen(const char *filename, int flags);\nvoid* dlsym(void *handle, const char *symbol);\n"
            "int dlclose(void *handle);\nchar* dlerror(void);\n"
            "#endif\n"
        );
    }
    if (strcmp(name, "elf.h") == 0) {
        return strdup(
            "#ifndef _ELF_H\n#define _ELF_H 1\n"
            "typedef unsigned short uint16_t;\ntypedef unsigned int uint32_t;\ntypedef int int32_t;\n"
            "typedef unsigned long uint64_t;\ntypedef long int64_t;\n"
            "typedef uint16_t Elf64_Half;\ntypedef uint32_t Elf64_Word;\ntypedef int32_t Elf64_Sword;\n"
            "typedef uint64_t Elf64_Xword;\ntypedef int64_t Elf64_Sxword;\ntypedef uint64_t Elf64_Addr;\ntypedef uint64_t Elf64_Off;\n"
            "#define EI_NIDENT 16\n"
            "typedef struct { unsigned char e_ident[EI_NIDENT]; Elf64_Half e_type; Elf64_Half e_machine; Elf64_Word e_version; Elf64_Addr e_entry; Elf64_Off e_phoff; Elf64_Off e_shoff; Elf64_Word e_flags; Elf64_Half e_ehsize; Elf64_Half e_phentsize; Elf64_Half e_phnum; Elf64_Half e_shentsize; Elf64_Half e_shnum; Elf64_Half e_shstrndx; } Elf64_Ehdr;\n"
            "typedef struct { Elf64_Word sh_name; Elf64_Word sh_type; Elf64_Xword sh_flags; Elf64_Addr sh_addr; Elf64_Off sh_offset; Elf64_Xword sh_size; Elf64_Word sh_link; Elf64_Word sh_info; Elf64_Xword sh_addralign; Elf64_Xword sh_entsize; } Elf64_Shdr;\n"
            "typedef struct { Elf64_Word p_type; Elf64_Word p_flags; Elf64_Off p_offset; Elf64_Addr p_vaddr; Elf64_Addr p_paddr; Elf64_Xword p_filesz; Elf64_Xword p_memsz; Elf64_Xword p_align; } Elf64_Phdr;\n"
            "typedef struct { Elf64_Word st_name; unsigned char st_info; unsigned char st_other; Elf64_Half st_shndx; Elf64_Addr st_value; Elf64_Xword st_size; } Elf64_Sym;\n"
            "#define EI_MAG0 0\n#define EI_MAG1 1\n#define EI_MAG2 2\n#define EI_MAG3 3\n#define EI_CLASS 4\n#define EI_DATA 5\n#define EI_VERSION 6\n#define EI_OSABI 7\n"
            "#define ELFMAG0 0x7f\n#define ELFMAG1 'E'\n#define ELFMAG2 'L'\n#define ELFMAG3 'F'\n"
            "#define ELFCLASS64 2\n#define ELFDATA2LSB 1\n#define EV_CURRENT 1\n#define ELFOSABI_SYSV 0\n"
            "#define ET_REL 1\n#define ET_EXEC 2\n#define EM_X86_64 62\n#define EM_AARCH64 183\n"
            "#define SHT_PROGBITS 1\n#define SHT_SYMTAB 2\n#define SHT_STRTAB 3\n"
            "#define SHF_WRITE 0x1\n#define SHF_ALLOC 0x2\n#define SHF_EXECINSTR 0x4\n"
            "#define PT_LOAD 1\n#define PF_X 0x1\n#define PF_W 0x2\n#define PF_R 0x4\n"
            "#define STB_GLOBAL 1\n#define STT_FUNC 2\n#define ELF64_ST_INFO(b,t) (((b)<<4)+((t)&0xf))\n"
            "#endif\n"
        );
    }
    if (strcmp(name, "pthread.h") == 0) {
        return strdup(
            "#ifndef _PTHREAD_H\n#define _PTHREAD_H 1\n"
            "typedef unsigned long pthread_t;\n"
            "typedef struct { int __lock; } pthread_mutex_t;\n"
            "typedef struct { int __cond; } pthread_cond_t;\n"
            "int pthread_create(pthread_t *thread, const void *attr, void *(*start_routine)(void *), void *arg);\n"
            "int pthread_join(pthread_t thread, void **retval);\n"
            "int pthread_mutex_init(pthread_mutex_t *mutex, const void *attr);\n"
            "int pthread_mutex_lock(pthread_mutex_t *mutex);\n"
            "int pthread_mutex_unlock(pthread_mutex_t *mutex);\n"
            "int pthread_mutex_destroy(pthread_mutex_t *mutex);\n"
            "int pthread_cond_init(pthread_cond_t *cond, const void *attr);\n"
            "int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex);\n"
            "int pthread_cond_signal(pthread_cond_t *cond);\n"
            "int pthread_cond_broadcast(pthread_cond_t *cond);\n"
            "int pthread_cond_destroy(pthread_cond_t *cond);\n"
            "#endif\n"
        );
    }
    if (strcmp(name, "time.h") == 0) {
        return strdup(
            "#ifndef _TIME_H\n#define _TIME_H 1\n"
            "typedef long time_t;\n"
            "struct timespec { time_t tv_sec; long tv_nsec; };\n"
            "#define CLOCK_REALTIME 0\n#define CLOCK_MONOTONIC 1\n"
            "int clock_gettime(int clk_id, struct timespec *tp);\n"
            "time_t time(time_t *tloc);\n"
            "#endif\n"
        );
    }
    if (strcmp(name, "poll.h") == 0) {
        return strdup(
            "#ifndef _POLL_H\n#define _POLL_H 1\n"
            "struct pollfd { int fd; short events; short revents; };\n"
            "typedef unsigned long nfds_t;\n"
            "#define POLLIN 0x0001\n#define POLLOUT 0x0004\n#define POLLERR 0x0008\n#define POLLHUP 0x0010\n"
            "int poll(struct pollfd *fds, nfds_t nfds, int timeout);\n"
            "#endif\n"
        );
    }
    if (strcmp(name, "sys/types.h") == 0) {
        return strdup(
            "#ifndef _SYS_TYPES_H\n#define _SYS_TYPES_H 1\n"
            "typedef long off_t;\ntypedef int pid_t;\ntypedef unsigned int mode_t;\ntypedef long ssize_t;\n"
            "#endif\n"
        );
    }
    if (strcmp(name, "sys/time.h") == 0) {
        return strdup(
            "#ifndef _SYS_TIME_H\n#define _SYS_TIME_H 1\n"
            "struct timeval { long tv_sec; long tv_usec; };\n"
            "int gettimeofday(struct timeval *tv, void *tz);\n"
            "#endif\n"
        );
    }
    if (strcmp(name, "signal.h") == 0) {
        return strdup(
            "#ifndef _SIGNAL_H\n#define _SIGNAL_H 1\n"
            "#define SIGINT 2\n#define SIGILL 4\n#define SIGABRT 6\n#define SIGFPE 8\n#define SIGSEGV 11\n#define SIGTERM 15\n"
            "typedef void (*sighandler_t)(int);\n"
            "sighandler_t signal(int signum, sighandler_t handler);\n"
            "#endif\n"
        );
    }
    if (strcmp(name, "errno.h") == 0) {
        return strdup(
            "#ifndef _ERRNO_H\n#define _ERRNO_H 1\n"
            "extern int errno;\n#define EINTR 4\n#define EAGAIN 11\n#define ENOMEM 12\n"
            "#endif\n"
        );
    }
    if (strcmp(name, "float.h") == 0) {
        return strdup(
            "#ifndef _FLOAT_H\n#define _FLOAT_H 1\n"
            "#define DBL_MAX 1.7976931348623157e+308\n#define DBL_MIN 2.2250738585072014e-308\n#define DBL_EPSILON 2.2204460492503131e-16\n"
            "#define FLT_MAX 3.402823466e+38F\n#define FLT_MIN 1.175494351e-38F\n#define FLT_EPSILON 1.192092896e-07F\n"
            "#endif\n"
        );
    }
    if (strcmp(name, "stdnoreturn.h") == 0) {
        /* C11 7.2: the `noreturn` keyword. zerocc lexes `_Noreturn` natively
         * (TK_NORETURN), so map the standard spelling onto it. */
        return strdup(
            "#ifndef _STDNORETURN_H\n#define _STDNORETURN_H 1\n"
            "#define noreturn _Noreturn\n"
            "#endif\n"
        );
    }
    if (strcmp(name, "stdatomic.h") == 0) {
        /* C11 7.17 atomics. The embedded text mirrors include/stdatomic.h
         * (kept in sync manually); the atomic_* macros lower through
         * zerocc's __atomic_* builtin family. */
        return strdup(
            "#ifndef _ZERO_CC_STDATOMIC_H\n#define _ZERO_CC_STDATOMIC_H\n"
            "\n"
            "#include <stdbool.h>\n"
            "#include <stddef.h>\n"
            "#include <stdint.h>\n"
            "\n"
            "/* ---- C11 7.17.1: atomic integer types ---- */\n"
            "typedef _Atomic bool           atomic_bool;\n"
            "typedef _Atomic char           atomic_char;\n"
            "typedef _Atomic signed char    atomic_schar;\n"
            "typedef _Atomic unsigned char  atomic_uchar;\n"
            "typedef _Atomic short          atomic_short;\n"
            "typedef _Atomic unsigned short atomic_ushort;\n"
            "typedef _Atomic int            atomic_int;\n"
            "typedef _Atomic unsigned int   atomic_uint;\n"
            "typedef _Atomic long           atomic_long;\n"
            "typedef _Atomic unsigned long  atomic_ulong;\n"
            "typedef _Atomic long long      atomic_llong;\n"
            "typedef _Atomic unsigned long long atomic_ullong;\n"
            "typedef _Atomic int8_t         atomic_int_least8_t;\n"
            "typedef _Atomic uint8_t        atomic_uint_least8_t;\n"
            "typedef _Atomic int16_t        atomic_int_least16_t;\n"
            "typedef _Atomic uint16_t       atomic_uint_least16_t;\n"
            "typedef _Atomic int32_t        atomic_int_least32_t;\n"
            "typedef _Atomic uint32_t       atomic_uint_least32_t;\n"
            "typedef _Atomic int64_t        atomic_int_least64_t;\n"
            "typedef _Atomic uint64_t       atomic_uint_least64_t;\n"
            "typedef _Atomic int_fast8_t    atomic_int_fast8_t;\n"
            "typedef _Atomic uint_fast8_t   atomic_uint_fast8_t;\n"
            "typedef _Atomic int_fast16_t   atomic_int_fast16_t;\n"
            "typedef _Atomic uint_fast16_t  atomic_uint_fast16_t;\n"
            "typedef _Atomic int_fast32_t   atomic_int_fast32_t;\n"
            "typedef _Atomic uint_fast32_t  atomic_uint_fast32_t;\n"
            "typedef _Atomic int_fast64_t   atomic_int_fast64_t;\n"
            "typedef _Atomic uint_fast64_t  atomic_uint_fast64_t;\n"
            "typedef _Atomic intptr_t       atomic_intptr_t;\n"
            "typedef _Atomic uintptr_t      atomic_uintptr_t;\n"
            "typedef _Atomic size_t         atomic_size_t;\n"
            "typedef _Atomic ptrdiff_t      atomic_ptrdiff_t;\n"
            "\n"
            "/* C11 7.17.6: atomic flags */\n"
            "typedef _Atomic int atomic_flag;\n"
            "#define ATOMIC_FLAG_INIT 0\n"
            "\n"
            "/* ---- C11 7.17.1: initializers ---- */\n"
            "#define ATOMIC_VAR_INIT(val) (val)\n"
            "#define ATOMIC_INIT(val)     ATOMIC_VAR_INIT(val) /* C11: obsolescent */\n"
            "\n"
            "#define atomic_init(obj, desired)          atomic_store(obj, desired)\n"
            "\n"
            "/* ---- C11 7.17.2: lock-free query ---- */\n"
            "#define atomic_is_lock_free(obj)           __atomic_is_lock_free(sizeof *(obj), 0)\n"
            "\n"
            "/* GCC compatibility (2 = lock-free, 1 = available, 0 = unavailable) */\n"
            "#define __GCC_ATOMIC_BOOL_LOCK_FREE        2\n"
            "#define __GCC_ATOMIC_CHAR_LOCK_FREE        2\n"
            "#define __GCC_ATOMIC_CHAR16_T_LOCK_FREE    2\n"
            "#define __GCC_ATOMIC_CHAR32_T_LOCK_FREE    2\n"
            "#define __GCC_ATOMIC_WCHAR_T_LOCK_FREE     2\n"
            "#define __GCC_ATOMIC_SHORT_LOCK_FREE       2\n"
            "#define __GCC_ATOMIC_INT_LOCK_FREE         2\n"
            "#define __GCC_ATOMIC_LONG_LOCK_FREE        2\n"
            "#define __GCC_ATOMIC_LLONG_LOCK_FREE       2\n"
            "#define __GCC_ATOMIC_POINTER_LOCK_FREE     2\n"
            "#define __GCC_ATOMIC_TEST_AND_SET_TRUEVAL  1\n"
            "\n"
            "/* ---- C11 7.17.3: atomic operations ---- */\n"
            "#define atomic_load(obj)                   __atomic_load_n(obj, __ATOMIC_SEQ_CST)\n"
            "#define atomic_load_explicit(obj, order)   __atomic_load_n(obj, order)\n"
            "#define atomic_store(obj, desired)         __atomic_store_n(obj, desired, __ATOMIC_SEQ_CST)\n"
            "#define atomic_store_explicit(obj, desired, order) __atomic_store_n(obj, desired, order)\n"
            "#define atomic_exchange(obj, desired)      __atomic_exchange_n(obj, desired, __ATOMIC_SEQ_CST)\n"
            "#define atomic_exchange_explicit(obj, desired, order) __atomic_exchange_n(obj, desired, order)\n"
            "\n"
            "#define atomic_compare_exchange_strong(obj, expected, desired) __atomic_compare_exchange_n(obj, expected, desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_ACQUIRE)\n"
            "#define atomic_compare_exchange_weak(obj, expected, desired) atomic_compare_exchange_strong(obj, expected, desired)\n"
            "#define atomic_compare_exchange_strong_explicit(obj, expected, desired, success, failure) __atomic_compare_exchange_n(obj, expected, desired, 0, success, failure)\n"
            "#define atomic_compare_exchange_weak_explicit(obj, expected, desired, success, failure) atomic_compare_exchange_strong_explicit(obj, expected, desired, success, failure)\n"
            "\n"
            "#define atomic_fetch_add(obj, arg)         __atomic_fetch_add(obj, arg, __ATOMIC_SEQ_CST)\n"
            "#define atomic_fetch_add_explicit(obj, arg, order) __atomic_fetch_add(obj, arg, order)\n"
            "#define atomic_fetch_sub(obj, arg)         __atomic_fetch_sub(obj, arg, __ATOMIC_SEQ_CST)\n"
            "#define atomic_fetch_sub_explicit(obj, arg, order) __atomic_fetch_sub(obj, arg, order)\n"
            "#define atomic_fetch_or(obj, arg)          __atomic_fetch_or(obj, arg, __ATOMIC_SEQ_CST)\n"
            "#define atomic_fetch_or_explicit(obj, arg, order) __atomic_fetch_or(obj, arg, order)\n"
            "#define atomic_fetch_and(obj, arg)         __atomic_fetch_and(obj, arg, __ATOMIC_SEQ_CST)\n"
            "#define atomic_fetch_and_explicit(obj, arg, order) __atomic_fetch_and(obj, arg, order)\n"
            "#define atomic_fetch_xor(obj, arg)         __atomic_fetch_xor(obj, arg, __ATOMIC_SEQ_CST)\n"
            "#define atomic_fetch_xor_explicit(obj, arg, order) __atomic_fetch_xor(obj, arg, order)\n"
            "\n"
            "#define atomic_add_fetch(obj, arg)         __atomic_add_fetch(obj, arg, __ATOMIC_SEQ_CST)\n"
            "#define atomic_add_fetch_explicit(obj, arg, order) __atomic_add_fetch(obj, arg, order)\n"
            "#define atomic_sub_fetch(obj, arg)         __atomic_sub_fetch(obj, arg, __ATOMIC_SEQ_CST)\n"
            "#define atomic_sub_fetch_explicit(obj, arg, order) __atomic_sub_fetch(obj, arg, order)\n"
            "#define atomic_or_fetch(obj, arg)          __atomic_or_fetch(obj, arg, __ATOMIC_SEQ_CST)\n"
            "#define atomic_or_fetch_explicit(obj, arg, order) __atomic_or_fetch(obj, arg, order)\n"
            "#define atomic_and_fetch(obj, arg)         __atomic_and_fetch(obj, arg, __ATOMIC_SEQ_CST)\n"
            "#define atomic_and_fetch_explicit(obj, arg, order) __atomic_and_fetch(obj, arg, order)\n"
            "#define atomic_xor_fetch(obj, arg)         __atomic_xor_fetch(obj, arg, __ATOMIC_SEQ_CST)\n"
            "#define atomic_xor_fetch_explicit(obj, arg, order) __atomic_xor_fetch(obj, arg, order)\n"
            "\n"
            "/* ---- C11 7.17.6: atomic flags ---- */\n"
            "#define atomic_flag_test_and_set(obj)      __atomic_test_and_set(obj)\n"
            "#define atomic_flag_clear(obj)             __atomic_clear(obj)\n"
            "\n"
            "#endif /* _ZERO_CC_STDATOMIC_H */\n"
        );
    }
    if (strcmp(name, "threads.h") == 0) {
        /* C11 7.26 threads (freestanding subset): thrd_t/mtx_t aliases over
         * the pthread surface + the thrd_ and mtx_ macros. Mirrors
         * include/threads.h (kept in sync manually). */
        return strdup(
            "#ifndef _ZERO_CC_THREADS_H\n#define _ZERO_CC_THREADS_H\n"
            "\n"
            "typedef unsigned long thrd_t;\n"
            "typedef union {\n"
            "    char __size[40];\n"
            "    long __align;\n"
            "} mtx_t;\n"
            "\n"
            "enum {\n"
            "    thrd_success  = 0,\n"
            "    thrd_error    = 1,\n"
            "    thrd_busy     = 2,\n"
            "    thrd_nomem    = 3,\n"
            "    thrd_timedout = 4\n"
            "};\n"
            "\n"
            "enum {\n"
            "    mtx_plain     = 0,\n"
            "    mtx_recursive = 1,\n"
            "    mtx_timed     = 2\n"
            "};\n"
            "\n"
            "int pthread_create(thrd_t *thread, void *attr, void *start_routine, void *arg);\n"
            "int pthread_join(thrd_t thread, void **retval);\n"
            "int pthread_mutex_init(mtx_t *mutex, void *attr);\n"
            "int pthread_mutex_lock(mtx_t *mutex);\n"
            "int pthread_mutex_unlock(mtx_t *mutex);\n"
            "int pthread_mutex_destroy(mtx_t *mutex);\n"
            "\n"
            "#define thrd_create(t, fn, arg) (pthread_create((t), (void *)0, (void *)(fn), (void *)(arg)) == 0 ? thrd_success : thrd_error)\n"
            "#define thrd_join(t, res)       (pthread_join((t), (void **)(res)) == 0 ? thrd_success : thrd_error)\n"
            "#define mtx_init(m, type)       (pthread_mutex_init((m), (void *)0) == 0 ? thrd_success : thrd_error)\n"
            "#define mtx_lock(m)             (pthread_mutex_lock(m) == 0 ? thrd_success : thrd_error)\n"
            "#define mtx_unlock(m)           (pthread_mutex_unlock(m) == 0 ? thrd_success : thrd_error)\n"
            "#define mtx_destroy(m)          pthread_mutex_destroy(m)\n"
            "\n"
            "#endif /* _ZERO_CC_THREADS_H */\n"
        );
    }
    return NULL;
}

static inline char* zero_cc_cpp_resolve_include(ZeroCCPreprocessor *cpp, const char *filename, bool is_system,
                                                 char *out_path, size_t out_cap) {
    /* A resolved include is a filesystem path, bounded by PATH_MAX. The
     * +64 leaves room for the "/filename" component appended below; a
     * result that still overflows is an invalid (too-long) path that
     * access() rejects, and snprintf truncates safely either way. */
    char path[PATH_MAX + 64];
    /* 1. First check virtual freestanding system headers */
    char *vhdr = zero_cc_cpp_get_virtual_header(filename);
    if (vhdr) {
        if (out_path && out_cap) out_path[0] = '\0'; /* virtual: no file directory */
        return vhdr;
    }

    /* 2. Quoted includes: directory of the including file first (C99
     *    6.10.2p2), nearest real-file directory on the stack first, then
     *    the working directory (top-level source siblings). */
    if (!is_system) {
        for (int d = cpp->include_dir_depth - 1; d >= 0; d--) {
            snprintf(path, sizeof(path), "%s/%s", cpp->include_file_dirs[d], filename);
            if (access(path, R_OK) == 0) {
                if (out_path && out_cap) snprintf(out_path, out_cap, "%s", path);
                return zero_cc_read_file(path);
            }
        }
        if (access(filename, R_OK) == 0) {
            if (out_path && out_cap) snprintf(out_path, out_cap, "%s", filename);
            return zero_cc_read_file(filename);
        }
    }
    /* 3. Check explicitly added include directories */
    for (size_t i = 0; i < cpp->include_dirs.count; i++) {
        snprintf(path, sizeof(path), "%s/%s", cpp->include_dirs.paths[i], filename);
        if (access(path, R_OK) == 0) {
            if (out_path && out_cap) snprintf(out_path, out_cap, "%s", path);
            return zero_cc_read_file(path);
        }
    }
    /* 4. Host fallback paths if needed */
    static const char *sys_paths[] = {
        "/usr/include",
        "/usr/local/include",
        "/usr/include/x86_64-linux-gnu",
        "/usr/include/aarch64-linux-gnu",
        NULL
    };
    for (int i = 0; sys_paths[i]; i++) {
        snprintf(path, sizeof(path), "%s/%s", sys_paths[i], filename);
        if (access(path, R_OK) == 0) {
            if (out_path && out_cap) snprintf(out_path, out_cap, "%s", path);
            return zero_cc_read_file(path);
        }
    }
    if (out_path && out_cap) out_path[0] = '\0';
    return NULL;
}

/* Push the directory of a real include file so nested quoted includes
 * resolve relative to it (C99 6.10.2p2).  Returns the stack depth to
 * restore afterwards. */
static inline int zero_cc_cpp_push_include_dir(ZeroCCPreprocessor *cpp, const char *resolved_path) {
    int saved = cpp->include_dir_depth;
    if (!resolved_path || !resolved_path[0]) return saved;
    if (cpp->include_dir_depth >= 32) return saved;
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", resolved_path);
    char *slash = strrchr(tmp, '/');
    if (slash && slash != tmp) {
        *slash = '\0';
        snprintf(cpp->include_file_dirs[cpp->include_dir_depth], PATH_MAX, "%s", tmp);
    } else {
        snprintf(cpp->include_file_dirs[cpp->include_dir_depth], PATH_MAX, ".");
    }
    cpp->include_dir_depth++;
    return saved;
}

static inline void zero_cc_cpp_pop_include_dir(ZeroCCPreprocessor *cpp, int saved_depth) {
    cpp->include_dir_depth = saved_depth;
}

/* Record an included file for dependency generation (-MMD/-MF/-MT) */
static inline void zero_cc_cpp_add_dependency(ZeroCCPreprocessor *cpp, const char *path) {
    if (!path || !path[0]) return;
    for (size_t i = 0; i < cpp->dep_file_count; i++) {
        if (strcmp(cpp->dep_files[i], path) == 0) return;
    }
    if (cpp->dep_file_count >= cpp->dep_file_capacity) {
        size_t new_cap = cpp->dep_file_capacity ? cpp->dep_file_capacity * 2 : 16;
        char **new_files = (char **)realloc(cpp->dep_files, new_cap * sizeof(char *));
        if (!new_files) return;
        cpp->dep_files = new_files;
        cpp->dep_file_capacity = new_cap;
    }
    cpp->dep_files[cpp->dep_file_count] = strdup(path);
    cpp->dep_file_count++;
}

/* Write Makefile-style dependency rule (-MMD/-MF/-MT) */
static inline bool zero_cc_cpp_write_dep_file(ZeroCCPreprocessor *cpp, const char *dep_file) {
    if (!cpp || !dep_file || !dep_file[0]) return false;
    const char *target = (cpp->dep_target && cpp->dep_target[0]) ? cpp->dep_target : "a.o";
    FILE *f = fopen(dep_file, "w");
    if (!f) {
        perror(dep_file);
        return false;
    }
    fprintf(f, "%s:", target);
    size_t col = strlen(target) + 1;
    for (size_t i = 0; i < cpp->dep_file_count; i++) {
        size_t len = strlen(cpp->dep_files[i]);
        if (col + len + 1 > 78) {
            fprintf(f, " \\\n ");
            col = 1;
        }
        fprintf(f, " %s", cpp->dep_files[i]);
        col += len + 1;
    }
    fprintf(f, "\n");
    fclose(f);
    return true;
}

static inline void zero_cc_cpp_process_text(ZeroCCPreprocessor *cpp, const char *src, ZeroCCBuffer *out);

/* ---- Pragma handling (message / GCC diagnostic / push_options) ---- */
static inline const char *zero_cc_pragma_skip_ws(const char *q) {
    while (*q == ' ' || *q == '\t') q++;
    return q;
}

static inline bool zero_cc_pragma_word(const char **pq, const char *w) {
    size_t n = strlen(w);
    const char *q = *pq;
    if (strncmp(q, w, n) != 0 || isalnum((unsigned char)q[n]) || q[n] == '_') return false;
    *pq = zero_cc_pragma_skip_ws(q + n);
    return true;
}

/* Copy first quoted string in q into out, stripping a leading "-W". */
static inline bool zero_cc_pragma_quoted(const char *q, char *out, size_t cap, bool strip_w) {
    const char *a = strchr(q, '"');
    if (!a) return false;
    a++;
    const char *b = strchr(a, '"');
    if (!b) return false;
    if (strip_w && strncmp(a, "-W", 2) == 0) a += 2;
    size_t n = (size_t)(b - a);
    if (n >= cap) n = cap - 1;
    memcpy(out, a, n);
    out[n] = '\0';
    return true;
}

static inline void zero_cc_diag_remove(ZeroCCPreprocessor *cpp, const char *name) {
    for (int i = 0; i < cpp->diagnostic_state.disabled_warning_count; i++) {
        if (cpp->diagnostic_state.disabled_warnings[i] &&
            strcmp(cpp->diagnostic_state.disabled_warnings[i], name) == 0) {
            free(cpp->diagnostic_state.disabled_warnings[i]);
            int last = --cpp->diagnostic_state.disabled_warning_count;
            cpp->diagnostic_state.disabled_warnings[i] = cpp->diagnostic_state.disabled_warnings[last];
            cpp->diagnostic_state.disabled_warnings[last] = NULL;
            return;
        }
    }
}

/* Returns true if the pragma was consumed by the preprocessor. */
static inline bool zero_cc_cpp_handle_pragma(ZeroCCPreprocessor *cpp, const char *text, size_t len) {
    char buf[512];
    if (len >= sizeof(buf)) len = sizeof(buf) - 1;
    memcpy(buf, text, len);
    buf[len] = '\0';
    while (len > 0 && isspace((unsigned char)buf[len - 1])) buf[--len] = '\0';
    const char *q = zero_cc_pragma_skip_ws(buf);

    if (zero_cc_pragma_word(&q, "message")) {
        char msg[400];
        if (!zero_cc_pragma_quoted(q, msg, sizeof(msg), false)) msg[0] = '\0';
        fprintf(stderr, "%s:%d: note: #pragma message: %s\n",
                cpp->filename ? cpp->filename : "<stdin>", (int)cpp->line_num, msg);
        return true;
    }
    if (zero_cc_pragma_word(&q, "push_macro")) {
        char name[128];
        if (zero_cc_pragma_quoted(q, name, sizeof(name), false)) {
            zero_cc_cpp_push_macro(cpp, name);
            return true;
        }
    }
    if (zero_cc_pragma_word(&q, "pop_macro")) {
        char name[128];
        if (zero_cc_pragma_quoted(q, name, sizeof(name), false)) {
            zero_cc_cpp_pop_macro(cpp, name);
            return true;
        }
    }
    if (!zero_cc_pragma_word(&q, "GCC")) return false;
    if (zero_cc_pragma_word(&q, "push_options") || zero_cc_pragma_word(&q, "pop_options")) {
        return true; /* no per-region options are tracked yet */
    }
    if (!zero_cc_pragma_word(&q, "diagnostic")) return false;
    typeof(cpp->diagnostic_state) *d = &cpp->diagnostic_state;
    if (zero_cc_pragma_word(&q, "push")) {
        if (d->stack_depth < 16) {
            int k = d->stack_depth++;
            d->warning_as_error_stack[k] = d->warning_as_error;
            d->disabled_warning_count_stack[k] = d->disabled_warning_count;
            for (int i = 0; i < d->disabled_warning_count; i++)
                d->disabled_warnings_stack[k][i] = d->disabled_warnings[i] ? strdup(d->disabled_warnings[i]) : NULL;
        }
        return true;
    }
    if (zero_cc_pragma_word(&q, "pop")) {
        if (d->stack_depth > 0) {
            int k = --d->stack_depth;
            for (int i = 0; i < d->disabled_warning_count; i++) free(d->disabled_warnings[i]);
            d->warning_as_error = d->warning_as_error_stack[k];
            d->disabled_warning_count = d->disabled_warning_count_stack[k];
            for (int i = 0; i < d->disabled_warning_count; i++) {
                d->disabled_warnings[i] = d->disabled_warnings_stack[k][i];
                d->disabled_warnings_stack[k][i] = NULL;
            }
            d->disabled_warning_count_stack[k] = 0;
        }
        return true;
    }
    bool ignored = zero_cc_pragma_word(&q, "ignored");
    bool warning = !ignored && zero_cc_pragma_word(&q, "warning");
    bool error = !ignored && !warning && zero_cc_pragma_word(&q, "error");
    if (ignored || warning || error) {
        char name[128];
        bool is_werror = false;
        if (zero_cc_pragma_quoted(q, name, sizeof(name), true)) {
            is_werror = (strcmp(name, "error") == 0);
            if (ignored) {
                bool present = false;
                for (int i = 0; i < d->disabled_warning_count; i++)
                    if (d->disabled_warnings[i] && strcmp(d->disabled_warnings[i], name) == 0) present = true;
                if (!present && d->disabled_warning_count < 64)
                    d->disabled_warnings[d->disabled_warning_count++] = strdup(name);
            } else if (is_werror) {
                d->warning_as_error = error;
            } else {
                zero_cc_diag_remove(cpp, name); /* warning/error re-enable the diagnostic */
            }
        }
        return true;
    }
    return true; /* unknown GCC pragma: ignore like GCC does */
}

/* Publish the final diagnostic pragma state for the tokenizer/compiler. */
static inline void zero_cc_cpp_publish_diag_state(ZeroCCPreprocessor *cpp) {
    for (int i = 0; i < zero_cc_global_diag_state.disabled_warning_count && i < 64; i++) {
        free(zero_cc_global_diag_state.disabled_warnings[i]);
        zero_cc_global_diag_state.disabled_warnings[i] = NULL;
    }
    zero_cc_global_diag_state.warning_as_error = cpp->diagnostic_state.warning_as_error;
    zero_cc_global_diag_state.disabled_warning_count = cpp->diagnostic_state.disabled_warning_count;
    for (int i = 0; i < cpp->diagnostic_state.disabled_warning_count; i++) {
        zero_cc_global_diag_state.disabled_warnings[i] =
            cpp->diagnostic_state.disabled_warnings[i] ? strdup(cpp->diagnostic_state.disabled_warnings[i]) : NULL;
    }
}


/* Preprocessor #if expression evaluator (C99 6.10.1) */

static inline void zero_cc_cpp_skip_expr_ws(const char **p) {
    while (**p) {
        if (isspace((unsigned char)**p)) {
            (*p)++;
        } else if (**p == '/' && (*p)[1] == '/') {
            *p += 2;
            while (**p && **p != '\n') (*p)++;
        } else if (**p == '/' && (*p)[1] == '*') {
            *p += 2;
            while (**p && !(**p == '*' && (*p)[1] == '/')) (*p)++;
            if (**p) *p += 2;
        } else {
            break;
        }
    }
}

static inline int64_t zero_cc_cpp_eval_comma(const char **p);
static inline int64_t zero_cc_cpp_eval_ternary(const char **p);
static inline int64_t zero_cc_cpp_eval_lor(const char **p);
static inline int64_t zero_cc_cpp_eval_land(const char **p);
static inline int64_t zero_cc_cpp_eval_bor(const char **p);
static inline int64_t zero_cc_cpp_eval_bxor(const char **p);
static inline int64_t zero_cc_cpp_eval_band(const char **p);
static inline int64_t zero_cc_cpp_eval_eq(const char **p);
static inline int64_t zero_cc_cpp_eval_rel(const char **p);
static inline int64_t zero_cc_cpp_eval_shift(const char **p);
static inline int64_t zero_cc_cpp_eval_add(const char **p);
static inline int64_t zero_cc_cpp_eval_mul(const char **p);
static inline int64_t zero_cc_cpp_eval_unary(const char **p);
static inline int64_t zero_cc_cpp_eval_primary(const char **p);

static inline int64_t zero_cc_cpp_eval_primary(const char **p) {
    zero_cc_cpp_skip_expr_ws(p);
    if (!**p) return 0;

    if (**p == '(') {
        (*p)++;
        int64_t val = zero_cc_cpp_eval_comma(p);
        zero_cc_cpp_skip_expr_ws(p);
        if (**p == ')') (*p)++;
        return val;
    }

    /* Character constant: 'x', '\n', L'x', etc. */
    bool is_wide_char = false;
    if ((**p == 'L' || **p == 'u' || **p == 'U') && (*p)[1] == '\'') {
        is_wide_char = true;
        (*p)++;
    }
    if (**p == '\'') {
        (*p)++;
        int64_t val = 0;
        while (**p && **p != '\'') {
            int ch = 0;
            if (**p == '\\') {
                (*p)++;
                char esc = 0;
                if (!zero_cc_handle_escape(p, NULL, &esc, &ch)) {
                    ch = (unsigned char)(**p);
                }
                if (**p) (*p)++;
            } else {
                ch = (unsigned char)(**p);
                (*p)++;
            }
            if (is_wide_char) {
                val = ch;
            } else {
                val = (val << 8) | (ch & 0xFF);
            }
        }
        if (**p == '\'') (*p)++;
        return val;
    }

    /* Integer literal */
    if (isdigit((unsigned char)**p)) {
        char *end = NULL;
        int base = 10;
        if (**p == '0' && ((*p)[1] == 'x' || (*p)[1] == 'X')) {
            base = 16;
            *p += 2;
        } else if (**p == '0' && ((*p)[1] == 'b' || (*p)[1] == 'B')) {
            base = 2;
            *p += 2;
        } else if (**p == '0' && isdigit((unsigned char)(*p)[1])) {
            base = 8;
        }
        uint64_t val = strtoull(*p, &end, base);
        if (end && end != *p) {
            *p = end;
            while (**p == 'u' || **p == 'U' || **p == 'l' || **p == 'L') {
                (*p)++;
            }
        }
        return (int64_t)val;
    }

    /* Identifier: remaining identifiers in preprocessor expressions evaluate to 0 */
    if (isalpha((unsigned char)**p) || **p == '_') {
        char id[128];
        size_t l = 0;
        while (**p && (isalnum((unsigned char)**p) || **p == '_')) {
            if (l + 1 < sizeof(id)) id[l++] = **p;
            (*p)++;
        }
        id[l] = '\0';
        if (strcmp(id, "true") == 0) return 1;
        return 0;
    }

    /* Skip unexpected char */
    (*p)++;
    return 0;
}

static inline int64_t zero_cc_cpp_eval_unary(const char **p) {
    zero_cc_cpp_skip_expr_ws(p);
    if (**p == '+') {
        (*p)++;
        return zero_cc_cpp_eval_unary(p);
    }
    if (**p == '-') {
        (*p)++;
        return -zero_cc_cpp_eval_unary(p);
    }
    if (**p == '~') {
        (*p)++;
        return ~zero_cc_cpp_eval_unary(p);
    }
    if (**p == '!') {
        (*p)++;
        return !zero_cc_cpp_eval_unary(p);
    }
    return zero_cc_cpp_eval_primary(p);
}

static inline int64_t zero_cc_cpp_eval_mul(const char **p) {
    int64_t left = zero_cc_cpp_eval_unary(p);
    while (1) {
        zero_cc_cpp_skip_expr_ws(p);
        if (**p == '*' && (*p)[1] != '=') {
            (*p)++;
            int64_t right = zero_cc_cpp_eval_unary(p);
            left = left * right;
        } else if (**p == '/' && (*p)[1] != '/' && (*p)[1] != '*' && (*p)[1] != '=') {
            (*p)++;
            int64_t right = zero_cc_cpp_eval_unary(p);
            left = (right != 0) ? (left / right) : 0;
        } else if (**p == '%' && (*p)[1] != '=') {
            (*p)++;
            int64_t right = zero_cc_cpp_eval_unary(p);
            left = (right != 0) ? (left % right) : 0;
        } else {
            break;
        }
    }
    return left;
}

static inline int64_t zero_cc_cpp_eval_add(const char **p) {
    int64_t left = zero_cc_cpp_eval_mul(p);
    while (1) {
        zero_cc_cpp_skip_expr_ws(p);
        if (**p == '+' && (*p)[1] != '+' && (*p)[1] != '=') {
            (*p)++;
            int64_t right = zero_cc_cpp_eval_mul(p);
            left = left + right;
        } else if (**p == '-' && (*p)[1] != '-' && (*p)[1] != '=' && (*p)[1] != '>') {
            (*p)++;
            int64_t right = zero_cc_cpp_eval_mul(p);
            left = left - right;
        } else {
            break;
        }
    }
    return left;
}

static inline int64_t zero_cc_cpp_eval_shift(const char **p) {
    int64_t left = zero_cc_cpp_eval_add(p);
    while (1) {
        zero_cc_cpp_skip_expr_ws(p);
        if (**p == '<' && (*p)[1] == '<') {
            *p += 2;
            int64_t right = zero_cc_cpp_eval_add(p);
            left = (right >= 0 && right < 64) ? (left << right) : 0;
        } else if (**p == '>' && (*p)[1] == '>') {
            *p += 2;
            int64_t right = zero_cc_cpp_eval_add(p);
            left = (right >= 0 && right < 64) ? (left >> right) : 0;
        } else {
            break;
        }
    }
    return left;
}

static inline int64_t zero_cc_cpp_eval_rel(const char **p) {
    int64_t left = zero_cc_cpp_eval_shift(p);
    while (1) {
        zero_cc_cpp_skip_expr_ws(p);
        if (**p == '<' && (*p)[1] == '=') {
            *p += 2;
            int64_t right = zero_cc_cpp_eval_shift(p);
            left = (left <= right);
        } else if (**p == '>' && (*p)[1] == '=') {
            *p += 2;
            int64_t right = zero_cc_cpp_eval_shift(p);
            left = (left >= right);
        } else if (**p == '<' && (*p)[1] != '<') {
            (*p)++;
            int64_t right = zero_cc_cpp_eval_shift(p);
            left = (left < right);
        } else if (**p == '>' && (*p)[1] != '>') {
            (*p)++;
            int64_t right = zero_cc_cpp_eval_shift(p);
            left = (left > right);
        } else {
            break;
        }
    }
    return left;
}

static inline int64_t zero_cc_cpp_eval_eq(const char **p) {
    int64_t left = zero_cc_cpp_eval_rel(p);
    while (1) {
        zero_cc_cpp_skip_expr_ws(p);
        if (**p == '=' && (*p)[1] == '=') {
            *p += 2;
            int64_t right = zero_cc_cpp_eval_rel(p);
            left = (left == right);
        } else if (**p == '!' && (*p)[1] == '=') {
            *p += 2;
            int64_t right = zero_cc_cpp_eval_rel(p);
            left = (left != right);
        } else {
            break;
        }
    }
    return left;
}

static inline int64_t zero_cc_cpp_eval_band(const char **p) {
    int64_t left = zero_cc_cpp_eval_eq(p);
    while (1) {
        zero_cc_cpp_skip_expr_ws(p);
        if (**p == '&' && (*p)[1] != '&') {
            (*p)++;
            int64_t right = zero_cc_cpp_eval_eq(p);
            left = left & right;
        } else {
            break;
        }
    }
    return left;
}

static inline int64_t zero_cc_cpp_eval_bxor(const char **p) {
    int64_t left = zero_cc_cpp_eval_band(p);
    while (1) {
        zero_cc_cpp_skip_expr_ws(p);
        if (**p == '^') {
            (*p)++;
            int64_t right = zero_cc_cpp_eval_band(p);
            left = left ^ right;
        } else {
            break;
        }
    }
    return left;
}

static inline int64_t zero_cc_cpp_eval_bor(const char **p) {
    int64_t left = zero_cc_cpp_eval_bxor(p);
    while (1) {
        zero_cc_cpp_skip_expr_ws(p);
        if (**p == '|' && (*p)[1] != '|') {
            (*p)++;
            int64_t right = zero_cc_cpp_eval_bxor(p);
            left = left | right;
        } else {
            break;
        }
    }
    return left;
}

static inline int64_t zero_cc_cpp_eval_land(const char **p) {
    int64_t left = zero_cc_cpp_eval_bor(p);
    while (1) {
        zero_cc_cpp_skip_expr_ws(p);
        if (**p == '&' && (*p)[1] == '&') {
            *p += 2;
            int64_t right = zero_cc_cpp_eval_bor(p);
            left = (left && right);
        } else {
            break;
        }
    }
    return left;
}

static inline int64_t zero_cc_cpp_eval_lor(const char **p) {
    int64_t left = zero_cc_cpp_eval_land(p);
    while (1) {
        zero_cc_cpp_skip_expr_ws(p);
        if (**p == '|' && (*p)[1] == '|') {
            *p += 2;
            int64_t right = zero_cc_cpp_eval_land(p);
            left = (left || right);
        } else {
            break;
        }
    }
    return left;
}

static inline int64_t zero_cc_cpp_eval_ternary(const char **p) {
    int64_t cond = zero_cc_cpp_eval_lor(p);
    zero_cc_cpp_skip_expr_ws(p);
    if (**p == '?') {
        (*p)++;
        int64_t then_val = zero_cc_cpp_eval_comma(p);
        zero_cc_cpp_skip_expr_ws(p);
        int64_t else_val = 0;
        if (**p == ':') {
            (*p)++;
            else_val = zero_cc_cpp_eval_ternary(p);
        }
        return cond ? then_val : else_val;
    }
    return cond;
}

static inline int64_t zero_cc_cpp_eval_comma(const char **p) {
    int64_t val = zero_cc_cpp_eval_ternary(p);
    while (1) {
        zero_cc_cpp_skip_expr_ws(p);
        if (**p == ',') {
            (*p)++;
            val = zero_cc_cpp_eval_ternary(p);
        } else {
            break;
        }
    }
    return val;
}

static inline int64_t zero_cc_cpp_eval_expr(ZeroCCPreprocessor *cpp, const char *expr) {
    while (*expr && isspace((unsigned char)*expr) && *expr != '\n') expr++;
    if (!*expr || *expr == '\n') return 0;

    /* Step 1: Extract expression up to newline or line-ending comment */
    ZeroCCBuffer line_buf;
    zero_cc_buf_init(&line_buf, 256);
    const char *p = expr;
    while (*p && *p != '\n') {
        if (*p == '\\' && p[1] == '\n') {
            p += 2;
            continue;
        }
        if (*p == '/' && p[1] == '/') {
            break;
        }
        if (*p == '/' && p[1] == '*') {
            p += 2;
            while (*p && !(*p == '*' && p[1] == '/')) p++;
            if (*p) p += 2;
            zero_cc_buf_putc(&line_buf, ' ');
            continue;
        }
        if (*p == '"' || *p == '\'') {
            char quote = *p;
            zero_cc_buf_putc(&line_buf, *p++);
            while (*p && *p != quote && *p != '\n') {
                if (*p == '\\' && p[1]) {
                    zero_cc_buf_putc(&line_buf, *p++);
                }
                zero_cc_buf_putc(&line_buf, *p++);
            }
            if (*p == quote) zero_cc_buf_putc(&line_buf, *p++);
            continue;
        }
        zero_cc_buf_putc(&line_buf, *p++);
    }
    zero_cc_buf_putc(&line_buf, '\0');

    /* Step 2: Replace `defined(ID)` / `defined ID` and `__has_include(...)` */
    ZeroCCBuffer def_buf;
    zero_cc_buf_init(&def_buf, line_buf.length ? line_buf.length : 256);
    const char *lp = line_buf.data;
    while (*lp) {
        if (isspace((unsigned char)*lp)) {
            zero_cc_buf_putc(&def_buf, *lp++);
            continue;
        }
        if (*lp == '"' || *lp == '\'') {
            char quote = *lp;
            zero_cc_buf_putc(&def_buf, *lp++);
            while (*lp && *lp != quote) {
                if (*lp == '\\' && lp[1]) {
                    zero_cc_buf_putc(&def_buf, *lp++);
                }
                zero_cc_buf_putc(&def_buf, *lp++);
            }
            if (*lp == quote) zero_cc_buf_putc(&def_buf, *lp++);
            continue;
        }
        if (isalpha((unsigned char)*lp) || *lp == '_') {
            char id[128];
            size_t id_len = 0;
            while (*lp && (isalnum((unsigned char)*lp) || *lp == '_')) {
                if (id_len + 1 < sizeof(id)) id[id_len++] = *lp;
                lp++;
            }
            id[id_len] = '\0';
            if (strcmp(id, "defined") == 0) {
                while (*lp && isspace((unsigned char)*lp)) lp++;
                bool has_paren = (*lp == '(');
                if (has_paren) {
                    lp++;
                    while (*lp && isspace((unsigned char)*lp)) lp++;
                }
                char macro_name[128];
                size_t ml = 0;
                while (*lp && (isalnum((unsigned char)*lp) || *lp == '_')) {
                    if (ml + 1 < sizeof(macro_name)) macro_name[ml++] = *lp;
                    lp++;
                }
                macro_name[ml] = '\0';
                if (has_paren) {
                    while (*lp && isspace((unsigned char)*lp)) lp++;
                    if (*lp == ')') lp++;
                }
                bool is_def = (strcmp(macro_name, "__has_include") == 0) ||
                              (zero_cc_cpp_find_macro(cpp, macro_name) != NULL);
                zero_cc_buf_putc(&def_buf, is_def ? '1' : '0');
                zero_cc_buf_putc(&def_buf, ' ');
            } else if (strcmp(id, "__has_include") == 0) {
                while (*lp && isspace((unsigned char)*lp)) lp++;
                if (*lp == '(') lp++;
                while (*lp && isspace((unsigned char)*lp)) lp++;
                bool is_sys = (*lp == '<');
                char delim = is_sys ? '>' : '"';
                if (*lp == '<' || *lp == '"') lp++;
                char inc_name[256];
                size_t inl = 0;
                while (*lp && *lp != delim && *lp != '\n' && inl + 1 < sizeof(inc_name)) {
                    inc_name[inl++] = *lp++;
                }
                inc_name[inl] = '\0';
                if (*lp == delim) lp++;
                while (*lp && isspace((unsigned char)*lp)) lp++;
                if (*lp == ')') lp++;
                char res_path[PATH_MAX];
                char *content = zero_cc_cpp_resolve_include(cpp, inc_name, is_sys, res_path, sizeof(res_path));
                bool has_inc = (content != NULL);
                if (content) free(content);
                zero_cc_buf_putc(&def_buf, has_inc ? '1' : '0');
                zero_cc_buf_putc(&def_buf, ' ');
            } else {
                zero_cc_buf_puts(&def_buf, id);
            }
            continue;
        }
        zero_cc_buf_putc(&def_buf, *lp++);
    }
    zero_cc_buf_putc(&def_buf, '\0');
    zero_cc_buf_free(&line_buf);

    /* Step 3: Macro expansion on expression tokens */
    ZeroCCBuffer exp_buf;
    zero_cc_buf_init(&exp_buf, def_buf.length ? def_buf.length * 2 : 256);
    size_t saved_line = cpp->line_num;
    int saved_depth = cpp->cond_depth;
    cpp->cond_depth = 0;
    zero_cc_cpp_process_text(cpp, def_buf.data, &exp_buf);
    cpp->cond_depth = saved_depth;
    cpp->line_num = saved_line;
    zero_cc_buf_putc(&exp_buf, '\0');
    zero_cc_buf_free(&def_buf);

    /* Step 4: Parse and evaluate constant expression */
    const char *ep = exp_buf.data;
    int64_t result = zero_cc_cpp_eval_comma(&ep);
    zero_cc_buf_free(&exp_buf);
    return result;
}

static inline void zero_cc_cpp_process_text(ZeroCCPreprocessor *cpp, const char *src, ZeroCCBuffer *out) {
    if (!src) return;
    const char *p = src;
    bool at_line_start = true;
    cpp->line_num = 1; /* Start at line 1 */

    while (*p) {
        /* Comments - process first */
        if (*p == '/' && p[1] == '/') {
            p += 2;
            while (*p && *p != '\n') p++;
            if (*p == '\n') { zero_cc_buf_putc(out, *p++); at_line_start = true; cpp->line_num++; }
            continue;
        }
        if (*p == '/' && p[1] == '*') {
            p += 2;
            while (*p && !(*p == '*' && p[1] == '/')) {
                if (*p == '\n') { zero_cc_buf_putc(out, '\n'); at_line_start = true; cpp->line_num++; }
                p++;
            }
            if (*p) p += 2;
            continue;
        }

        if (at_line_start) {
            const char *look = p;
            while (*look && isspace((unsigned char)*look) && *look != '\n') look++;
            if (*look == '#') {
                p = look + 1;
                while (*p && isspace((unsigned char)*p)) p++;
                char directive[32]; size_t dl = 0;
                while (*p && isalpha((unsigned char)*p) && dl < 31) directive[dl++] = *p++;
                directive[dl] = '\0';
                /* Do not cross the line end: a bare directive (e.g. #endif,
                 * #else) must leave p at this line's newline. */
                while (*p && isspace((unsigned char)*p) && *p != '\n') p++;

                bool active = (cpp->cond_depth == 0 || cpp->cond_true[cpp->cond_depth]);

                if (strcmp(directive, "ifdef") == 0) {
                    char macro_name[128]; size_t ml = 0;
                    while (*p && (isalnum((unsigned char)*p) || *p == '_') && ml < 127) macro_name[ml++] = *p++;
                    macro_name[ml] = '\0';
                    cpp->cond_depth++;
                    bool val = active && (zero_cc_cpp_find_macro(cpp, macro_name) != NULL);
                    cpp->cond_true[cpp->cond_depth] = val;
                    cpp->cond_seen_true[cpp->cond_depth] = val;
                } else if (strcmp(directive, "ifndef") == 0) {
                    char macro_name[128]; size_t ml = 0;
                    while (*p && (isalnum((unsigned char)*p) || *p == '_') && ml < 127) macro_name[ml++] = *p++;
                    macro_name[ml] = '\0';
                    cpp->cond_depth++;
                    bool val = active && (zero_cc_cpp_find_macro(cpp, macro_name) == NULL);
                    cpp->cond_true[cpp->cond_depth] = val;
                    cpp->cond_seen_true[cpp->cond_depth] = val;
                } else if (strcmp(directive, "if") == 0) {
                    cpp->cond_depth++;
                    bool val = active && (zero_cc_cpp_eval_expr(cpp, p) != 0);
                    cpp->cond_true[cpp->cond_depth] = val;
                    cpp->cond_seen_true[cpp->cond_depth] = val;
                } else if (strcmp(directive, "elif") == 0) {
                    if (cpp->cond_depth > 0) {
                        bool prev_depth_active = (cpp->cond_depth == 1 || cpp->cond_true[cpp->cond_depth - 1]);
                        if (!cpp->cond_seen_true[cpp->cond_depth] && prev_depth_active && zero_cc_cpp_eval_expr(cpp, p) != 0) {
                            cpp->cond_true[cpp->cond_depth] = true;
                            cpp->cond_seen_true[cpp->cond_depth] = true;
                        } else {
                            cpp->cond_true[cpp->cond_depth] = false;
                        }
                    }
                } else if (strcmp(directive, "else") == 0) {
                    if (cpp->cond_depth > 0) {
                        bool prev_depth_active = (cpp->cond_depth == 1 || cpp->cond_true[cpp->cond_depth - 1]);
                        cpp->cond_true[cpp->cond_depth] = prev_depth_active && !cpp->cond_seen_true[cpp->cond_depth];
                    }
                } else if (strcmp(directive, "endif") == 0) {
                    if (cpp->cond_depth > 0) cpp->cond_depth--;
                } else if (active && strcmp(directive, "define") == 0) {
                    char macro_name[128]; size_t ml = 0;
                    while (*p && (isalnum((unsigned char)*p) || *p == '_') && ml < 127) macro_name[ml++] = *p++;
                    macro_name[ml] = '\0';

                    if (*p == '(') {
                        p++;
                        char *params[32];
                        int pcount = 0;
                        bool is_var = false;
                        while (*p && *p != ')') {
                            while (*p && (isspace((unsigned char)*p) || *p == ',')) p++;
                            if (*p == '.' && p[1] == '.' && p[2] == '.') {
                                is_var = true;
                                if (pcount < 32) params[pcount++] = strdup("__VA_ARGS__");
                                p += 3;
                                break;
                            }
                            char pname[128]; size_t pl = 0;
                            while (*p && (isalnum((unsigned char)*p) || *p == '_') && pl < 127) pname[pl++] = *p++;
                            pname[pl] = '\0';
                            if (*p == '.' && p[1] == '.' && p[2] == '.') {
                                is_var = true;
                                p += 3;
                                if (pl > 0 && pcount < 32) {
                                    params[pcount++] = strdup(pname);
                                }
                                break;
                            }
                            if (pl > 0 && pcount < 32) {
                                params[pcount++] = strdup(pname);
                            }
                        }
                        if (*p == ')') p++;
                        while (*p && isspace((unsigned char)*p) && *p != '\n') p++;
                        /* Build the body, splicing backslash-newline
                         * continuations (phase 2) out of the stored text. */
                        char *body = (char *)malloc(1024); size_t blen = 0;
                        while (*p && *p != '\n') {
                            if (*p == '\\' && p[1] == '\n') { p += 2; cpp->line_num++; continue; }
                            if (blen + 2 > 1024) {
                                size_t nc = (blen + 2) * 2;
                                char *nb = (char *)malloc(nc);
                                memcpy(nb, body, blen);
                                free(body);
                                body = nb;
                            }
                            body[blen++] = *p++;
                        }
                        body[blen] = '\0';

                        if (cpp->macro_count < ZERO_CC_MAX_MACROS) {
                            size_t idx = cpp->macro_count++;
                            ZeroCCMacro *m = &cpp->macros[idx];
                            memset(m,0,sizeof(*m));
                            snprintf(m->name, sizeof(m->name), "%s", macro_name);
                            m->param_count = pcount;
                            m->is_variadic = is_var;
                            for (int k = 0; k < pcount; k++) m->params[k] = params[k];
                            m->replacement = body;

                            uint32_t b = (uint32_t)(zero_cc_hash64(macro_name) & 4095);
                            cpp->hash_next[idx] = cpp->hash_buckets[b];
                            cpp->hash_buckets[b] = (int32_t)idx;
                        }
                    } else {
                        while (*p && isspace((unsigned char)*p) && *p != '\n') p++;
                        /* Object-macro value: splice backslash-newline
                         * continuations out (phase 2). */
                        char *val = (char *)malloc(1024); size_t vlen = 0;
                        while (*p && *p != '\n') {
                            if (*p == '\\' && p[1] == '\n') { p += 2; cpp->line_num++; continue; }
                            if (vlen + 2 > 1024) {
                                size_t nc = (vlen + 2) * 2;
                                char *nv = (char *)malloc(nc);
                                memcpy(nv, val, vlen);
                                free(val);
                                val = nv;
                            }
                            val[vlen++] = *p++;
                        }
                        val[vlen] = '\0';
                        zero_cc_cpp_define_obj(cpp, macro_name, val);
                        free(val);
                    }
                } else if (active && strcmp(directive, "undef") == 0) {
                    char macro_name[128]; size_t ml = 0;
                    while (*p && (isalnum((unsigned char)*p) || *p == '_') && ml < 127) macro_name[ml++] = *p++;
                    macro_name[ml] = '\0';
                    zero_cc_cpp_undef(cpp, macro_name);
                } else if (active && strcmp(directive, "include") == 0) {
                    bool is_sys = (*p == '<');
                    char delim_end = is_sys ? '>' : '"';
                    p++;
                    char inc_name[256]; size_t inl = 0;
                    while (*p && *p != delim_end && *p != '\n' && inl < 255) inc_name[inl++] = *p++;
                    inc_name[inl] = '\0';
                    if (*p == delim_end) p++;

                    char resolved_path[PATH_MAX];
                    int saved_inc_depth = 0;
                    char *inc_content = zero_cc_cpp_resolve_include(cpp, inc_name, is_sys, resolved_path, sizeof(resolved_path));
                    if (inc_content) {
                        saved_inc_depth = zero_cc_cpp_push_include_dir(cpp, resolved_path);
                        /* Track dependency for -MMD/-MF/-MT */
                        zero_cc_cpp_add_dependency(cpp, resolved_path);
                        /* Track the file being processed (Sprint 41):
                         * #pragma once dedup and __FILE__ expansion both
                         * key off cpp->filename.  resolved_path is valid
                         * for the duration of the nested process_text
                         * call; virtual headers leave it empty (NULL). */
                        char *saved_filename = cpp->filename;
                        cpp->filename = resolved_path[0] ? resolved_path : NULL;
                        zero_cc_cpp_process_text(cpp, inc_content, out);
                        cpp->filename = saved_filename;
                        zero_cc_cpp_pop_include_dir(cpp, saved_inc_depth);
                        free(inc_content);
                    } else if (cpp->include_error[0] == '\0') {
                        /* C99 6.10.2p1: if the named file cannot be found,
                         * a diagnostic is required (gcc: "fatal error").
                         * Record the first failure; the ext2 entry point
                         * aborts preprocessing with it instead of silently
                         * skipping the missing file. */
                        snprintf(cpp->include_error, sizeof(cpp->include_error),
                                 "%s:%zu: fatal error: %s: No such file or directory",
                                 cpp->filename ? cpp->filename : "<stdin>",
                                 cpp->line_num, inc_name);
                    }
                } else if (active && strcmp(directive, "pragma") == 0) {
                    while (*p && isspace((unsigned char)*p) && *p != '\n') p++;
                    if (strncmp(p, "once", 4) == 0 && isspace((unsigned char)p[4])) {
                        /* Handle #pragma once */
                        if (cpp->filename) {
                            int found = 0;
                            for (size_t i = 0; i < cpp->once_file_count; i++) {
                                if (strcmp(cpp->once_files[i], cpp->filename) == 0) {
                                    found = 1;
                                    break;
                                }
                            }
                            if (found) {
                                /* Skip the rest of the file */
                                while (*p) {
                                    if (*p == '\n') {
                                        cpp->line_num++;
                                    }
                                    p++;
                                }
                            } else {
                                /* Add to once files */
                                if (cpp->once_file_count < ONCE_FILES_MAX) {
                                    cpp->once_files[cpp->once_file_count] = strdup(cpp->filename);
                                    cpp->once_file_count++;
                                }
                            }
                        }
                        /* Skip the rest of the line */
                        while (*p && *p != '\n') p++;
                        if (*p == '\n') { zero_cc_buf_putc(out, *p++); at_line_start = true; cpp->line_num++; }
                        continue;
                    }
                    /* message / GCC diagnostic are handled here; others (pack, ...) go to the tokenizer */
                    {
                        const char *eol = p;
                        while (*eol && *eol != '\n') eol++;
                        if (zero_cc_cpp_handle_pragma(cpp, p, (size_t)(eol - p))) {
                            p = eol;
                            if (*p == '\n') { zero_cc_buf_putc(out, *p++); at_line_start = true; cpp->line_num++; }
                            continue;
                        }
                    }
                    zero_cc_buf_puts(out, "#pragma ");
                    while (*p && *p != '\n') zero_cc_buf_putc(out, *p++);
                    if (*p == '\n') { zero_cc_buf_putc(out, *p++); at_line_start = true; cpp->line_num++; }
                    continue;
                } else if (active && strcmp(directive, "embed") == 0) {
                    while (*p && isspace((unsigned char)*p) && *p != '\n') p++;
                    bool is_sys = (*p == '<');
                    char delim_end = is_sys ? '>' : '"';
                    if (*p == '"' || *p == '<') p++;
                    char asset_path[256]; size_t apl = 0;
                    while (*p && *p != delim_end && *p != '\n' && apl < 255) asset_path[apl++] = *p++;
                    asset_path[apl] = '\0';
                    if (*p == delim_end) p++;

                    FILE *f = fopen(asset_path, "rb");
                    if (!f && cpp) {
                        /* Try resolve via include search paths */
                        char resolved_path2[PATH_MAX];
                        char *resolved = zero_cc_cpp_resolve_include(cpp, asset_path, is_sys, resolved_path2, sizeof(resolved_path2));
                        if (resolved) {
                            /* read from resolved string as raw bytes */
                            size_t rlen = strlen(resolved);
                            for (size_t i = 0; i < rlen; i++) {
                                char buf[16];
                                snprintf(buf, sizeof(buf), "%u%s", (unsigned char)resolved[i], (i + 1 < rlen) ? ", " : "");
                                zero_cc_buf_puts(out, buf);
                            }
                            free(resolved);
                            f = NULL;
                        }
                    }
                    if (f) {
                        fseek(f, 0, SEEK_END);
                        long fsz = ftell(f);
                        fseek(f, 0, SEEK_SET);
                        if (fsz > 0) {
                            unsigned char *data = (unsigned char *)malloc((size_t)fsz);
                            if (data) {
                                size_t r = fread(data, 1, (size_t)fsz, f);
                                for (size_t i = 0; i < r; i++) {
                                    char buf[16];
                                    snprintf(buf, sizeof(buf), "%u%s", data[i], (i + 1 < r) ? ", " : "");
                                    zero_cc_buf_puts(out, buf);
                                }
                                free(data);
                            }
                        }
                        fclose(f);
                    }
                }

                while (*p && *p != '\n') {
                    if (*p == '\\' && p[1] == '\n') {
                        p += 2;
                        cpp->line_num++;
                        continue;
                    }
                    p++;
                }
                if (*p == '\n') p++;
                at_line_start = true; cpp->line_num++;
                continue;
            }
        }

        if (cpp->cond_depth > 0 && !cpp->cond_true[cpp->cond_depth]) {
            while (*p && *p != '\n') p++;
            if (*p == '\n') { p++; at_line_start = true; cpp->line_num++; }
            continue;
        }

        if (*p == '\n') { zero_cc_buf_putc(out, *p++); at_line_start = true; cpp->line_num++; continue; }
        at_line_start = false;

        /* String literals */
        if (*p == '"' || *p == '\'') {
            char quote = *p;
            zero_cc_buf_putc(out, *p++);
            while (*p && *p != quote) {
                if (*p == '\\' && p[1]) {
                    zero_cc_buf_putc(out, *p++);
                }
                zero_cc_buf_putc(out, *p++);
            }
            if (*p == quote) zero_cc_buf_putc(out, *p++);
            continue;
        }

        /* Identifiers or macro substitutions */
        if (isalpha((unsigned char)*p) || *p == '_') {
            char ident[128]; size_t il = 0;
            while (*p && (isalnum((unsigned char)*p) || *p == '_') && il < 127) ident[il++] = *p++;
            ident[il] = '\0';

            /* Handle special predefined macros with dynamic values */
            if (strcmp(ident, "__FILE__") == 0) {
                zero_cc_buf_putc(out, '"');
                const char *fn = cpp->filename ? cpp->filename : "";
                for (const char *f = fn; *f; f++) {
                    if (*f == '\\') {
                        zero_cc_buf_putc(out, '\\');
                        zero_cc_buf_putc(out, '\\');
                    } else if (*f == '"') {
                        zero_cc_buf_putc(out, '\\');
                        zero_cc_buf_putc(out, '"');
                    } else {
                        zero_cc_buf_putc(out, *f);
                    }
                }
                zero_cc_buf_putc(out, '"');
                continue;
            } else if (strcmp(ident, "__LINE__") == 0) {
                char line_buf[32];
                snprintf(line_buf, sizeof(line_buf), "%zu", cpp->line_num);
                zero_cc_buf_puts(out, line_buf);
                continue;
            } else if (strcmp(ident, "__func__") == 0) {
                /* __func__ is handled during AST lowering - emit as-is */
                zero_cc_buf_puts(out, "__func__");
                continue;
            } else if (strcmp(ident, "_Pragma") == 0) {
                /* Handle _Pragma operator */
                /* Skip whitespace and check for '(' */
                const char *p2 = p;
                while (*p2 && isspace((unsigned char)*p2)) p2++;
                if (*p2 == '(') {
                    p2++; /* skip '(' */
                    while (*p2 && isspace((unsigned char)*p2)) p2++;
                    /* Expect a string literal */
                    if (*p2 == '"') {
                        char quote = *p2;
                        const char *start = p2 + 1;
                        const char *end = start;
                        while (*end && *end != quote) {
                            if (*end == '\\' && end[1]) {
                                /* skip escaped character */
                                end += 2;
                            } else {
                                end++;
                            }
                        }
                        if (*end == quote) {
                            size_t len = end - start;
                            char *temp = malloc(len + 1);
                            if (temp) {
                                memcpy(temp, start, len);
                                temp[len] = '\0';
                                /* Process escape sequences in the string */
                                char *processed = malloc(len + 1); /* worst case */
                                size_t processed_len = 0;
                                const char *s = temp;
                                while (*s) {
                                    if (*s == '\\' && s[1]) {
                                        s++; /* point at the escaped character */
                                        char esc = 0;
                                        int val = 0;
                                        if (!zero_cc_handle_escape(&s, NULL, &esc, &val)) {
                                            esc = (unsigned char)*s;
                                        }
                                        processed[processed_len++] = (char)val;
                                        s++;
                                    } else {
                                        processed[processed_len++] = *s++;
                                    }
                                }
                                processed[processed_len] = '\0';
                                /* Now we have the processed string without quotes, with escape sequences applied.
                                 * We need to treat this as the text after a #pragma directive.
                                 * That means we need to process it as if it were the rest of a line after "#pragma ".
                                 * We'll handle #pragma once only.
                                 */
                                const char *pragma_text = processed;
                                while (*pragma_text && isspace((unsigned char)*pragma_text)) pragma_text++;
                                if (strncmp(pragma_text, "once", 4) == 0 && isspace((unsigned char)pragma_text[4])) {
                                    /* Handle #pragma once */
                                    if (cpp->filename) {
                                        int found = 0;
                                        for (size_t i = 0; i < cpp->once_file_count; i++) {
                                            if (strcmp(cpp->once_files[i], cpp->filename) == 0) {
                                                found = 1;
                                                break;
                                            }
                                        }
                                        if (found) {
                                            /* Skip the rest of the file */
                                            while (*p) {
                                                if (*p == '\n') {
                                                    cpp->line_num++;
                                                }
                                                p++;
                                            }
                                            free(temp);
                                            free(processed);
                                            continue; /* skip the rest of the loop */
                                        } else {
                                            /* Add to once files */
                                            if (cpp->once_file_count < ONCE_FILES_MAX) {
                                                cpp->once_files[cpp->once_file_count] = strdup(cpp->filename);
                                                cpp->once_file_count++;
                                            }
                                        }
                                    }
                                } else {
                                /* Forward other pragmas (e.g. pack) to the tokenizer as a #pragma line */
                                const char *close = end + 1;
                                while (*close && isspace((unsigned char)*close)) close++;
                                if (*close == ')' && zero_cc_cpp_handle_pragma(cpp, pragma_text, strlen(pragma_text))) {
                                    p = close + 1;
                                    free(temp);
                                    free(processed);
                                    continue;
                                }
                                if (*close == ')') {
                                    zero_cc_buf_puts(out, "\n#pragma ");
                                    zero_cc_buf_puts(out, pragma_text);
                                    zero_cc_buf_puts(out, "\n");
                                    p = close + 1;
                                    free(temp);
                                    free(processed);
                                    continue;
                                }
                                }
                                free(temp);
                                free(processed);
                            }
                        }
                    }
                }
                /* If we didn't handle it, fall back to treating _Pragma as an identifier */
                zero_cc_buf_puts(out, ident);
                continue;
            }

            ZeroCCMacro *m = zero_cc_cpp_find_macro(cpp, ident);
            if (m) {
                if (m->param_count < 0) {
                    int prev_expanding = cpp->expanding_macro_index;
                    cpp->expanding_macro_index = (int)(m - cpp->macros);
                    zero_cc_cpp_process_text(cpp, m->replacement, out);
                    cpp->expanding_macro_index = prev_expanding;
                } else {
                    const char *ws_skip = p;
                    size_t ws_newlines = 0;
                    while (*ws_skip && isspace((unsigned char)*ws_skip)) {
                        if (*ws_skip == '\n') ws_newlines++;
                        ws_skip++;
                    }
                    if (*ws_skip == '(') {
                        p = ws_skip + 1;
                        cpp->line_num += ws_newlines;
                        char *args[64];
                        int arg_count = 0;
                        while (*p && *p != ')') {
                            while (*p && isspace((unsigned char)*p)) p++;
                            const char *arg_s = p;
                            int paren_nest = 0;
                            while (*p && (paren_nest > 0 || (*p != ',' && *p != ')'))) {
                                if (*p == '(') paren_nest++;
                                else if (*p == ')') paren_nest--;
                                p++;
                            }
                            size_t arg_l = (size_t)(p - arg_s);
                            while (arg_l > 0 && isspace((unsigned char)arg_s[arg_l - 1])) arg_l--;
                            char *arg_str = (char *)malloc(arg_l + 1);
                        if (arg_str) {
                            memcpy(arg_str, arg_s, arg_l);
                            arg_str[arg_l] = '\0';
                            if (arg_count < 64) {
                                args[arg_count++] = arg_str;
                            } else {
                                free(arg_str);
                            }
                        }
                        if (*p == ',') p++;
                    }
                    if (*p == ')') p++;

                    /* Bundle variadic arguments into __VA_ARGS__ / named variadic param */
                    if (m->is_variadic && m->param_count > 0) {
                        if (arg_count >= m->param_count) {
                            size_t total_va_len = 0;
                            for (int k = m->param_count - 1; k < arg_count; k++) {
                                total_va_len += strlen(args[k]) + 4;
                            }
                            char *va_str = (char *)malloc(total_va_len + 1);
                            if (va_str) {
                                va_str[0] = '\0';
                                for (int k = m->param_count - 1; k < arg_count; k++) {
                                    if (k > m->param_count - 1) strcat(va_str, ", ");
                                    strcat(va_str, args[k]);
                                    free(args[k]);
                                }
                                args[m->param_count - 1] = va_str;
                                arg_count = m->param_count;
                            }
                        } else if (arg_count == m->param_count - 1 && arg_count < 32) {
                            args[m->param_count - 1] = strdup("");
                            arg_count = m->param_count;
                        }
                    }

                    /* Token pasting buffer */
                    char paste_buf[512];
                    size_t paste_len = 0;
                    bool pending_paste = false;
                    paste_buf[0] = '\0';

                    const char *b = m->replacement;
                    ZeroCCBuffer temp_buf;
                    zero_cc_buf_init(&temp_buf, 256);
                    int prev_expanding = cpp->expanding_macro_index;
                    cpp->expanding_macro_index = (int)(m - cpp->macros);
                    while (*b) {
                        /* Token pasting: ## (C11 6.10.3.3): whitespace on
                         * both sides of ## is discarded so the adjacent
                         * argument texts form ONE pasted token
                         * (`a ## b` with v,1 -> `v1`, not `v 1`). */
                        if (b[0] == '#' && b[1] == '#') {
                            while (temp_buf.length > 0 &&
                                   (temp_buf.data[temp_buf.length - 1] == ' ' ||
                                    temp_buf.data[temp_buf.length - 1] == '\t')) {
                                temp_buf.length--;
                                temp_buf.data[temp_buf.length] = '\0';
                            }
                            b += 2;
                            while (*b == ' ' || *b == '\t') b++;
                            paste_len = 0;
                            paste_buf[0] = '\0';
                            pending_paste = true;
                            continue;
                        }

                        /* Stringification: #param */
                        if (*b == '#' && (isalpha((unsigned char)b[1]) || b[1] == '_')) {
                            b++;
                            char param_id[128]; size_t pl = 0;
                            while (*b && (isalnum((unsigned char)*b) || *b == '_') && pl < 127) param_id[pl++] = *b++;
                            param_id[pl] = '\0';
                            int match = -1;
                            for (int k = 0; k < m->param_count; k++) {
                                if (strcmp(m->params[k], param_id) == 0) { match = k; break; }
                            }
                            if (strcmp(param_id, "__VA_ARGS__") == 0 && m->is_variadic && m->param_count > 0) {
                                match = m->param_count - 1;
                            }
                            if (match >= 0 && match < arg_count) {
                                if (pending_paste) {
                                    /* Paste then stringify */
                                    paste_len += snprintf(paste_buf + paste_len, sizeof(paste_buf) - paste_len, "%s", args[match]);
                                    zero_cc_buf_putc(&temp_buf, '"');
                                    zero_cc_buf_puts(&temp_buf, paste_buf);
                                    zero_cc_buf_putc(&temp_buf, '"');
                                    paste_len = 0;
                                    paste_buf[0] = '\0';
                                    pending_paste = false;
                                } else {
                                    /* Stringification: emit a string literal */
                                    zero_cc_buf_putc(&temp_buf, '"');
                                    zero_cc_buf_puts(&temp_buf, args[match]);
                                    zero_cc_buf_putc(&temp_buf, '"');
                                }
                            } else {
                                if (pending_paste) {
                                    paste_len += snprintf(paste_buf + paste_len, sizeof(paste_buf) - paste_len, "%s", param_id);
                                    zero_cc_buf_putc(&temp_buf, '"');
                                    zero_cc_buf_puts(&temp_buf, paste_buf);
                                    zero_cc_buf_putc(&temp_buf, '"');
                                    paste_len = 0;
                                    paste_buf[0] = '\0';
                                    pending_paste = false;
                                } else {
                                    zero_cc_buf_putc(&temp_buf, '"');
                                    zero_cc_buf_puts(&temp_buf, param_id);
                                    zero_cc_buf_putc(&temp_buf, '"');
                                }
                            }
                        } else if (isalpha((unsigned char)*b) || *b == '_') {
                            char param_id[128]; size_t pl = 0;
                            while (*b && (isalnum((unsigned char)*b) || *b == '_') && pl < 127) param_id[pl++] = *b++;
                            param_id[pl] = '\0';
                            int match = -1;
                            for (int k = 0; k < m->param_count; k++) {
                                if (strcmp(m->params[k], param_id) == 0) { match = k; break; }
                            }
                            if (strcmp(param_id, "__VA_ARGS__") == 0 && m->is_variadic && m->param_count > 0) {
                                match = m->param_count - 1;
                            }
                            if (match >= 0 && match < arg_count) {
                                if (pending_paste) {
                                    paste_len += snprintf(paste_buf + paste_len, sizeof(paste_buf) - paste_len, "%s", args[match]);
                                    /* Flush immediately: a second `##` in the
                                     * same body must start a fresh paste. */
                                    zero_cc_buf_puts(&temp_buf, paste_buf);
                                    paste_len = 0;
                                    paste_buf[0] = '\0';
                                    pending_paste = false;
                                } else {
                                    zero_cc_buf_puts(&temp_buf, args[match]);
                                }
                            } else {
                                if (pending_paste) {
                                    paste_len += snprintf(paste_buf + paste_len, sizeof(paste_buf) - paste_len, "%s", param_id);
                                    zero_cc_buf_puts(&temp_buf, paste_buf);
                                    paste_len = 0;
                                    paste_buf[0] = '\0';
                                    pending_paste = false;
                                } else {
                                    zero_cc_buf_puts(&temp_buf, param_id);
                                }
                            }
                        } else if (pending_paste && (*b >= '0' && *b <= '9')) {
                            /* Digit after ## - collect full number */
                            while (*b && ((*b >= '0' && *b <= '9') || *b == '.' || *b == 'x' || *b == 'X' ||
                                   *b == 'a' || *b == 'b' || *b == 'c' || *b == 'd' || *b == 'e' || *b == 'f' ||
                                   *b == 'A' || *b == 'B' || *b == 'C' || *b == 'D' || *b == 'E' || *b == 'F' ||
                                   *b == 'u' || *b == 'U' || *b == 'l' || *b == 'L')) {
                                paste_len += snprintf(paste_buf + paste_len, sizeof(paste_buf) - paste_len, "%c", *b++);
                            }
                            zero_cc_buf_puts(&temp_buf, paste_buf);
                            paste_len = 0;
                            paste_buf[0] = '\0';
                            pending_paste = false;
                        } else if (pending_paste) {
                            /* Other char after ## - paste single char */
                            paste_len += snprintf(paste_buf + paste_len, sizeof(paste_buf) - paste_len, "%c", *b++);
                            zero_cc_buf_puts(&temp_buf, paste_buf);
                            paste_len = 0;
                            paste_buf[0] = '\0';
                            pending_paste = false;
                        } else {
                            zero_cc_buf_putc(&temp_buf, *b++);
                        }
                    }
                    /* Any still-pending paste (## at end of body) has no
                     * operand; the text is discarded (constraint violation). */
                    zero_cc_cpp_process_text(cpp, temp_buf.data, out);
                    zero_cc_buf_free(&temp_buf);
                        cpp->expanding_macro_index = prev_expanding;
                        for (int k = 0; k < arg_count; k++) free(args[k]);
                    } else {
                        zero_cc_buf_puts(out, ident);
                        continue;
                    }
                }
            } else {
                zero_cc_buf_puts(out, ident);
                continue;
            }
            continue;
        }

        zero_cc_buf_putc(out, *p++);
    }
}

typedef struct {
    char *name;
    char *value;
} ZeroCCMacroDef;

typedef struct {
    ZeroCCMacroDef defs[128];
    int undef[128]; /* 1 = -U entry (name only, undefine), 0 = -D entry */
    size_t count;
} ZeroCCDefines;

/* Full-control entry point with dependency generation support
 * (-MMD/-MF/-MT) and -include force-includes. */
static inline char* zero_cc_preprocess_ext3(const char *source, ZeroCCIncludeDirs *inc_dirs,
                                            ZeroCCDefines *defines, const char *main_file,
                                            char *err_out, size_t err_cap,
                                            const char *dep_target, const char *dep_file,
                                            int gen_deps,
                                            const char **force_includes,
                                            int force_include_count) {
    ZeroCCPreprocessor *cpp = (ZeroCCPreprocessor *)calloc(1, sizeof(ZeroCCPreprocessor));
    if (!cpp) return strdup(source ? source : "");
    zero_cc_cpp_init(cpp);
    zero_cc_cpp_define_builtin_macros(cpp);
    if (dep_target && dep_target[0]) {
        cpp->dep_target = strdup(dep_target);
    }
    if (defines) {
        for (size_t i = 0; i < defines->count; i++) {
            if (defines->defs[i].name) {
                if (defines->undef[i]) {
                    zero_cc_cpp_undefine(cpp, defines->defs[i].name);
                } else {
                    zero_cc_cpp_define_obj(cpp, defines->defs[i].name, defines->defs[i].value ? defines->defs[i].value : "1");
                }
            }
        }
    }
    if (inc_dirs) {
        for (size_t i = 0; i < inc_dirs->count; i++) {
            zero_cc_cpp_add_include_dir(cpp, inc_dirs->paths[i]);
        }
    }
    if (main_file && main_file[0]) {
        /* Main file identity for __FILE__ / #pragma once, and its directory
         * as the first search location for quoted includes (C99 6.10.2p2). */
        cpp->filename = (char *)main_file;
        zero_cc_cpp_push_include_dir(cpp, main_file);
        /* Track main file as a dependency */
        zero_cc_cpp_add_dependency(cpp, main_file);
    }
    ZeroCCBuffer out;
    zero_cc_buf_init(&out, 4096);

    /* -include file: processed as if '#include "file"' appeared as the
     * first line of the main source file (gcc semantics).  Virtual
     * freestanding headers resolve first, so '-include stdarg.h' works. */
    for (int fi = 0; force_includes && fi < force_include_count; fi++) {
        char resolved_path[PATH_MAX];
        char *inc_content = zero_cc_cpp_resolve_include(cpp, force_includes[fi], false,
                                                        resolved_path, sizeof(resolved_path));
        if (inc_content) {
            int saved_inc_depth = zero_cc_cpp_push_include_dir(cpp, resolved_path);
            zero_cc_cpp_add_dependency(cpp, resolved_path);
            char *saved_filename = cpp->filename;
            cpp->filename = resolved_path[0] ? resolved_path : NULL;
            zero_cc_cpp_process_text(cpp, inc_content, &out);
            cpp->filename = saved_filename;
            zero_cc_cpp_pop_include_dir(cpp, saved_inc_depth);
            free(inc_content);
        } else if (cpp->include_error[0] == '\0') {
            snprintf(cpp->include_error, sizeof(cpp->include_error),
                     "<command-line>: fatal error: %s: No such file or directory",
                     force_includes[fi]);
        }
    }

    zero_cc_cpp_process_text(cpp, source, &out);
    int include_failed = cpp->include_error[0] != '\0';
    if (include_failed && err_out && err_cap) {
        snprintf(err_out, err_cap, "%s", cpp->include_error);
    }
    if (!include_failed && gen_deps && dep_file && dep_file[0]) {
        zero_cc_cpp_write_dep_file(cpp, dep_file);
    }
    zero_cc_cpp_publish_diag_state(cpp);
    zero_cc_cpp_destroy(cpp);
    free(cpp);
    if (include_failed) {
        free(out.data);
        return NULL;
    }
    return out.data;
}

/* Full-control entry point (Sprint 42): `main_file` names the command-line
 * source file.  Per C99 6.10.2p2 a quoted include in the main file is
 * searched first in the main file's own directory, and `__FILE__` in the
 * main file expands to this path.  If any include file cannot be found,
 * the return is NULL and `err_out` (when provided) receives the diagnostic.
 * main_file=NULL / err_out=NULL reproduces the legacy zero_cc_preprocess_ext
 * behavior exactly (missing includes are skipped, no error channel). */
static inline char* zero_cc_preprocess_ext2(const char *source, ZeroCCIncludeDirs *inc_dirs,
                                            ZeroCCDefines *defines, const char *main_file,
                                            char *err_out, size_t err_cap) {
    return zero_cc_preprocess_ext3(source, inc_dirs, defines, main_file, err_out, err_cap,
                                   NULL, NULL, 0, NULL, 0);
}

static inline char* zero_cc_preprocess_ext(const char *source, ZeroCCIncludeDirs *inc_dirs, ZeroCCDefines *defines) {
    return zero_cc_preprocess_ext2(source, inc_dirs, defines, NULL, NULL, 0);
}

static inline char* zero_cc_preprocess(const char *source, ZeroCCIncludeDirs *inc_dirs) {
    return zero_cc_preprocess_ext(source, inc_dirs, NULL);
}




#endif /* ZERO_CC_CORE_PREPROCESSOR_H */
