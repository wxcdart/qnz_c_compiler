#ifndef ZERO_CC_CORE_UTILS_H
#define ZERO_CC_CORE_UTILS_H

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




#endif /* ZERO_CC_CORE_UTILS_H */
