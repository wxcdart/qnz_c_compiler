#ifndef ZERO_CC_POSIX_LIBC_H
#define ZERO_CC_POSIX_LIBC_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

static inline char *zero_posix_strcpy(char *dest, const char *src) {
#if defined(ZERO_NOLIBC) || defined(ZERO_FREESTANDING)
    return zero_posix_baremetal_strcpy(dest, src);
#else
    return strcpy(dest, src);
#endif
}

static inline char *zero_posix_strncpy(char *dest, const char *src, size_t n) {
#if defined(ZERO_NOLIBC) || defined(ZERO_FREESTANDING)
    return zero_posix_baremetal_strncpy(dest, src, n);
#else
    return strncpy(dest, src, n);
#endif
}

static inline char *zero_posix_strcat(char *dest, const char *src) {
#if defined(ZERO_NOLIBC) || defined(ZERO_FREESTANDING)
    return zero_posix_baremetal_strcat(dest, src);
#else
    return strcat(dest, src);
#endif
}

static inline char *zero_posix_strncat(char *dest, const char *src, size_t n) {
#if defined(ZERO_NOLIBC) || defined(ZERO_FREESTANDING)
    return zero_posix_baremetal_strncat(dest, src, n);
#else
    return strncat(dest, src, n);
#endif
}

#ifndef zero_posix_malloc
#if defined(ZERO_NOLIBC) || defined(ZERO_FREESTANDING)
#include "zero_posix_baremetal.h"
#define ZERO_POSIX_BAREMETAL_INCLUDED 1
#define zero_posix_malloc(size)       zero_posix_baremetal_malloc(size)
#define zero_posix_free(ptr)          zero_posix_baremetal_free(ptr)
#define zero_posix_calloc(n, sz)      zero_posix_baremetal_calloc((n), (sz))
#define zero_posix_realloc(ptr, size) zero_posix_baremetal_realloc((ptr), (size))
#else
#define zero_posix_malloc(size)       malloc((size))
#define zero_posix_free(ptr)          free((ptr))
#define zero_posix_calloc(n, sz)      calloc((n), (sz))
#define zero_posix_realloc(ptr, size) realloc((ptr), (size))
#endif
#endif

#endif /* ZERO_CC_POSIX_LIBC_H */
