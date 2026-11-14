#include "zero_cc/core/preprocessor.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "zero_cc/core/utils.h"
#include "zero_cc/core/buffers.h"
#include "zero_cc/core/compiler_state.h"
#include "zero_cc/core/cpp_expr.h"
#include "zero_cc/core/virtual_headers.h"
#include "zero_cc/lexer/scan.h"

void zero_cc_cpp_init(ZeroCCPreprocessor *cpp) {
    memset(cpp, 0, sizeof(*cpp));
    cpp->macro_capacity = 512;
    cpp->macros = (ZeroCCMacro *)calloc(cpp->macro_capacity, sizeof(ZeroCCMacro));
    cpp->hash_next = (int32_t *)malloc(cpp->macro_capacity * sizeof(int32_t));
    for (int i = 0; i < 4096; i++) cpp->hash_buckets[i] = -1;
    cpp->cond_depth = 0;
    cpp->cond_true[0] = true;
    cpp->cond_seen_true[0] = true;
    cpp->expanding_top = -1;
    cpp->default_pack = 0;
    cpp->pack_stack_depth = 0;
    cpp->diagnostic_state.warning_as_error = false;
    cpp->diagnostic_state.disabled_warning_count = 0;
    cpp->diagnostic_state.stack_depth = 0;
}

