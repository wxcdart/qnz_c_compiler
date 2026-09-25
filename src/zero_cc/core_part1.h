/* ==========================================================================
 *  zero_cc.h
 *  Single-Header C99 Freestanding Compiler, JIT Engine & ELF64 Linker.
 *  
 *  Pipeline:
 *    [Source C Code / zero.h Headers]
 *          │
 *          ▼
 *    1. Preprocessor (CPP): #include, #define (obj/func), #if/#ifdef/#ifndef, ##, #
 *          │
 *          ▼
 *    2. Type & Struct Registry: typedef, struct members, pointers (*), arrays ([])
 *          │
 *          ▼
 *    3. Parser & Multi-Function Compiler:
 *       - if/else, while, for, do/while, break, continue, return
 *       - GNU Statement Expressions ({ ... })
 *       - Struct member access (. and ->), Array indexing ([]), Pointer math
 *       - Multi-argument function calls & System V AMD64 ABI (rdi, rsi, rdx, rcx, r8, r9)
 *          │
 *          ▼
 *    4. Linker & Machine Code Emitters:
 *       - JIT Engine (mmap / dlsym host symbol resolution / execution)
 *       - ELF64 Object Emitter (.o with .text, .rodata, .data, .symtab, .rela)
 *       - ELF64 Executable Emitter (Standalone executable binary with _start)
 *  ========================================================================== */

#ifndef ZERO_CC_CORE_H
#define ZERO_CC_CORE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#ifdef __linux__
#include <sys/mman.h>
#include <elf.h>
#include <dlfcn.h>
#ifndef RTLD_DEFAULT
#define RTLD_DEFAULT ((void *) 0)
#endif
#endif

static inline size_t zero_strlcpy(char *dst, const char *src, size_t siz)
{
    char *d = dst;
    const char *s = src;
    size_t n = siz;
    if (n != 0) {
        while (--n != 0) {
            if ((*d++ = *s++) == '\0')
                break;
        }
    }
    if (n == 0) {
        if (siz != 0)
            *d = '\0'; /* null-terminate */
        while (*s++);
    }
    return(s - src - 1);
}

/* Target Architecture Detection and Mutually Exclusive Switches */
#if defined(ZERO_CC_TARGET_X86_64) && defined(ZERO_CC_TARGET_ARM64)
#error "ZERO_CC_TARGET_X86_64 and ZERO_CC_TARGET_ARM64 are mutually exclusive"
#endif
#if !defined(ZERO_CC_TARGET_X86_64) && !defined(ZERO_CC_TARGET_ARM64)
#if defined(__x86_64__) || defined(_M_X64)
#define ZERO_CC_TARGET_X86_64 1
#elif defined(__aarch64__) || defined(_M_ARM64)
#define ZERO_CC_TARGET_ARM64 1
#else
#define ZERO_CC_TARGET_X86_64 1
#endif
#endif

/* Register / Calling Convention Mapping */
typedef struct {
    const char *int_arg_regs[6];
    const char *fp_arg_regs[8];
    const char *int_ret;
    const char *fp_ret;
} ZeroCCRegMap;

static inline ZeroCCRegMap zero_cc_get_regmap(void) {
#if defined(ZERO_CC_TARGET_ARM64)
    ZeroCCRegMap m = {
        .int_arg_regs = { "x0","x1","x2","x3","x4","x5" },
        .fp_arg_regs  = { "d0","d1","d2","d3","d4","d5","d6","d7" },
        .int_ret = "x0",
        .fp_ret = "d0",
    };
    return m;
#else
    ZeroCCRegMap m = {
        .int_arg_regs = { "rdi","rsi","rdx","rcx","r8","r9" },
        .fp_arg_regs  = { "xmm0","xmm1","xmm2","xmm3","xmm4","xmm5","xmm6","xmm7" },
        .int_ret = "rax",
        .fp_ret = "xmm0",
    };
    return m;
#endif
}



/* --------------------------------------------------------------------------
 *  1. DYNAMIC BYTE & STRING BUFFERS
 *  -------------------------------------------------------------------------- */
typedef struct {
    char *data;
    size_t capacity;
    size_t length;
} ZeroCCBuffer;

static inline void zero_cc_buf_init(ZeroCCBuffer *b, size_t initial_cap) {
    b->capacity = initial_cap ? initial_cap : 256;
    b->length = 0;
    b->data = (char *)malloc(b->capacity);
    if (b->data) b->data[0] = '\0';
}

