#ifndef ZERO_CC_POSIX_SYSTEM_H
#define ZERO_CC_POSIX_SYSTEM_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>

static inline void* zero_posix_memcpy(void *dest, const void *src, size_t n) {
#if defined(ZERO_NOLIBC)
    return zero_nolibc_memcpy(dest, src, n);
#else
    return memcpy(dest, src, n);
#endif
}

static inline void* zero_posix_memset(void *s, int c, size_t n) {
#if defined(ZERO_NOLIBC)
    return zero_nolibc_memset(s, c, n);
#else
    return memset(s, c, n);
#endif
}

static inline void* zero_posix_memmove(void *dest, const void *src, size_t n) {
#if defined(ZERO_NOLIBC)
    return zero_nolibc_memmove(dest, src, n);
#else
    return memmove(dest, src, n);
#endif
}

static inline int zero_posix_memcmp(const void *s1, const void *s2, size_t n) {
#if defined(ZERO_NOLIBC)
    return zero_nolibc_memcmp(s1, s2, n);
#else
    return memcmp(s1, s2, n);
#endif
}

static inline size_t zero_posix_strlen(const char *s) {
#if defined(ZERO_NOLIBC)
    return zero_nolibc_strlen(s);
#else
    return strlen(s);
#endif
}

static inline int zero_posix_strcmp(const char *s1, const char *s2) {
#if defined(ZERO_NOLIBC)
    return zero_nolibc_strcmp(s1, s2);
#else
    return strcmp(s1, s2);
#endif
}

static inline int zero_posix_strncmp(const char *s1, const char *s2, size_t n) {
#if defined(ZERO_NOLIBC)
    return zero_nolibc_strncmp(s1, s2, n);
#else
    return strncmp(s1, s2, n);
#endif
}

/* File Watching System Abstractions (inotify) */
#if defined(ZERO_PLATFORM_POSIX) && !defined(ZERO_NOLIBC) && !defined(ZERO_FREESTANDING)
#include <sys/inotify.h>
#endif

static inline int zero_posix_inotify_init(void) {
#if defined(ZERO_PLATFORM_POSIX) && defined(IN_NONBLOCK)
    return inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
#elif defined(ZERO_PLATFORM_POSIX) && defined(SYS_inotify_init)
    return inotify_init();
#else
    return -1;
#endif
}

static inline int zero_posix_inotify_add_watch(int fd, const char *pathname, uint32_t mask) {
#if defined(ZERO_PLATFORM_POSIX) && !defined(ZERO_FREESTANDING)
    return inotify_add_watch(fd, pathname, mask);
#else
    (void)fd; (void)pathname; (void)mask;
    return -1;
#endif
}

static inline int zero_posix_inotify_rm_watch(int fd, int wd) {
#if defined(ZERO_PLATFORM_POSIX) && !defined(ZERO_FREESTANDING)
    return inotify_rm_watch(fd, wd);
#else
    (void)fd; (void)wd;
    return -1;
#endif
}

static inline int zero_posix_gethostname(char *name, size_t len) {
#if defined(ZERO_PLATFORM_WINDOWS)
    DWORD l = (DWORD)len;
    return GetComputerNameA(name, &l) ? 0 : -1;
#elif defined(ZERO_PLATFORM_POSIX) && !defined(ZERO_FREESTANDING) && !defined(ZERO_NOLIBC)
    return gethostname(name, len);
#else
    if (name && len > 0) zero_posix_snprintf(name, len, "localhost");
    return 0;
#endif
}

static inline int zero_posix_num_cpus(void) {
#if defined(ZERO_PLATFORM_WINDOWS)
    SYSTEM_INFO sysinfo;
    GetSystemInfo(&sysinfo);
    return (int)sysinfo.dwNumberOfProcessors;
#elif defined(ZERO_PLATFORM_POSIX) && !defined(ZERO_FREESTANDING) && !defined(ZERO_NOLIBC)
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return (n > 0) ? (int)n : 1;
#else
    return 1;
#endif
}

#ifdef __cplusplus
}
#endif

/* =========================================================================
 * Additional POSIX wrappers for libc functions
 * =========================================================================
 */

static inline int zero_posix_atexit(void (*func)(void)) {
#if defined(ZERO_NOLIBC) || defined(ZERO_FREESTANDING)
    return zero_posix_baremetal_atexit(func);
#else
    return atexit(func);
#endif
}

static inline clock_t zero_posix_clock(void) {
#if defined(ZERO_NOLIBC) || defined(ZERO_FREESTANDING)
    return zero_posix_baremetal_clock();
#else
    return clock();
#endif
}

static inline void zero_posix__exit(int status) {
#if defined(ZERO_NOLIBC) || defined(ZERO_FREESTANDING)
    zero_posix_baremetal__exit(status);
#else
    _exit(status);
#endif
}

static inline int zero_posix_abs(int j) {
#if defined(ZERO_NOLIBC) || defined(ZERO_FREESTANDING)
    return zero_posix_baremetal_abs(j);
#else
    return abs(j);
#endif
}

static inline long zero_posix_labs(long j) {
#if defined(ZERO_NOLIBC) || defined(ZERO_FREESTANDING)
    return zero_posix_baremetal_labs(j);
#else
    return labs(j);
#endif
}

static inline long long zero_posix_llabs(long long j) {
#if defined(ZERO_NOLIBC) || defined(ZERO_FREESTANDING)
    return zero_posix_baremetal_llabs(j);
#else
    return llabs(j);
#endif
}

static inline div_t zero_posix_div(int numer, int denom) {
#if defined(ZERO_NOLIBC) || defined(ZERO_FREESTANDING)
    return zero_posix_baremetal_div(numer, denom);
#else
    return div(numer, denom);
#endif
}

static inline ldiv_t zero_posix_ldiv(long numer, long denom) {
#if defined(ZERO_NOLIBC) || defined(ZERO_FREESTANDING)
    return zero_posix_baremetal_ldiv(numer, denom);
#else
    return ldiv(numer, denom);
#endif
}

static inline lldiv_t zero_posix_lldiv(long long numer, long long denom) {
#if defined(ZERO_NOLIBC) || defined(ZERO_FREESTANDING)
    return zero_posix_baremetal_lldiv(numer, denom);
#else
    return lldiv(numer, denom);
#endif
}



#endif /* ZERO_CC_POSIX_SYSTEM_H */