void zero_cc_cpp_destroy(ZeroCCPreprocessor *cpp) {
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

void zero_cc_cpp_add_include_dir(ZeroCCPreprocessor *cpp, const char *path) {
    if (cpp->include_dirs.count < ZERO_CC_MAX_INCLUDE_PATHS && path) {
        cpp->include_dirs.paths[cpp->include_dirs.count++] = strdup(path);
    }
}

ZeroCCMacro* zero_cc_cpp_find_macro(ZeroCCPreprocessor *cpp, const char *name) {
    if (!cpp || !name || cpp->macro_count == 0) return NULL;
    uint32_t b = (uint32_t)(zero_cc_hash64(name) & 4095);
    int32_t idx = cpp->hash_buckets[b];
    while (idx >= 0 && (size_t)idx < cpp->macro_count) {
        if (strcmp(cpp->macros[idx].name, name) == 0) {
            /* Blue-paint (C99 6.10.3.4p2): inhibit expansion of ANY macro
             * already on the expansion stack - the outer macro stays blue
             * through nested expansions, not just the innermost one. */
            for (int32_t i = cpp->expanding_top; i >= 0; i--) {
                if (cpp->expanding_stack[i] == idx) return NULL;
            }
            return &cpp->macros[idx];
        }
        idx = cpp->hash_next[idx];
    }
    return NULL;
}

void zero_cc_cpp_define_obj(ZeroCCPreprocessor *cpp, const char *name, const char *value) {
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
void zero_cc_cpp_undefine(ZeroCCPreprocessor *cpp, const char *name) {
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

void zero_cc_cpp_define_func1(ZeroCCPreprocessor *cpp, const char *name, const char *p1, const char *value) {
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

void zero_cc_cpp_define_func2(ZeroCCPreprocessor *cpp, const char *name, const char *p1, const char *p2, const char *value) {
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

void zero_cc_cpp_define_func3(ZeroCCPreprocessor *cpp, const char *name, const char *p1, const char *p2, const char *p3, const char *value) {
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

void zero_cc_cpp_define_builtin_macros(ZeroCCPreprocessor *cpp, const char *std_version) {
    zero_cc_cpp_define_obj(cpp, "__STDC__", "1");
    /* __STDC_VERSION__ follows the -std= dialect:
     *   c99/gnu99  -> 199901L (C99)
     *   c11/gnu11  -> 201112L (C11)
     *   c17/gnu17  -> 201112L (C17 is C11 + defect fixes)
     *   c23/gnu23  -> 202311L (C23)
     * The dialect only *enables* keywords and version macros; it never
     * *rejects* syntax (gnu89/C89 leniency wins for gate stability). */
    zero_cc_cpp_define_obj(cpp, "__STDC_VERSION__", std_version);
    zero_cc_cpp_define_obj(cpp, "__STDC_EMBED_NOT_FOUND__", "0");
    zero_cc_cpp_define_obj(cpp, "__STDC_EMBED_FOUND__", "1");
    zero_cc_cpp_define_obj(cpp, "__STDC_EMBED_EMPTY__", "2");
    /* __STDC_NO_COMPLEX__ must NOT be defined: _Complex float/double and
     * GNU integer _Complex are implemented (Sprint 85) - defining it made
     * `#ifdef __STDC_NO_COMPLEX__` take the wrong branch. Per C11/C23 it
     * is only defined when the feature is absent. */
    zero_cc_cpp_define_obj(cpp, "__zerocc__", "1");
    zero_cc_cpp_define_obj(cpp, "__ZEROCC__", "1");
    zero_cc_cpp_define_obj(cpp, "__GNUC__", "4");
    zero_cc_cpp_define_obj(cpp, "__GNUC_MINOR__", "2");
    zero_cc_cpp_define_obj(cpp, "__linux__", "1");
    zero_cc_cpp_define_obj(cpp, "__unix__", "1");
    /* GNU predefined: the assembler label prefix for this target (empty on
     * all supported Linux ISAs; `_` on macOS). Empty replacement: expands
     * to nothing. c-torture execute/builtins/{strstr,memops}-asm build the
     * `__asm("...")` symbol with ASMNAME(__USER_LABEL_PREFIX__, name) -
     * undefined, the identifier survives stringification and the symbol
     * becomes "__USER_LABEL_PREFIX__my_X". */
    zero_cc_cpp_define_obj(cpp, "__USER_LABEL_PREFIX__", "");
    zero_cc_cpp_define_obj(cpp, "NULL", "((void*)0)");
    /* C99 6.10.3.2 / 6.10.8 + GCC-style double-underscore predefined
     * macros (LP64 Linux, signed char). Literal values, not recursive:
     * the preprocessor does not self-reference macro definitions. */
    zero_cc_cpp_define_obj(cpp, "CHAR_BIT", "8");
    zero_cc_cpp_define_obj(cpp, "__CHAR_BIT__", "8");
    zero_cc_cpp_define_obj(cpp, "__CHAR_WIDTH__", "8");
    zero_cc_cpp_define_obj(cpp, "__SCHAR_MIN__", "(-128)");
    zero_cc_cpp_define_obj(cpp, "__SCHAR_MAX__", "127");
    zero_cc_cpp_define_obj(cpp, "__SCHAR_WIDTH__", "8");
    zero_cc_cpp_define_obj(cpp, "__CHAR_MIN__", "(-128)");
    zero_cc_cpp_define_obj(cpp, "__CHAR_MAX__", "127");
    zero_cc_cpp_define_obj(cpp, "__SHRT_MIN__", "(-32768)");
    zero_cc_cpp_define_obj(cpp, "__SHRT_MAX__", "32767");
    zero_cc_cpp_define_obj(cpp, "__SHRT_WIDTH__", "16");
    zero_cc_cpp_define_obj(cpp, "__INT_MAX__", "2147483647");
    zero_cc_cpp_define_obj(cpp, "__INT_MIN__", "(-2147483647-1)");
    zero_cc_cpp_define_obj(cpp, "__INT_WIDTH__", "32");
    zero_cc_cpp_define_obj(cpp, "__LONG_MAX__", "9223372036854775807L");
    zero_cc_cpp_define_obj(cpp, "__LONG_MIN__", "(-9223372036854775807L-1)");
    zero_cc_cpp_define_obj(cpp, "__LONG_WIDTH__", "64");
    zero_cc_cpp_define_obj(cpp, "__LONG_LONG_MAX__", "9223372036854775807LL");
    zero_cc_cpp_define_obj(cpp, "__LONG_LONG_MIN__", "(-9223372036854775807LL-1)");
    zero_cc_cpp_define_obj(cpp, "__LONG_LONG_WIDTH__", "64");
    zero_cc_cpp_define_obj(cpp, "__SIZE_MAX__", "18446744073709551615UL");
    zero_cc_cpp_define_obj(cpp, "__PTRDIFF_MAX__", "9223372036854775807L");
    zero_cc_cpp_define_obj(cpp, "__INTPTR_MAX__", "9223372036854775807L");
    zero_cc_cpp_define_obj(cpp, "__UINTPTR_MAX__", "18446744073709551615UL");
    zero_cc_cpp_define_obj(cpp, "__INTMAX_MAX__", "9223372036854775807L");
    zero_cc_cpp_define_obj(cpp, "__UINTMAX_MAX__", "18446744073709551615UL");
    zero_cc_cpp_define_obj(cpp, "__SIZEOF_SHORT__", "2");
    zero_cc_cpp_define_obj(cpp, "__SIZEOF_INT__", "4");
    zero_cc_cpp_define_obj(cpp, "__SIZEOF_LONG__", "8");
    zero_cc_cpp_define_obj(cpp, "__SIZEOF_LONG_LONG__", "8");
    zero_cc_cpp_define_obj(cpp, "__SIZEOF_FLOAT__", "4");
    zero_cc_cpp_define_obj(cpp, "__SIZEOF_DOUBLE__", "8");
    zero_cc_cpp_define_obj(cpp, "__SIZEOF_POINTER__", "8");
    zero_cc_cpp_define_obj(cpp, "__SIZEOF_SIZE_T__", "8");
    zero_cc_cpp_define_obj(cpp, "__SIZEOF_PTRDIFF_T__", "8");
    zero_cc_cpp_define_obj(cpp, "__SIZEOF_LONG_DOUBLE__", "16");
    zero_cc_cpp_define_obj(cpp, "__SIZEOF_WCHAR_T__", "4");
    zero_cc_cpp_define_obj(cpp, "__SIZEOF_WINT_T__", "4");
    /* GCC-style __*_TYPE__ macros (LP64 Linux). Undefined in the
     * preprocessor, these silently became `int` base types and
     * miscompiled stdint-style code (c-torture execute/pr42833). */
    zero_cc_cpp_define_obj(cpp, "__INT8_TYPE__", "signed char");
    zero_cc_cpp_define_obj(cpp, "__INT16_TYPE__", "short");
    zero_cc_cpp_define_obj(cpp, "__INT32_TYPE__", "int");
    zero_cc_cpp_define_obj(cpp, "__INT64_TYPE__", "long int");
    zero_cc_cpp_define_obj(cpp, "__UINT8_TYPE__", "unsigned char");
    zero_cc_cpp_define_obj(cpp, "__UINT16_TYPE__", "unsigned short");
    zero_cc_cpp_define_obj(cpp, "__UINT32_TYPE__", "unsigned int");
    zero_cc_cpp_define_obj(cpp, "__UINT64_TYPE__", "long unsigned int");
    zero_cc_cpp_define_obj(cpp, "__INT_LEAST8_TYPE__", "signed char");
    zero_cc_cpp_define_obj(cpp, "__INT_LEAST16_TYPE__", "short");
    zero_cc_cpp_define_obj(cpp, "__INT_LEAST32_TYPE__", "int");
    zero_cc_cpp_define_obj(cpp, "__INT_LEAST64_TYPE__", "long int");
    zero_cc_cpp_define_obj(cpp, "__UINT_LEAST8_TYPE__", "unsigned char");
    zero_cc_cpp_define_obj(cpp, "__UINT_LEAST16_TYPE__", "unsigned short");
    zero_cc_cpp_define_obj(cpp, "__UINT_LEAST32_TYPE__", "unsigned int");
    zero_cc_cpp_define_obj(cpp, "__UINT_LEAST64_TYPE__", "long unsigned int");
    zero_cc_cpp_define_obj(cpp, "__INT_FAST8_TYPE__", "long int");
    zero_cc_cpp_define_obj(cpp, "__INT_FAST16_TYPE__", "long int");
    zero_cc_cpp_define_obj(cpp, "__INT_FAST32_TYPE__", "long int");
    zero_cc_cpp_define_obj(cpp, "__INT_FAST64_TYPE__", "long int");
    zero_cc_cpp_define_obj(cpp, "__UINT_FAST8_TYPE__", "long unsigned int");
    zero_cc_cpp_define_obj(cpp, "__UINT_FAST16_TYPE__", "long unsigned int");
    zero_cc_cpp_define_obj(cpp, "__UINT_FAST32_TYPE__", "long unsigned int");
    zero_cc_cpp_define_obj(cpp, "__UINT_FAST64_TYPE__", "long unsigned int");
    zero_cc_cpp_define_obj(cpp, "__INTMAX_TYPE__", "long int");
    zero_cc_cpp_define_obj(cpp, "__UINTMAX_TYPE__", "long unsigned int");
    zero_cc_cpp_define_obj(cpp, "__INTPTR_TYPE__", "long int");
    zero_cc_cpp_define_obj(cpp, "__UINTPTR_TYPE__", "long unsigned int");
    zero_cc_cpp_define_obj(cpp, "SCHAR_MIN", "(-128)");
    zero_cc_cpp_define_obj(cpp, "SCHAR_MAX", "127");
    zero_cc_cpp_define_obj(cpp, "CHAR_MIN", "(-128)");
    zero_cc_cpp_define_obj(cpp, "CHAR_MAX", "127");
    zero_cc_cpp_define_obj(cpp, "SHRT_MIN", "(-32768)");
    zero_cc_cpp_define_obj(cpp, "SHRT_MAX", "32767");
    zero_cc_cpp_define_obj(cpp, "INT_MIN", "(-2147483647-1)");
    zero_cc_cpp_define_obj(cpp, "INT_MAX", "2147483647");
    zero_cc_cpp_define_obj(cpp, "LONG_MIN", "(-9223372036854775807L-1)");
    zero_cc_cpp_define_obj(cpp, "LONG_MAX", "9223372036854775807L");
    zero_cc_cpp_define_obj(cpp, "SCHAR_WIDTH", "8");
    zero_cc_cpp_define_obj(cpp, "CHAR_WIDTH", "8");
    zero_cc_cpp_define_obj(cpp, "SHRT_WIDTH", "16");
    zero_cc_cpp_define_obj(cpp, "INT_WIDTH", "32");
    zero_cc_cpp_define_obj(cpp, "LONG_WIDTH", "64");
    /* FLT/DBL limits (C99 6.10.3.3 / 6.10.8, IEEE-754) */
    zero_cc_cpp_define_obj(cpp, "__FLT_RADIX__", "2");
    zero_cc_cpp_define_obj(cpp, "__FLT_MANT_DIG__", "24");
    zero_cc_cpp_define_obj(cpp, "__FLT_DIG__", "6");
    zero_cc_cpp_define_obj(cpp, "__FLT_DECIMAL_DIG__", "9");
    zero_cc_cpp_define_obj(cpp, "__FLT_EPSILON__", "1.19209289550781250000e-7F");
    zero_cc_cpp_define_obj(cpp, "__FLT_HAS_DENORM__", "1");
    zero_cc_cpp_define_obj(cpp, "__FLT_HAS_INFINITY__", "1");
    zero_cc_cpp_define_obj(cpp, "__FLT_HAS_QUIET_NAN__", "1");
    zero_cc_cpp_define_obj(cpp, "__FLT_MAX_EXP__", "128");
    zero_cc_cpp_define_obj(cpp, "__FLT_MIN_EXP__", "(-125)");
    zero_cc_cpp_define_obj(cpp, "__FLT_MAX_10_EXP__", "38");
    zero_cc_cpp_define_obj(cpp, "__FLT_MIN_10_EXP__", "(-37)");
    zero_cc_cpp_define_obj(cpp, "__FLT_MAX__", "3.40282346638528859812e+38F");
    zero_cc_cpp_define_obj(cpp, "__FLT_MIN__", "1.17549435082228750797e-38F");
    zero_cc_cpp_define_obj(cpp, "__FLT_DENORM_MIN__", "1.40129846432481707082e-45F");
    zero_cc_cpp_define_obj(cpp, "__DBL_RADIX__", "2");
    zero_cc_cpp_define_obj(cpp, "__DBL_MANT_DIG__", "53");
    zero_cc_cpp_define_obj(cpp, "__DBL_DIG__", "15");
    zero_cc_cpp_define_obj(cpp, "__DBL_DECIMAL_DIG__", "17");
    zero_cc_cpp_define_obj(cpp, "__DBL_EPSILON__", "2.22044604925031308085e-16");
    zero_cc_cpp_define_obj(cpp, "__DBL_HAS_DENORM__", "1");
    zero_cc_cpp_define_obj(cpp, "__DBL_HAS_INFINITY__", "1");
    zero_cc_cpp_define_obj(cpp, "__DBL_HAS_QUIET_NAN__", "1");
    zero_cc_cpp_define_obj(cpp, "__DBL_MAX_EXP__", "1024");
    zero_cc_cpp_define_obj(cpp, "__DBL_MIN_EXP__", "(-1021)");
    zero_cc_cpp_define_obj(cpp, "__DBL_MAX_10_EXP__", "308");
    zero_cc_cpp_define_obj(cpp, "__DBL_MIN_10_EXP__", "(-307)");
    zero_cc_cpp_define_obj(cpp, "__DBL_MAX__", "1.7976931348623157081452742373170435679807e+308");
    zero_cc_cpp_define_obj(cpp, "__DBL_MIN__", "2.22507385850720138309e-308");
    zero_cc_cpp_define_obj(cpp, "__DBL_DENORM_MIN__", "4.94065645841246544177e-324");
    /* long double is a 16-byte object lowered through the 64-bit double
     * path (-mlong-double-64 model): LDBL_* limits mirror the double set. */
    zero_cc_cpp_define_obj(cpp, "__LDBL_MANT_DIG__", "53");
    zero_cc_cpp_define_obj(cpp, "__LDBL_DIG__", "15");
    zero_cc_cpp_define_obj(cpp, "__LDBL_DECIMAL_DIG__", "17");
    zero_cc_cpp_define_obj(cpp, "__LDBL_EPSILON__", "2.22044604925031308085e-16L");
    zero_cc_cpp_define_obj(cpp, "__LDBL_HAS_DENORM__", "1");
    zero_cc_cpp_define_obj(cpp, "__LDBL_HAS_INFINITY__", "1");
    zero_cc_cpp_define_obj(cpp, "__LDBL_HAS_QUIET_NAN__", "1");
    zero_cc_cpp_define_obj(cpp, "__LDBL_MAX_EXP__", "1024");
    zero_cc_cpp_define_obj(cpp, "__LDBL_MIN_EXP__", "(-1021)");
    zero_cc_cpp_define_obj(cpp, "__LDBL_MAX_10_EXP__", "308");
    zero_cc_cpp_define_obj(cpp, "__LDBL_MIN_10_EXP__", "(-307)");
    zero_cc_cpp_define_obj(cpp, "__LDBL_MAX__", "1.7976931348623157081452742373170435679807e+308L");
    zero_cc_cpp_define_obj(cpp, "__LDBL_MIN__", "2.22507385850720138309e-308L");
    zero_cc_cpp_define_obj(cpp, "__LDBL_DENORM_MIN__", "4.94065645841246544177e-324L");
    zero_cc_cpp_define_obj(cpp, "__DECIMAL_DIG__", "17");
    zero_cc_cpp_define_obj(cpp, "FLT_RADIX", "2");
    zero_cc_cpp_define_obj(cpp, "FLT_MANT_DIG", "24");
    zero_cc_cpp_define_obj(cpp, "FLT_DIG", "6");
    zero_cc_cpp_define_obj(cpp, "FLT_EPSILON", "1.19209289550781250000e-7F");
    zero_cc_cpp_define_obj(cpp, "FLT_MAX_EXP", "128");
    zero_cc_cpp_define_obj(cpp, "FLT_MIN_EXP", "(-125)");
    zero_cc_cpp_define_obj(cpp, "FLT_MAX_10_EXP", "38");
    zero_cc_cpp_define_obj(cpp, "FLT_MIN_10_EXP", "(-37)");
    zero_cc_cpp_define_obj(cpp, "FLT_MAX", "3.40282346638528859812e+38F");
    zero_cc_cpp_define_obj(cpp, "FLT_MIN", "1.17549435082228750797e-38F");
    zero_cc_cpp_define_obj(cpp, "DBL_RADIX", "2");
    zero_cc_cpp_define_obj(cpp, "DBL_MANT_DIG", "53");
    zero_cc_cpp_define_obj(cpp, "DBL_DIG", "15");
    zero_cc_cpp_define_obj(cpp, "DBL_EPSILON", "2.22044604925031308085e-16");
    zero_cc_cpp_define_obj(cpp, "DBL_MAX_EXP", "1024");
    zero_cc_cpp_define_obj(cpp, "DBL_MIN_EXP", "(-1021)");
    zero_cc_cpp_define_obj(cpp, "DBL_MAX_10_EXP", "308");
    zero_cc_cpp_define_obj(cpp, "DBL_MIN_10_EXP", "(-307)");
    zero_cc_cpp_define_obj(cpp, "DBL_MAX", "1.7976931348623157081452742373170435679807e+308");
    zero_cc_cpp_define_obj(cpp, "DBL_MIN", "2.22507385850720138309e-308");
    zero_cc_cpp_define_obj(cpp, "LDBL_EPSILON", "2.22044604925031308085e-16L");
    zero_cc_cpp_define_obj(cpp, "LDBL_MAX_EXP", "1024");
    zero_cc_cpp_define_obj(cpp, "LDBL_MIN_EXP", "(-1021)");
    zero_cc_cpp_define_obj(cpp, "LDBL_MAX_10_EXP", "308");
    zero_cc_cpp_define_obj(cpp, "LDBL_MIN_10_EXP", "(-307)");
    zero_cc_cpp_define_obj(cpp, "LDBL_MAX", "1.7976931348623157081452742373170435679807e+308L");
    zero_cc_cpp_define_obj(cpp, "LDBL_MIN", "2.22507385850720138309e-308L");
    zero_cc_cpp_define_obj(cpp, "LDBL_MANT_DIG", "53");
    zero_cc_cpp_define_obj(cpp, "LDBL_DIG", "15");
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

void zero_cc_cpp_undef(ZeroCCPreprocessor *cpp, const char *name) {
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

void zero_cc_cpp_restore_macro(ZeroCCPreprocessor *cpp, const ZeroCCMacro *src) {
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

void zero_cc_cpp_push_macro(ZeroCCPreprocessor *cpp, const char *name) {
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

void zero_cc_cpp_pop_macro(ZeroCCPreprocessor *cpp, const char *name) {
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

/* C11 6.10.3.1p2: each argument, *except for arguments used with the # or ##
 * preprocessing operators in the replacement list*, is fully macro-expanded
 * before substitution. An argument whose parameter is used with # or ## must
 * be substituted RAW - the classic double-indirection idiom (STRING(X) ->
 * "X", TOSTRING(X) -> "42") and every __ASMNAME-style symbol-name macro
 * depend on it. Pre-expanding such arguments (the pre-Sprint-176 behavior)
 * made STRING(X) yield "42" instead of "X".
 *
 * Scans the function-like macro's replacement text and marks, per parameter
 * index, whether that parameter occurs adjacent to a # or ## operator:
 *   #param  (stringification), param ## , ## param (token pasting), with
 *   whitespace allowed around ## and between # and the identifier. String
 * and char literals in the body are skipped (a # or ## inside them is not an
 *   operator). __VA_ARGS__ maps to the variadic slot (param_count - 1). */
static void cpp_scan_hash_params(const ZeroCCMacro *m, bool *used, int max_param) {
    for (int k = 0; k < max_param && k < 32; k++) used[k] = false;
    if (!m || !m->replacement || m->param_count < 0 || max_param <= 0) return;

    const char *b = m->replacement;
    char last_id[128];
    size_t last_id_len = 0;
    while (*b) {
        if (*b == '"' || *b == '\'') {
            char q = *b++;
            last_id_len = 0;
            while (*b) {
                if (*b == '\\' && b[1]) { b += 2; continue; }
                if (*b == q) { b++; break; }
                b++;
            }
            continue;
        }
        if (*b == '#' && b[1] == '#') {
            /* Parameter immediately BEFORE the ## (whitespace between the
             * identifier and ## does not separate the tokens). */
            if (last_id_len > 0) {
                for (int k = 0; k < m->param_count && k < max_param; k++) {
                    if (strlen(m->params[k]) == last_id_len &&
                        strncmp(m->params[k], last_id, last_id_len) == 0)
                        used[k] = true;
                }
                if (m->is_variadic && m->param_count > 0 &&
                    m->param_count - 1 < max_param &&
                    last_id_len == 11 && strncmp(last_id, "__VA_ARGS__", 11) == 0)
                    used[m->param_count - 1] = true;
            }
            b += 2;
            last_id_len = 0;
            while (*b == ' ' || *b == '\t') b++;
            /* Parameter immediately AFTER the ## */
            if (isalpha((unsigned char)*b) || *b == '_') {
                char id[128];
                size_t l = 0;
                while ((*b && (isalnum((unsigned char)*b) || *b == '_')) && l < 127) id[l++] = *b++;
                id[l] = '\0';
                for (int k = 0; k < m->param_count && k < max_param; k++)
                    if (strcmp(m->params[k], id) == 0) used[k] = true;
                if (m->is_variadic && m->param_count > 0 &&
                    m->param_count - 1 < max_param && strcmp(id, "__VA_ARGS__") == 0)
                    used[m->param_count - 1] = true;
            }
            continue;
        }
        if (*b == '#') {
            /* #param stringification (a single # is not an operator here,
             * it is only significant when it directly precedes a parameter) */
            b++;
            last_id_len = 0;
            while (*b == ' ' || *b == '\t') b++;
            if (isalpha((unsigned char)*b) || *b == '_') {
                char id[128];
                size_t l = 0;
                while ((*b && (isalnum((unsigned char)*b) || *b == '_')) && l < 127) id[l++] = *b++;
                id[l] = '\0';
                for (int k = 0; k < m->param_count && k < max_param; k++)
                    if (strcmp(m->params[k], id) == 0) used[k] = true;
                if (m->is_variadic && m->param_count > 0 &&
                    m->param_count - 1 < max_param && strcmp(id, "__VA_ARGS__") == 0)
                    used[m->param_count - 1] = true;
            }
            continue;
        }
        if (isalpha((unsigned char)*b) || *b == '_') {
            size_t l = 0;
            while ((*b && (isalnum((unsigned char)*b) || *b == '_')) && l < 127) last_id[l++] = *b++;
            last_id[l] = '\0';
            last_id_len = l;
            continue;
        }
        if (isspace((unsigned char)*b)) { b++; continue; }  /* whitespace keeps last_id */
        last_id_len = 0;
        b++;
    }
}

/* A char that can JOIN with an adjacent char into one token: identifier
 * chars, pp-number chars (digit . e E p P x X suffixes), plus '/' to
 * block accidental // and /* comment starts. Used to keep substitution
 * boundaries from fusing two body tokens into one (see below). */
static bool cpp_fusion_char(char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
           (ch >= '0' && ch <= '9') || ch == '_' || ch == '.' || ch == '/';
}

void zero_cc_cpp_process_text(ZeroCCPreprocessor *cpp, const char *src, ZeroCCBuffer *out) {
    if (!src) return;
    const char *p = src;
    bool at_line_start = true;
    /* line_num is NOT reset here - the caller owns it.  Top-level file entry
     * sets it to 1; #include pushes the outer count and resets to 1 for the
     * included file (C99 6.10.8.1 per-file numbering); nested string
     * processing (macro bodies, #if/attribute expressions) saves and restores
     * it so those strings' newlines never leak into the source line count and
     * a __LINE__ in a macro body expands to the line where the macro is used. */

    while (*p) {
        /* Comments - process first */
        if (*p == '/' && p[1] == '/') {
            p += 2;
            for (;;) {
                while (*p && *p != '\n') p++;
                if (*p != '\n') break;
                /* Phase-2 splice: a backslash before the newline joins the
                 * next line into THIS comment (it is deleted before comment
                 * recognition) - do not terminate or emit a logical line,
                 * but the physical line number still advances. */
                if (p[-1] == '\\') { p++; cpp->line_num++; continue; }
                zero_cc_buf_putc(out, *p++); at_line_start = true; cpp->line_num++;
                break;
            }
            continue;
        }
        if (*p == '/' && p[1] == '*') {
            p += 2;
            while (*p && !(*p == '*' && p[1] == '/')) {
                if (*p == '\n') {
                    /* A spliced newline (backslash before it) is deleted in
                     * phase 1 and not re-emitted, but it still advances the
                     * physical line number. */
                    if (p[-1] != '\\') zero_cc_buf_putc(out, '\n');
                    at_line_start = true; cpp->line_num++;
                }
                p++;
            }
            if (*p) p += 2;
            continue;
        }

        if (at_line_start && cpp->arg_expand_depth == 0) {
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
                    bool val = active && zero_cc_cpp_is_macro_defined(cpp, macro_name);
                    cpp->cond_true[cpp->cond_depth] = val;
                    cpp->cond_seen_true[cpp->cond_depth] = val;
                } else if (strcmp(directive, "ifndef") == 0) {
                    char macro_name[128]; size_t ml = 0;
                    while (*p && (isalnum((unsigned char)*p) || *p == '_') && ml < 127) macro_name[ml++] = *p++;
                    macro_name[ml] = '\0';
                    cpp->cond_depth++;
                    bool val = active && !zero_cc_cpp_is_macro_defined(cpp, macro_name);
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
                } else if (strcmp(directive, "elifdef") == 0) {
                    char macro_name[128]; size_t ml = 0;
                    while (*p && (isalnum((unsigned char)*p) || *p == '_') && ml < 127) macro_name[ml++] = *p++;
                    macro_name[ml] = '\0';
                    if (cpp->cond_depth > 0) {
                        bool prev_depth_active = (cpp->cond_depth == 1 || cpp->cond_true[cpp->cond_depth - 1]);
                        if (!cpp->cond_seen_true[cpp->cond_depth] && prev_depth_active && zero_cc_cpp_is_macro_defined(cpp, macro_name)) {
                            cpp->cond_true[cpp->cond_depth] = true;
                            cpp->cond_seen_true[cpp->cond_depth] = true;
                        } else {
                            cpp->cond_true[cpp->cond_depth] = false;
                        }
                    }
                } else if (strcmp(directive, "elifndef") == 0) {
                    char macro_name[128]; size_t ml = 0;
                    while (*p && (isalnum((unsigned char)*p) || *p == '_') && ml < 127) macro_name[ml++] = *p++;
                    macro_name[ml] = '\0';
                    if (cpp->cond_depth > 0) {
                        bool prev_depth_active = (cpp->cond_depth == 1 || cpp->cond_true[cpp->cond_depth - 1]);
                        if (!cpp->cond_seen_true[cpp->cond_depth] && prev_depth_active && !zero_cc_cpp_is_macro_defined(cpp, macro_name)) {
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
                            /* Phase-2 splice (C99 5.1.1.2): a backslash+
                             * newline inside the parameter list is deleted
                             * before the list is parsed (`#define CHAIN(a, \\
 b)`
                             * is the two-parameter macro CHAIN(a, b)). The
                             * old scan stalled on the backslash (infinite
                             * loop). A backslash not followed by a newline
                             * is a constraint violation - skip it so the
                             * scan cannot stall either way. */
                            if (*p == '\\') {
                                if (p[1] == '\n') { p += 2; cpp->line_num++; }
                                else if (p[1] == '\r' && p[2] == '\n') { p += 3; cpp->line_num++; }
                                else p++;
                            }
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
                            } else if (pl == 0) {
                                /* Defensive: a non-parameter token in the
                                 * list (constraint violation) - consume it
                                 * so the scan always makes progress. */
                                if (*p) p++;
                            }
                        }
                        if (*p == ')') p++;
                        while (*p && isspace((unsigned char)*p) && *p != '\n') p++;
                        /* Build the body (phase-2 comment + continuation
                         * splicing - see the helper). */
                        char *body = zero_cc_cpp_capture_define_value(cpp, &p);
                        if (!body) {  /* OOM */
                            body = (char *)malloc(1);
                            if (!body) { fprintf(stderr, "Out of memory\n"); exit(1); }
                            body[0] = '\0';
                        }

                        if (cpp->macro_count < ZERO_CC_MAX_MACROS) {
                            if (cpp->macro_count >= cpp->macro_capacity) {
                                cpp->macro_capacity *= 2;
                                cpp->macros = (ZeroCCMacro *)realloc(cpp->macros, cpp->macro_capacity * sizeof(ZeroCCMacro));
                                cpp->hash_next = (int32_t *)realloc(cpp->hash_next, cpp->macro_capacity * sizeof(int32_t));
                            }
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
                        /* Object-macro value (phase-2 comment + continuation
                         * splicing - see the helper). */
                        char *val = zero_cc_cpp_capture_define_value(cpp, &p);
                        if (!val) {  /* OOM */
                            val = (char *)malloc(1);
                            if (!val) { fprintf(stderr, "Out of memory\n"); exit(1); }
                            val[0] = '\0';
                        }
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
                        size_t saved_line = cpp->line_num;
                        cpp->filename = resolved_path[0] ? resolved_path : NULL;
                        cpp->line_num = 1; /* included file starts at line 1 */
                        zero_cc_cpp_process_text(cpp, inc_content, out);
                        cpp->line_num = saved_line;
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
                    /* gcc -E keeps the pragma line in the output (side
                     * effects still run: message note, macro stack, diag
                     * state). Compile mode consumes handled pragmas and
                     * forwards the rest (pack, unknown) to the lexer, which
                     * records pack events and drops the remainder. */
                    {
                        const char *eol = p;
                        while (*eol && *eol != '\n') eol++;
                        bool handled = zero_cc_cpp_handle_pragma(cpp, p, (size_t)(eol - p));
                        if (handled && !cpp->preproc_only) {
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
                    const char *dp = p;
                    char *expanded_line = NULL;
                    if (*dp != '"' && *dp != '<') {
                        const char *line_end = dp;
                        while (*line_end && *line_end != '\n') {
                            if (*line_end == '\\' && line_end[1] == '\n') line_end += 2;
                            else line_end++;
                        }
                        size_t raw_len = (size_t)(line_end - dp);
                        char *raw = (char *)malloc(raw_len + 1);
                        if (raw) {
                            memcpy(raw, dp, raw_len);
                            raw[raw_len] = '\0';
                            ZeroCCBuffer eb;
                            zero_cc_buf_init(&eb, raw_len * 2 + 32);
                            size_t sl = cpp->line_num;
                            int sd = cpp->cond_depth;
                            cpp->cond_depth = 0;
                            zero_cc_cpp_process_text(cpp, raw, &eb);
                            cpp->cond_depth = sd;
                            cpp->line_num = sl;
                            zero_cc_buf_putc(&eb, '\0');
                            free(raw);
                            expanded_line = eb.data;
                            dp = expanded_line;
                            while (*dp && isspace((unsigned char)*dp)) dp++;
                        }
                    }
                    bool is_sys = (*dp == '<');
                    char delim_end = is_sys ? '>' : '"';
                    if (*dp == '"' || *dp == '<') dp++;
                    char asset_path[256]; size_t apl = 0;
                    while (*dp && *dp != delim_end && *dp != '\n' && apl < 255) asset_path[apl++] = *dp++;
                    asset_path[apl] = '\0';
                    if (*dp == delim_end) dp++;

                    int64_t limit_val = -1;
                    char *prefix_tokens = NULL;
                    char *suffix_tokens = NULL;
                    char *if_empty_tokens = NULL;

                    while (*dp && *dp != '\n') {
                        while (*dp && isspace((unsigned char)*dp) && *dp != '\n') dp++;
                        if (*dp == '\n' || *dp == '\0') break;
                        if (*dp == '\\' && dp[1] == '\n') { dp += 2; continue; }
                        char pname[64]; size_t pnl = 0;
                        while (*dp && (isalnum((unsigned char)*dp) || *dp == '_' || *dp == ':') && pnl + 1 < sizeof(pname)) {
                            pname[pnl++] = *dp++;
                        }
                        pname[pnl] = '\0';
                        while (*dp && isspace((unsigned char)*dp) && *dp != '\n') dp++;
                        if (*dp == '(') {
                            char *inner = zero_cc_cpp_capture_paren_tokens(&dp);
                            if (strcmp(pname, "limit") == 0) {
                                if (inner) {
                                    int64_t lim = zero_cc_cpp_eval_expr(cpp, inner);
                                    limit_val = (lim >= 0) ? lim : 0;
                                }
                            } else if (strcmp(pname, "prefix") == 0) {
                                if (prefix_tokens) free(prefix_tokens);
                                prefix_tokens = inner;
                                inner = NULL;
                            } else if (strcmp(pname, "suffix") == 0) {
                                if (suffix_tokens) free(suffix_tokens);
                                suffix_tokens = inner;
                                inner = NULL;
                            } else if (strcmp(pname, "if_empty") == 0) {
                                if (if_empty_tokens) free(if_empty_tokens);
                                if_empty_tokens = inner;
                                inner = NULL;
                            }
                            if (inner) free(inner);
                        } else {
                            if (*dp && *dp != '\n') dp++;
                        }
                    }

                    size_t rsz = 0;
                    char resolved_file[PATH_MAX];
                    unsigned char *rdata = zero_cc_cpp_read_binary_resource(cpp, asset_path, is_sys, &rsz, resolved_file, sizeof(resolved_file));
                    if (!rdata) {
                        if (cpp->include_error[0] == '\0') {
                            snprintf(cpp->include_error, sizeof(cpp->include_error),
                                     "%s:%zu: fatal error: %s: No such file or directory",
                                     cpp->filename ? cpp->filename : "<stdin>",
                                     cpp->line_num, asset_path);
                        }
                    } else {
                        if (resolved_file[0]) {
                            zero_cc_cpp_add_dependency(cpp, resolved_file);
                        }
                        size_t emit_len = rsz;
                        if (limit_val >= 0 && (size_t)limit_val < emit_len) {
                            emit_len = (size_t)limit_val;
                        }
                        if (emit_len == 0) {
                            if (if_empty_tokens) {
                                zero_cc_buf_puts(out, if_empty_tokens);
                            }
                        } else {
                            if (prefix_tokens) {
                                zero_cc_buf_puts(out, prefix_tokens);
                            }
                            for (size_t i = 0; i < emit_len; i++) {
                                char buf[16];
                                snprintf(buf, sizeof(buf), "%u%s", rdata[i], (i + 1 < emit_len) ? ", " : "");
                                zero_cc_buf_puts(out, buf);
                            }
                            if (suffix_tokens) {
                                zero_cc_buf_puts(out, suffix_tokens);
                            }
                        }
                        free(rdata);
                    }
                    if (prefix_tokens) free(prefix_tokens);
                    if (suffix_tokens) free(suffix_tokens);
                    if (if_empty_tokens) free(if_empty_tokens);
                    if (expanded_line) free(expanded_line);
                } else if (active && strcmp(directive, "warning") == 0) {
                    /* C23 6.9.1 #warning (GCC extension pre-C23): emit a
                     * warning whose message is the directive's own text
                     * (gcc: `warning: #warning "my text" [-Wcpp]`; zerocc
                     * omits the [-W] tag per its diagnostic style).  The
                     * argument is raw (quotes preserved); empty is legal.
                     * `#pragma GCC diagnostic ignored "-Wcpp"` suppresses
                     * it; `... error "-Werror"` escalates it to a hard
                     * error (recorded via include_error - the
                     * preprocessor's only failure channel, consumed by the
                     * driver as a preprocess failure). */
                    bool w_disabled = false;
                    for (int i = 0; i < cpp->diagnostic_state.disabled_warning_count; i++) {
                        if (cpp->diagnostic_state.disabled_warnings[i] &&
                            strcmp(cpp->diagnostic_state.disabled_warnings[i], "cpp") == 0) {
                            w_disabled = true;
                            break;
                        }
                    }
                    const char *arg_start = p;
                    const int warn_line = (int)cpp->line_num; /* the directive's own line (a spliced argument advances line_num) */
                    while (*p && *p != '\n') {
                        if (*p == '\\' && p[1] == '\n') { p += 2; cpp->line_num++; continue; }
                        p++;
                    }
                    if (!w_disabled) {
                        char wmsg[256]; size_t wl = 0;
                        for (const char *t = arg_start; t < p && wl < sizeof(wmsg) - 1; ) {
                            if (*t == '\\' && t[1] == '\n') { t += 2; continue; } /* spliced */
                            wmsg[wl++] = *t++;
                        }
                        while (wl > 0 && isspace((unsigned char)wmsg[wl - 1])) wl--;
                        wmsg[wl] = '\0';
                        bool as_error = cpp->diagnostic_state.warning_as_error;
                        if (as_error) {
                            /* Hard error: record via include_error (the
                             * preprocessor's only failure channel) so the
                             * driver prints it once and aborts - the same
                             * single-print path as a missing #include. */
                            if (cpp->include_error[0] == '\0') {
                                snprintf(cpp->include_error, sizeof(cpp->include_error),
                                         "%s:%d: error: #warning %s",
                                         cpp->filename ? cpp->filename : "<stdin>",
                                         warn_line, wmsg);
                            }
                        } else {
                            fprintf(stderr, "%s:%d: warning: #warning %s\n",
                                    cpp->filename ? cpp->filename : "<stdin>", warn_line,
                                    wmsg);
                        }
                    }
                } else if (active && strcmp(directive, "error") == 0) {
                    /* C99 6.10.1 #error: a diagnostic is REQUIRED and the
                     * translation must be abandoned.  gcc:
                     * `error: <arg-text>` + fatal.  The message is the
                     * directive's raw argument (the `#` and `error` token
                     * are not part of it); the build fails via
                     * include_error (the preprocessor's only failure
                     * channel - single print, like a missing #include). */
                    const char *arg_start = p;
                    const int err_line = (int)cpp->line_num;
                    while (*p && *p != '\n') {
                        if (*p == '\\' && p[1] == '\n') { p += 2; cpp->line_num++; continue; }
                        p++;
                    }
                    char emsg[256]; size_t el = 0;
                    for (const char *t = arg_start; t < p && el < sizeof(emsg) - 1; ) {
                        if (*t == '\\' && t[1] == '\n') { t += 2; continue; } /* spliced */
                        emsg[el++] = *t++;
                    }
                    while (el > 0 && isspace((unsigned char)emsg[el - 1])) el--;
                    emsg[el] = '\0';
                    if (cpp->include_error[0] == '\0') {
                        snprintf(cpp->include_error, sizeof(cpp->include_error),
                                 "%s:%d: error: #error %s",
                                 cpp->filename ? cpp->filename : "<stdin>",
                                 err_line, emsg);
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
            if (*p == '\n') {
                if (p > src && p[-1] == '\\') { p++; cpp->line_num++; } /* spliced: physical line advances */
                else { p++; at_line_start = true; cpp->line_num++; }
            }
            continue;
        }

        if (*p == '\n') { zero_cc_buf_putc(out, *p++); at_line_start = true; cpp->line_num++; continue; }
        at_line_start = false;

        /* String literals */
        if (*p == '"' || *p == '\'') {
            char quote = *p;
            zero_cc_buf_putc(out, *p++);
            while (*p && *p != quote) {
                /* Phase-2 splice: backslash+newline is DELETED (the string
                 * continues on the next physical line with no extra chars);
                 * the physical line number still advances. Other escapes
                 * copy verbatim. */
                if (*p == '\\' && p[1] == '\n') { p += 2; cpp->line_num++; continue; }
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
            const char *ident_at = p - il; /* source position of the ident */

            /* C99 6.4.2.1: an alphanumeric run immediately preceded by a
             * digit or underscore is the suffix of a PP-number, not an
             * identifier (the `F` in `0.0F`, the hex digit in `0x1F`, the
             * `u` in `1u`).  A PP-number is one preprocessing token, so
             * macro expansion must not apply to its letters - `#define F
             * 140` must not rewrite `0.0F` into `0.0140` (which would
             * silently re-scale every float literal, c-torture
             * execute/cmpsf-1: `#define F 140` + `float args[] = {0.0F}`). */
            if (ident_at > src &&
                (isdigit((unsigned char)ident_at[-1]) || ident_at[-1] == '_')) {
                zero_cc_buf_puts(out, ident);
                continue;
            }

            /* Wide/UTF string literal prefixes: `L'X'`, `L"s"`, `u"s"`,
             * `U"s"`, `u8"s"` are single lexical units - the prefix is NOT
             * an identifier, so a `#define L 264` must not rewrite `L'X'`
             * into `264'X'` (c-torture 970214-1). Emit verbatim; the quote
             * handling below will copy the literal itself. */
            if ((*p == '"' || *p == '\'' ) &&
                ((il == 1 && ident[0] == 'L') ||
                 (il == 1 && ident[0] == 'u') ||
                 (il == 1 && ident[0] == 'U') ||
                 (il == 2 && ident[0] == 'u' && ident[1] == '8'))) {
                zero_cc_buf_put_bytes(out, ident, il);
                continue;
            }

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
                if (cpp->expand_depth > 32) {
                    /* Depth guard: cap the expansion nesting and leave the
                     * macro name as a plain identifier. Mutual cycles are
                     * normally cut by the blue-paint stack (the inner
                     * occurrence finds the outer macro still blue); this
                     * bounds pathologically deep but non-cyclic nesting. */
                    zero_cc_buf_puts(out, ident);
                    continue;
                }
                if (m->param_count < 0) {
                    if (cpp->expanding_top < 39) cpp->expanding_stack[++cpp->expanding_top] = (int)(m - cpp->macros);
                    cpp->expand_depth++;
                    {
                        size_t obj_saved_line = cpp->line_num;
                        zero_cc_cpp_process_text(cpp, m->replacement, out);
                        cpp->line_num = obj_saved_line;
                    }
                    cpp->expand_depth--;
                    if (cpp->expanding_top >= 0) cpp->expanding_top--;
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
                        const char *arglist_start = p;
                        char *args[64];
                        int arg_count = 0;
                        /* C11 6.10.3.1p2: which arguments must stay RAW
                         * (their parameter is used with # or ## in the
                         * body) instead of being pre-expanded. */
                        bool hash_used[32];
                        cpp_scan_hash_params(m, hash_used, 32);
                        while (*p) {
                            while (*p && isspace((unsigned char)*p)) p++;
                            /* Sprint 72: a `)` reached after skipping whitespace
                             * (e.g. `F(0, )` with a space before the close) ends the
                             * argument list - do not push a phantom empty argument.
                             * The old condition checked `*p != ')'` *before* the
                             * whitespace skip, so `F(0, )` re-entered and added a
                             * third empty arg, breaking token-paste of the macro. */
                            if (!*p || *p == ')') break;
                            const char *arg_s = p;
                            int paren_nest = 0;
                            bool in_str = false, in_chr = false;
                            /* A comma INSIDE a string/char literal is not an
                             * argument separator (c-torture-style F("a, b");
                             * sqlite3.c's OpHelp("...NULL, goto P2")). The old
                             * scan counted only parens, so the comma ended the
                             * argument early, the arg count mismatched the
                             * parameter count, and the invocation was left
                             * unexpanded. The separator check must run BEFORE
                             * the paren count is updated: the closing `)` of
                             * the call drives a top-level nest from 0 to -1. */
                            while (*p) {
                                char ch = *p;
                                if (in_str) {
                                    if (ch == '\\' && p[1]) { p += 2; continue; }
                                    if (ch == '"') in_str = false;
                                    p++;
                                    continue;
                                }
                                if (in_chr) {
                                    if (ch == '\\' && p[1]) { p += 2; continue; }
                                    if (ch == '\'') in_chr = false;
                                    p++;
                                    continue;
                                }
                                if (ch == '"') { in_str = true; p++; continue; }
                                if (ch == '\'') { in_chr = true; p++; continue; }
                                if (paren_nest == 0 && (ch == ',' || ch == ')')) break;
                                if (ch == '(') paren_nest++;
                                else if (ch == ')') paren_nest--;
                                p++;
                            }
                            size_t arg_l = (size_t)(p - arg_s);
                            while (arg_l > 0 && isspace((unsigned char)arg_s[arg_l - 1])) arg_l--;
                            char *arg_str = (char *)malloc(arg_l + 1);
                        if (arg_str) {
                            memcpy(arg_str, arg_s, arg_l);
                            arg_str[arg_l] = '\0';
                            if (arg_count < 64) {
                                /* C99 6.10.3.4p3: macro invocations within
                                 * an argument are completely expanded before
                                 * the argument is substituted (c-torture
                                 * compile/20050423-1: TEN(TEN(a += b; ...))).
                                 * C11 6.10.3.1p2: EXCEPT when the argument's
                                 * parameter is used with # or ## in the body -
                                 * that argument must be substituted raw
                                 * (STRING(X) -> "X", not "42"; the body walk
                                 * stringifies/pastes the raw text at the #/##
                                 * site, and any normal-position occurrence is
                                 * still expanded by the rescan below).
                                 * The outer macro stays blue (expanding_macro
                                 * index unchanged) so self-reference cannot
                                 * loop; the depth guard bounds mutual cycles. */
                                int hash_pi = arg_count;
                                if (m->is_variadic && m->param_count > 0 &&
                                    hash_pi >= m->param_count - 1)
                                    hash_pi = m->param_count - 1;
                                if (arg_l > 0 && hash_pi < 32 && !hash_used[hash_pi]) {
                                    ZeroCCBuffer arg_src, arg_out;
                                    zero_cc_buf_init(&arg_src, arg_l + 2);
                                    zero_cc_buf_init(&arg_out, arg_l + 16);
                                    zero_cc_buf_puts(&arg_src, arg_str);
                                    zero_cc_buf_putc(&arg_src, 0);
                                    cpp->expand_depth++;
                                    cpp->arg_expand_depth++;
                                    {
                                        size_t arg_saved_line = cpp->line_num;
                                        zero_cc_cpp_process_text(cpp, arg_src.data, &arg_out);
                                        cpp->line_num = arg_saved_line;
                                    }
                                    cpp->arg_expand_depth--;
                                    cpp->expand_depth--;
                                    zero_cc_buf_putc(&arg_out, 0);
                                    free(arg_str);
                                    arg_str = (char *)malloc(arg_out.length + 1);
                                    if (arg_str) {
                                        memcpy(arg_str, arg_out.data, arg_out.length + 1);
                                    } else {
                                        arg_str = (char *)malloc(1);
                                        if (arg_str) arg_str[0] = 0;
                                    }
                                    zero_cc_buf_free(&arg_src);
                                    zero_cc_buf_free(&arg_out);
                                }
                                args[arg_count++] = arg_str;
                            } else {
                                free(arg_str);
                            }
                        }
                        if (*p == ',') {
                            p++;
                            /* Trailing empty argument: `F(0,)` has TWO
                             * arguments (second empty) - C99 6.10.3.2. */
                            const char *q = p;
                            while (*q && isspace((unsigned char)*q)) q++;
                            if (*q == ')' && arg_count < 64) {
                                char *e = (char *)malloc(1);
                                if (e) {
                                    e[0] = 0;
                                    args[arg_count++] = e;
                                }
                            }
                        }
                    }
                    if (*p == ')') p++;

                    /* C99 6.10.8.1: the argument list is consumed here, not
                     * by the main loop, so its physical newlines (multi-line
                     * calls like TEST_ASSERT(F(\n ...)) must advance the line
                     * count now; each argument's own expansion keeps them
                     * from leaking elsewhere via its save/restore. */
                    for (const char *nl = arglist_start; nl < p; nl++)
                        if (*nl == '\n') cpp->line_num++;
                    if (m->is_variadic && m->param_count > 0) {
                        if (arg_count >= m->param_count) {
                            size_t total_va_len = 0;
                            for (int k = m->param_count - 1; k < arg_count; k++) {
                                total_va_len += strlen(args[k]) + 4;
                            }
                            char *va_str = (char *)malloc(total_va_len + 1);
                            if (va_str) {
                                size_t off = 0;
                                for (int k = m->param_count - 1; k < arg_count; k++) {
                                    if (k > m->param_count - 1) {
                                        va_str[off++] = ',';
                                        va_str[off++] = ' ';
                                    }
                                    size_t al = strlen(args[k]);
                                    memcpy(va_str + off, args[k], al);
                                    off += al;
                                    free(args[k]);
                                }
                                va_str[off] = '\0';
                                args[m->param_count - 1] = va_str;
                                arg_count = m->param_count;
                            }
                        } else if (arg_count == m->param_count - 1 && arg_count < 32) {
                            args[m->param_count - 1] = strdup("");
                            arg_count = m->param_count;
                        }
                    }

                    /* C99 6.10.3p2: a function-like macro with too MANY
                     * arguments is NOT an invocation - the name
                     * is not replaced.  Without this, `k(1,2)` for
                     * `#define k(a) #a` silently expands on the first arg
                     * (c-torture compile/921227-1: gcc leaves the text
                     * as-is so the outer stringification yields
                     * "k(1,2)").
                     *
                     * Too FEW: GCC/cpp treats the missing trailing
                     * arguments as empty (strictly UB; the de facto
                     * behavior is what real code relies on): `#define
                     * S(x) #x` + `S()` -> `""`.  c-torture
                     * execute/builtins {strstr,memops}-asm build their
                     * `__asm` symbol names via ASMNAME2(prefix, cname) =
                     * STRING(prefix) cname with prefix =
                     * __USER_LABEL_PREFIX__ (empty macro) - the argument
                     * pre-expansion above empties it, the substitution
                     * yields `STRING ()`, and the rescan must stringify
                     * the empty argument to `""` (or the symbol becomes
                     * "__USER_LABEL_PREFIX__my_X"). */
                    if (!m->is_variadic && arg_count > m->param_count) {
                        for (int k = 0; k < arg_count; k++) free(args[k]);
                        for (const char *q = ident_at; q < p; q++) zero_cc_buf_putc(out, *q);
                        continue;
                    }
                    if (!m->is_variadic && arg_count < m->param_count) {
                        if (m->param_count > 64) {
                            for (int k = 0; k < arg_count; k++) free(args[k]);
                            for (const char *q = ident_at; q < p; q++) zero_cc_buf_putc(out, *q);
                            continue;
                        }
                        for (int k = arg_count; k < m->param_count; k++) {
                            char *e = (char *)malloc(1);
                            if (e) { e[0] = 0; args[k] = e; }
                        }
                        arg_count = m->param_count;
                    }

                    /* Token pasting buffer */
                    char paste_buf[512];
                    size_t paste_len = 0;
                    bool pending_paste = false;
                    bool left_comma = false;
                    paste_buf[0] = '\0';

                    /* GNU vector attribute: `__attribute__((__vector_size__(N)))`
                     * would normally be erased wholesale (the predefined macro
                     * expands to empty). Keep the width visible to the parser by
                     * emitting a `__zc_vector_size__(N)` marker - an identifier
                     * followed by a parenthesized constant expression - that the
                     * declaration paths recognize and turn into a vector type
                     * (see zero_cc_parse_vector_attr). Other attributes keep the
                     * erase behavior. */
                    bool vec_marker_done = false;
                    if (arg_count > 0 &&
                        (strcmp(m->name, "__attribute__") == 0 || strcmp(m->name, "__attribute") == 0)) {
                        for (int k = 0; k < arg_count && !vec_marker_done; k++) {
                            const char *a = args[k];
                            size_t al = strlen(a);
                            while (al > 0 && (a[al - 1] == ' ' || a[al - 1] == '\t')) al--;
                            const char *it = a;
                            size_t il = al;
                            /* Peel one level of redundant surrounding parens */
                            if (il >= 2 && it[0] == '(' && it[il - 1] == ')') {
                                it++;
                                il -= 2;
                                while (il > 0 && (it[il - 1] == ' ' || it[il - 1] == '\t')) il--;
                                while (il > 0 && (it[0] == ' ' || it[0] == '\t')) { it++; il--; }
                            }
                            if (il < 2) continue;
                            /* Scan the top level of the attribute list for a
                             * vector_size(N) / __vector_size__(N) element
                             * (GCC: `__attribute__((__vector_size__(16),
                             * __may_alias__))` - the vector entry may be one
                             * of several attributes). */
                            const char *ex = NULL;
                            size_t el = 0;
                            int ex_marker = 0; /* 1 = vector_size, 2 = aligned, 3 = alias, 4 = mode, 5 = scalar_storage_order, 7 = weak */
                            int depth = 0;
                            for (const char *q = it; q < it + il; q++) {
                                if (*q == '(') {
                                    if (depth == 0) {
                                        /* name immediately before this '('? */
                                        size_t pre = (size_t)(q - it);
                                        size_t end = pre;
                                        while (end > 0 && (it[end - 1] == ' ' || it[end - 1] == '\t')) end--;
                                        size_t start = end;
                                        while (start > 0 && (isalnum((unsigned char)it[start - 1]) || it[start - 1] == '_')) start--;
                                        size_t namelen = end - start;
                                        const char *nam = it + start;
                                        bool is_vs = (namelen == 11 && strncmp(nam, "vector_size", 11) == 0) ||
                                                     (namelen == 15 && strncmp(nam, "__vector_size__", 15) == 0);
                                        bool is_al = (namelen == 7 && strncmp(nam, "aligned", 7) == 0) ||
                                                     (namelen == 11 && strncmp(nam, "__aligned__", 11) == 0);
                                        bool is_as = (namelen == 5 && strncmp(nam, "alias", 5) == 0) ||
                                                     (namelen == 9 && strncmp(nam, "__alias__", 9) == 0);
                                        bool is_md = (namelen == 4 && strncmp(nam, "mode", 4) == 0) ||
                                                     (namelen == 8 && strncmp(nam, "__mode__", 8) == 0);
                                        bool is_sso = (namelen == 20 && strncmp(nam, "scalar_storage_order", 20) == 0);
                                        bool is_sec = (namelen == 7 && strncmp(nam, "section", 7) == 0) ||
                                                       (namelen == 11 && strncmp(nam, "__section__", 11) == 0);
                                        bool is_wk = (namelen == 4 && strncmp(nam, "weak", 4) == 0) ||
                                                      (namelen == 8 && strncmp(nam, "__weak__", 8) == 0);
                                        if (is_vs || is_al || is_as || is_md || is_sso || is_sec || is_wk) {
                                            /* find the matching ')' */
                                            int nest = 0;
                                            const char *close = NULL;
                                            for (const char *r = q; r < it + il; r++) {
                                                if (*r == '(') nest++;
                                                else if (*r == ')' && --nest == 0) { close = r; break; }
                                            }
                                            if (close) {
                                                ex = q + 1;
                                                el = (size_t)(close - ex);
                                                ex_marker = is_vs ? 1 : (is_al ? 2 : (is_as ? 3 : (is_md ? 4 : (is_sso ? 5 : (is_sec ? 6 : 7)))));
                                            }
                                            break;
                                        }
                                    }
                                    depth++;
                                } else if (*q == ')') {
                                    if (depth > 0) depth--;
                                }
                            }
                            if (!ex) {
                                /* Check for bare `aligned` / `__aligned__` /
                                 * `weak` / `__weak__` without parens (GCC
                                 * extensions: default alignment 16; weak is
                                 * argument-free). */
                                for (const char *q = it; q < it + il; ) {
                                    while (q < it + il && (*q == ' ' || *q == '\t' || *q == ',')) q++;
                                    if (q >= it + il) break;
                                    const char *start = q;
                                    while (q < it + il && (isalnum((unsigned char)*q) || *q == '_')) q++;
                                    size_t len = (size_t)(q - start);
                                    if ((len == 7 && strncmp(start, "aligned", 7) == 0) ||
                                        (len == 11 && strncmp(start, "__aligned__", 11) == 0)) {
                                        const char *nxt = q;
                                        while (nxt < it + il && (*nxt == ' ' || *nxt == '\t')) nxt++;
                                        if (nxt >= it + il || *nxt != '(') {
                                            ex = "16";
                                            el = 2;
                                            ex_marker = 2;
                                            break;
                                        }
                                    } else if ((len == 4 && strncmp(start, "weak", 4) == 0) ||
                                                 (len == 8 && strncmp(start, "__weak__", 8) == 0)) {
                                        ex = "";
                                        el = 0;
                                        ex_marker = 7;
                                        break;
                                    }
                                    if (q == start) q++;
                                }
                            }
                            if (!ex) continue;
                            char expr_text[512];
                            if (el > sizeof(expr_text) - 1) el = sizeof(expr_text) - 1;
                            memcpy(expr_text, ex, el);
                            expr_text[el] = '\0';
                            /* scalar_storage_order("little-endian") is the
                             * default on LE hosts - skip the element; the
                             * whole __attribute__ erases via its empty
                             * replacement.  Only "big-endian" needs the
                             * parser marker.  (Do NOT free args here: the
                             * outer expansion frees them - double free.) */
                            if (ex_marker == 5) {
                                if (strstr(expr_text, "big-endian") == NULL) continue;
                            }
                            /* weak: no argument - bare marker identifier
                             * (consumed by the file-scope loop, which marks
                             * the next function/object symbol weak). */
                            if (ex_marker == 7) el = 0;
                            /* Expand macros inside the size expression
                             * (e.g. __vector_size__(sizeof(__INTPTR_TYPE__))). */
                            ZeroCCBuffer expr_buf;
                            zero_cc_buf_init(&expr_buf, el + 16);
                            {
                                size_t expr_saved_line = cpp->line_num;
                                zero_cc_cpp_process_text(cpp, expr_text, &expr_buf);
                                cpp->line_num = expr_saved_line;
                            }
                            size_t el2 = expr_buf.length;
                            char *et2 = expr_buf.data;
                            char marker[600];
                            const char *mk = (ex_marker == 1) ? "__zc_vector_size__" : (ex_marker == 2 ? "__zc_aligned__" : (ex_marker == 3 ? "__zc_alias__" : (ex_marker == 4 ? "__zc_mode__" : (ex_marker == 6 ? "__zc_section__" : (ex_marker == 7 ? "__zc_weak__" : "__zc_sso_be__")))));
                            if (el2 > sizeof(marker) - 22) { el2 = sizeof(marker) - 22; }
                            if (ex_marker == 5 || ex_marker == 7) {
                                /* No argument: bare marker identifier. */
                                snprintf(marker, sizeof(marker), "%s", mk);
                            } else {
                                snprintf(marker, sizeof(marker), "%s(%.*s)", mk, (int)el2, et2);
                            }
                            zero_cc_buf_free(&expr_buf);
                            zero_cc_buf_puts(out, marker);
                            vec_marker_done = true;
                        }
                    }
                    if (vec_marker_done) {
                        for (int k = 0; k < arg_count; k++) free(args[k]);
                        continue;
                    }

                    const char *b = m->replacement;
                    ZeroCCBuffer temp_buf;
                    zero_cc_buf_init(&temp_buf, 256);
                    /* Blue from here - AFTER argument pre-expansion, so
                     * call-site arguments that mention M still expand -
                     * through the rescan of the substituted result. */
                    if (cpp->expanding_top < 39) cpp->expanding_stack[++cpp->expanding_top] = (int)(m - cpp->macros);
                    cpp->expand_depth++;
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
                            /* Did the left side end in a comma? The GNU
                             * `x, ## __VA_ARGS__` idiom needs it: empty
                             * variadic -> drop the comma; non-empty ->
                             * keep the comma and do NOT paste. */
                            left_comma = (temp_buf.length > 0 &&
                                          temp_buf.data[temp_buf.length - 1] == ',');
                            b += 2;
                            while (*b == ' ' || *b == '\t') b++;
                            paste_len = 0;
                            paste_buf[0] = '\0';
                            pending_paste = true;
                            continue;
                        }

                        /* Wide/UTF string & character literal prefixes in macro body:
                         * L'X', L"s", u'X', u"s", U'X', U"s", u8"s".
                         * The prefix is not an identifier, so parameter substitution
                         * must not rewrite e.g. L'1' into 0'1' (c-torture 970214-2). */
                        if ((*b == 'L' || *b == 'u' || *b == 'U') && (b[1] == '\'' || b[1] == '"')) {
                            char pfx = *b++;
                            char quote = *b;
                            if (pending_paste) {
                                paste_len += snprintf(paste_buf + paste_len, sizeof(paste_buf) - paste_len, "%c%c", pfx, *b++);
                                while (*b && *b != quote) {
                                    if (*b == '\\' && b[1]) {
                                        paste_len += snprintf(paste_buf + paste_len, sizeof(paste_buf) - paste_len, "%c", *b++);
                                    }
                                    paste_len += snprintf(paste_buf + paste_len, sizeof(paste_buf) - paste_len, "%c", *b++);
                                }
                                if (*b == quote) {
                                    paste_len += snprintf(paste_buf + paste_len, sizeof(paste_buf) - paste_len, "%c", *b++);
                                }
                                zero_cc_buf_puts(&temp_buf, paste_buf);
                                paste_len = 0;
                                paste_buf[0] = '\0';
                                pending_paste = false;
                            } else {
                                zero_cc_buf_putc(&temp_buf, pfx);
                                zero_cc_buf_putc(&temp_buf, *b++);
                                while (*b && *b != quote) {
                                    if (*b == '\\' && b[1]) {
                                        zero_cc_buf_putc(&temp_buf, *b++);
                                    }
                                    zero_cc_buf_putc(&temp_buf, *b++);
                                }
                                if (*b == quote) zero_cc_buf_putc(&temp_buf, *b++);
                            }
                            continue;
                        }
                        if (b[0] == 'u' && b[1] == '8' && b[2] == '"') {
                            b += 2;
                            char quote = '"';
                            if (pending_paste) {
                                paste_len += snprintf(paste_buf + paste_len, sizeof(paste_buf) - paste_len, "u8%c", *b++);
                                while (*b && *b != quote) {
                                    if (*b == '\\' && b[1]) {
                                        paste_len += snprintf(paste_buf + paste_len, sizeof(paste_buf) - paste_len, "%c", *b++);
                                    }
                                    paste_len += snprintf(paste_buf + paste_len, sizeof(paste_buf) - paste_len, "%c", *b++);
                                }
                                if (*b == quote) {
                                    paste_len += snprintf(paste_buf + paste_len, sizeof(paste_buf) - paste_len, "%c", *b++);
                                }
                                zero_cc_buf_puts(&temp_buf, paste_buf);
                                paste_len = 0;
                                paste_buf[0] = '\0';
                                pending_paste = false;
                            } else {
                                zero_cc_buf_puts(&temp_buf, "u8");
                                zero_cc_buf_putc(&temp_buf, *b++);
                                while (*b && *b != quote) {
                                    if (*b == '\\' && b[1]) {
                                        zero_cc_buf_putc(&temp_buf, *b++);
                                    }
                                    zero_cc_buf_putc(&temp_buf, *b++);
                                }
                                if (*b == quote) zero_cc_buf_putc(&temp_buf, *b++);
                            }
                            continue;
                        }

                        /* String literals and character literals in macro body:
                         * copy verbatim without parameter substitution. */
                        if (*b == '"' || *b == '\'') {
                            char quote = *b;
                            if (pending_paste) {
                                paste_len += snprintf(paste_buf + paste_len, sizeof(paste_buf) - paste_len, "%c", *b++);
                                while (*b && *b != quote) {
                                    if (*b == '\\' && b[1]) {
                                        paste_len += snprintf(paste_buf + paste_len, sizeof(paste_buf) - paste_len, "%c", *b++);
                                    }
                                    paste_len += snprintf(paste_buf + paste_len, sizeof(paste_buf) - paste_len, "%c", *b++);
                                }
                                if (*b == quote) {
                                    paste_len += snprintf(paste_buf + paste_len, sizeof(paste_buf) - paste_len, "%c", *b++);
                                }
                                zero_cc_buf_puts(&temp_buf, paste_buf);
                                paste_len = 0;
                                paste_buf[0] = '\0';
                                pending_paste = false;
                            } else {
                                zero_cc_buf_putc(&temp_buf, *b++);
                                while (*b && *b != quote) {
                                    if (*b == '\\' && b[1]) {
                                        zero_cc_buf_putc(&temp_buf, *b++);
                                    }
                                    zero_cc_buf_putc(&temp_buf, *b++);
                                }
                                if (*b == quote) zero_cc_buf_putc(&temp_buf, *b++);
                            }
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
                                    const char *av = args[match];
                                    bool is_va = (strcmp(param_id, "__VA_ARGS__") == 0);
                                    if (is_va && left_comma && av[0] != '\0') {
                                        /* GNU `x, ## __VA_ARGS__` with a
                                         * NON-EMPTY variadic part: keep the
                                         * comma, no token paste (gcc:
                                         * VW(a,b,c) -> a, b, c, not ab, c). */
                                        zero_cc_buf_putc(&temp_buf, ' ');
                                        zero_cc_buf_puts(&temp_buf, av);
                                        pending_paste = false;
                                    } else {
                                        paste_len += snprintf(paste_buf + paste_len, sizeof(paste_buf) - paste_len, "%s", av);
                                        /* Empty right operand after a comma
                                         * (`x, ## y` with y empty): the GNU
                                         * comma drop swallows the preceding
                                         * comma. */
                                        if (paste_len == 0 && left_comma &&
                                            temp_buf.length > 0 &&
                                            temp_buf.data[temp_buf.length - 1] == ',') {
                                            temp_buf.length--;
                                            temp_buf.data[temp_buf.length] = '\0';
                                        }
                                        /* Flush immediately: a second `##`
                                         * in the same body must start a
                                         * fresh paste. */
                                        zero_cc_buf_puts(&temp_buf, paste_buf);
                                        paste_len = 0;
                                        paste_buf[0] = '\0';
                                        pending_paste = false;
                                    }
                                } else {
                                    /* Tokens must not fuse across the
                                     * substitution boundary: body `a.b` with
                                     * args 1,5 must stay the tokens 1 . 5,
                                     * not the pp-number 1.5 (gcc re-emits the
                                     * substitution token-by-token; the
                                     * character stream approximates that by
                                     * inserting a space at any boundary where
                                     * both sides could join into one token).
                                     * A space never MERGES tokens, so this is
                                     * tokenization-safe even when no fusion
                                     * would occur (v.size -> v .size, same
                                     * tokens). */
                                    const char *sub = args[match];
                                    if (temp_buf.length > 0 && sub[0] &&
                                        cpp_fusion_char(temp_buf.data[temp_buf.length - 1]) &&
                                        cpp_fusion_char(sub[0]))
                                        zero_cc_buf_putc(&temp_buf, ' ');
                                    zero_cc_buf_puts(&temp_buf, sub);
                                    size_t sub_len = strlen(sub);
                                    if (sub_len > 0 && *b &&
                                        cpp_fusion_char(sub[sub_len - 1]) &&
                                        cpp_fusion_char(*b))
                                        zero_cc_buf_putc(&temp_buf, ' ');
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
                    {
                        size_t body_saved_line = cpp->line_num;
                        zero_cc_cpp_process_text(cpp, temp_buf.data, out);
                        cpp->line_num = body_saved_line;
                    }
                    cpp->expand_depth--;
                    zero_cc_buf_free(&temp_buf);
                        if (cpp->expanding_top >= 0) cpp->expanding_top--;
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

        /* Phase-2 splice: backslash+newline is deleted before tokenization
         * (C99 5.1.1.2). A stray `\` in code - sqlite3.c has one in an
         * inlined UTF8 writer - must not reach the lexer. The newline
         * itself IS preserved (gcc -E emits the continuation on its own
         * physical line) and advances the line count (gcc counts the
         * spliced newline in __LINE__) - pre-Sprint-176 the splice
         * swallowed both, undercounting every __LINE__ after a spliced
         * line. String/char literals handle their own splices above. */
        if (*p == '\\' && p[1] == '\n') {
            zero_cc_buf_putc(out, '\n');
            at_line_start = true;
            cpp->line_num++;
            p += 2;
            continue;
        }

        zero_cc_buf_putc(out, *p++);
    }
}

/* Full-control entry point with dependency generation support
 * (-MMD/-MF/-MT) and -include force-includes. */
char* zero_cc_preprocess_ext3(const char *source, ZeroCCIncludeDirs *inc_dirs,
                                            ZeroCCDefines *defines, const char *main_file,
                                            char *err_out, size_t err_cap,
                                            const char *dep_target, const char *dep_file,
                                            int gen_deps,
                                            const char **force_includes,
                                            int force_include_count,
                                            const char *std_version,
                                            bool preproc_only) {
    ZeroCCPreprocessor *cpp = (ZeroCCPreprocessor *)calloc(1, sizeof(ZeroCCPreprocessor));
    if (!cpp) return strdup(source ? source : "");
    zero_cc_cpp_init(cpp);
    cpp->preproc_only = preproc_only;
    /* The dialect drives dialect-dependent virtual header content (the C23
     * stdbool.h omits the true/false definitions - they are keywords).
     * std_version is the driver-resolved __STDC_VERSION__ string, so the
     * mapping is a clean 3-way (the c/gnu split is not observable here). */
    if (std_version && strcmp(std_version, "202311L") == 0)
        cpp->std_dialect = ZC_STD_C23;
    else if (std_version && strcmp(std_version, "199901L") == 0)
        cpp->std_dialect = ZC_STD_C99;
    else
        cpp->std_dialect = ZC_STD_C11;
    zero_cc_cpp_define_builtin_macros(cpp, std_version);
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
            size_t finc_saved_line = cpp->line_num;
            cpp->filename = resolved_path[0] ? resolved_path : NULL;
            cpp->line_num = 1; /* forced include: main file still starts at 1 */
            zero_cc_cpp_process_text(cpp, inc_content, &out);
            cpp->line_num = finc_saved_line;
            cpp->filename = saved_filename;
            zero_cc_cpp_pop_include_dir(cpp, saved_inc_depth);
            free(inc_content);
        } else if (cpp->include_error[0] == '\0') {
            snprintf(cpp->include_error, sizeof(cpp->include_error),
                     "<command-line>: fatal error: %s: No such file or directory",
                     force_includes[fi]);
        }
    }

    cpp->line_num = 1; /* the main source file starts at line 1 */
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
char* zero_cc_preprocess_ext2(const char *source, ZeroCCIncludeDirs *inc_dirs,
                                            ZeroCCDefines *defines, const char *main_file,
                                            char *err_out, size_t err_cap,
                                            const char *std_version) {
    return zero_cc_preprocess_ext3(source, inc_dirs, defines, main_file, err_out, err_cap,
                                   NULL, NULL, 0, NULL, 0, std_version, false);
}

char* zero_cc_preprocess_ext(const char *source, ZeroCCIncludeDirs *inc_dirs, ZeroCCDefines *defines,
                             const char *std_version) {
    return zero_cc_preprocess_ext2(source, inc_dirs, defines, NULL, NULL, 0, std_version);
}

char* zero_cc_preprocess(const char *source, ZeroCCIncludeDirs *inc_dirs) {
    return zero_cc_preprocess_ext(source, inc_dirs, NULL, "201112L");
}