static inline void zero_cc_buf_free(ZeroCCBuffer *b) {
    if (b->data) {
        free(b->data);
        b->data = NULL;
    }
    b->capacity = 0;
    b->length = 0;
}

static inline void zero_cc_buf_ensure(ZeroCCBuffer *b, size_t extra) {
    if (b->length + extra + 1 >= b->capacity) {
        size_t new_cap = (b->capacity * 2 > b->length + extra + 1) ? b->capacity * 2 : b->length + extra + 256;
        b->data = (char *)realloc(b->data, new_cap);
        b->capacity = new_cap;
    }
}

static inline void zero_cc_buf_putc(ZeroCCBuffer *b, char c) {
    zero_cc_buf_ensure(b, 1);
    b->data[b->length++] = c;
    b->data[b->length] = '\0';
}

static inline void zero_cc_buf_puts(ZeroCCBuffer *b, const char *s) {
    if (!s) return;
    size_t len = strlen(s);
    zero_cc_buf_ensure(b, len);
    memcpy(b->data + b->length, s, len);
    b->length += len;
    b->data[b->length] = '\0';
}

static inline void zero_cc_buf_put_bytes(ZeroCCBuffer *b, const void *bytes, size_t len) {
    if (!bytes || !len) return;
    zero_cc_buf_ensure(b, len);
    memcpy(b->data + b->length, bytes, len);
    b->length += len;
}

/* --------------------------------------------------------------------------
 *  SharpArena Lesson #2: Dedicated Unmanaged Pointer Stack (ZeroPtrStack)
 *  Bypasses heap tracking for AST, Tokenizer, and Expression evaluation
 * -------------------------------------------------------------------------- */
typedef struct {
    void **ptrs;
    size_t capacity;
    size_t top;
} ZeroPtrStack;

static inline void zero_ptr_stack_init(ZeroPtrStack *st, size_t initial_cap) {
    st->capacity = initial_cap ? initial_cap : 128;
    st->top = 0;
    st->ptrs = (void **)malloc(st->capacity * sizeof(void *));
}

static inline void zero_ptr_stack_destroy(ZeroPtrStack *st) {
    if (st && st->ptrs) {
        free(st->ptrs);
        st->ptrs = NULL;
        st->capacity = 0;
        st->top = 0;
    }
}

static inline void zero_ptr_stack_push(ZeroPtrStack *st, void *ptr) {
    if (st->top >= st->capacity) {
        size_t new_cap = st->capacity * 2;
        st->ptrs = (void **)realloc(st->ptrs, new_cap * sizeof(void *));
        st->capacity = new_cap;
    }
    st->ptrs[st->top++] = ptr;
}

static inline void* zero_ptr_stack_pop(ZeroPtrStack *st) {
    if (!st || st->top == 0) return NULL;
    return st->ptrs[--st->top];
}

static inline void* zero_ptr_stack_peek(const ZeroPtrStack *st) {
    if (!st || st->top == 0) return NULL;
    return st->ptrs[st->top - 1];
}

static inline void zero_ptr_stack_clear(ZeroPtrStack *st) {
    if (st) st->top = 0;
}

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
#if defined(ZERO_CC_TARGET_X86_64)
    zero_cc_cpp_define_obj(cpp, "__x86_64__", "1");
    zero_cc_cpp_define_obj(cpp, "__LP64__", "1");
#elif defined(ZERO_CC_TARGET_ARM64)
    zero_cc_cpp_define_obj(cpp, "__aarch64__", "1");
    zero_cc_cpp_define_obj(cpp, "__LP64__", "1");
