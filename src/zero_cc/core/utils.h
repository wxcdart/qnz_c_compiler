#ifndef ZERO_CC_CORE_UTILS_H
#define ZERO_CC_CORE_UTILS_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

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

#endif /* ZERO_CC_CORE_UTILS_H */