#endif
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
            "#define NULL ((void*)0)\n"
            "#define offsetof(type, member) ((size_t)&(((type*)0)->member))\n"
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
    if (strcmp(name, "stdlib.h") == 0) {
        return strdup(
            "#ifndef _STDLIB_H\n#define _STDLIB_H 1\n"
            "typedef unsigned long size_t;\n"
            "void* malloc(size_t size);\nvoid free(void* ptr);\nvoid* calloc(size_t nmemb, size_t size);\n"
            "void* realloc(void* ptr, size_t size);\nvoid exit(int status);\nint system(const char *command);\n"
            "long strtoll(const char *nptr, char **endptr, int base);\nint atoi(const char *nptr);\nint abs(int j);\n"
            "#endif\n"
        );
    }
    if (strcmp(name, "string.h") == 0) {
        return strdup(
            "#ifndef _STRING_H\n#define _STRING_H 1\n"
            "typedef unsigned long size_t;\n"
            "void* memcpy(void *dest, const void *src, size_t n);\n"
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
    if (strcmp(name, "stdio.h") == 0) {
        return strdup(
            "#ifndef _STDIO_H\n#define _STDIO_H 1\n"
            "typedef unsigned long size_t;\n"
            "typedef struct FILE FILE;\n"
            "extern FILE *stdin, *stdout, *stderr;\n"
            "#define SEEK_SET 0\n#define SEEK_CUR 1\n#define SEEK_END 2\n#define EOF (-1)\n"
            "int printf(const char *format, ...);\n"
            "int fprintf(FILE *stream, const char *format, ...);\n"
            "int sprintf(char *str, const char *format, ...);\n"
            "int snprintf(char *str, size_t size, const char *format, ...);\n"
            "int puts(const char *s);\nint putchar(int c);\n"
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
    if (strcmp(name, "stdarg.h") == 0) {
        return strdup(
            "#ifndef _STDARG_H\n#define _STDARG_H 1\n"
            "typedef void *va_list;\n"
            "#define va_start(ap, last) __builtin_va_start(ap, last)\n"
            "#define va_arg(ap, type) __builtin_va_arg(ap, type)\n"
            "#define va_end(ap) __builtin_va_end(ap)\n"
            "#define va_copy(dest, src) ((dest) = (src))\n"
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
    return NULL;
}

static inline char* zero_cc_cpp_resolve_include(ZeroCCPreprocessor *cpp, const char *filename, bool is_system) {
    char path[1024];
    /* 1. First check virtual freestanding system headers */
    char *vhdr = zero_cc_cpp_get_virtual_header(filename);
    if (vhdr) return vhdr;

    /* 2. Check local relative path */
    if (!is_system) {
        if (access(filename, R_OK) == 0) {
            return zero_cc_read_file(filename);
        }
    }
    /* 3. Check explicitly added include directories */
    for (size_t i = 0; i < cpp->include_dirs.count; i++) {
        snprintf(path, sizeof(path), "%s/%s", cpp->include_dirs.paths[i], filename);
        if (access(path, R_OK) == 0) {
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
            return zero_cc_read_file(path);
        }
    }
    return NULL;
}

static inline void zero_cc_cpp_process_text(ZeroCCPreprocessor *cpp, const char *src, ZeroCCBuffer *out);

static inline int64_t zero_cc_cpp_eval_expr(ZeroCCPreprocessor *cpp, const char *expr) {
    while (*expr && isspace((unsigned char)*expr)) expr++;
    if (!*expr) return 0;

    if (strncmp(expr, "defined", 7) == 0) {
        const char *p = expr + 7;
        while (*p && isspace((unsigned char)*p)) p++;
        bool paren = (*p == '(');
        if (paren) p++;
        while (*p && isspace((unsigned char)*p)) p++;
        char id[128]; size_t l = 0;
        while (*p && (isalnum((unsigned char)*p) || *p == '_') && l < 127) id[l++] = *p++;
        id[l] = '\0';
        return (zero_cc_cpp_find_macro(cpp, id) != NULL) ? 1 : 0;
    }

    if (isdigit((unsigned char)*expr)) {
        return strtoll(expr, NULL, 0);
    }
    if (isalpha((unsigned char)*expr) || *expr == '_') {
        char id[128]; size_t l = 0;
        const char *p = expr;
        while (*p && (isalnum((unsigned char)*p) || *p == '_') && l < 127) id[l++] = *p++;
        id[l] = '\0';
        ZeroCCMacro *m = zero_cc_cpp_find_macro(cpp, id);
        if (m && m->replacement) {
            return strtoll(m->replacement, NULL, 0);
        }
        return 0;
    }
    return 0;
}

static inline void zero_cc_cpp_process_text(ZeroCCPreprocessor *cpp, const char *src, ZeroCCBuffer *out) {
    if (!src) return;
    const char *p = src;
    bool at_line_start = true;

    while (*p) {
        /* Comments - process first */
        if (*p == '/' && p[1] == '/') {
            p += 2;
            while (*p && *p != '\n') p++;
            if (*p == '\n') { zero_cc_buf_putc(out, *p++); at_line_start = true; }
            continue;
        }
        if (*p == '/' && p[1] == '*') {
            p += 2;
            while (*p && !(*p == '*' && p[1] == '/')) {
                if (*p == '\n') { zero_cc_buf_putc(out, '\n'); at_line_start = true; }
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
                while (*p && isspace((unsigned char)*p)) p++;

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
                            if (pl > 0 && pcount < 32) {
                                params[pcount++] = strdup(pname);
                            }
                        }
                        if (*p == ')') p++;
