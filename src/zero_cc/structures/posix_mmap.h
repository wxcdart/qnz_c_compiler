#ifndef ZERO_CC_POSIX_MMAP_H
#define ZERO_CC_POSIX_MMAP_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>

/* Standard File Descriptors & Access Constants */
#ifndef STDIN_FILENO
#define STDIN_FILENO  0
#endif
#ifndef STDOUT_FILENO
#define STDOUT_FILENO 1
#endif
#ifndef STDERR_FILENO
#define STDERR_FILENO 2
#endif

#ifndef O_RDONLY
#define O_RDONLY 0x0000
#endif
#ifndef O_WRONLY
#define O_WRONLY 0x0001
#endif
#ifndef O_RDWR
#define O_RDWR   0x0002
#endif
#ifndef O_CREAT
#define O_CREAT  0x0040
#endif
#ifndef O_TRUNC
#define O_TRUNC  0x0200
#endif
#ifndef O_APPEND
#define O_APPEND 0x0400
#endif

/* Memory Mapping Flags */
#ifndef PROT_READ
#define PROT_READ  0x1
#endif
#ifndef PROT_WRITE
#define PROT_WRITE 0x2
#endif
#ifndef PROT_EXEC
#define PROT_EXEC  0x4
#endif
#ifndef PROT_NONE
#define PROT_NONE  0x0
#endif

#ifndef MAP_SHARED
#define MAP_SHARED  0x01
#endif
#ifndef MAP_PRIVATE
#define MAP_PRIVATE 0x02
#endif
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS 0x20
#endif
#ifndef MAP_FAILED
#define MAP_FAILED ((void*)-1)
#endif

/* Memory Mapping Abstraction */
static inline void* zero_posix_mmap(void* addr, size_t length, int prot, int flags, int fd, size_t offset) {
#if defined(ZERO_PLATFORM_WINDOWS)
    (void)addr; (void)flags;
    if (length == 0) return MAP_FAILED;

    HANDLE hFile = (fd >= 0) ? (HANDLE)_get_osfhandle(fd) : INVALID_HANDLE_VALUE;
    DWORD flProtect = PAGE_READONLY;
    DWORD dwAccess = FILE_MAP_READ;

    if ((prot & PROT_WRITE) && (prot & PROT_READ)) {
        flProtect = PAGE_READWRITE;
        dwAccess = FILE_MAP_ALL_ACCESS;
    } else if (prot & PROT_WRITE) {
        flProtect = PAGE_READWRITE;
        dwAccess = FILE_MAP_WRITE;
    }

    HANDLE hMap = CreateFileMappingA(hFile, NULL, flProtect, (DWORD)((length >> 32) & 0xFFFFFFFF), (DWORD)(length & 0xFFFFFFFF), NULL);
    if (!hMap) return MAP_FAILED;

    void* ptr = MapViewOfFile(hMap, dwAccess, (DWORD)((offset >> 32) & 0xFFFFFFFF), (DWORD)(offset & 0xFFFFFFFF), length);
    CloseHandle(hMap);
    return ptr ? ptr : MAP_FAILED;
#elif !defined(ZERO_FREESTANDING) && !defined(ZERO_NO_POSIX) && !defined(ZERO_NOLIBC)
    return mmap(addr, length, prot, flags, fd, (off_t)offset);
#else
    (void)addr; (void)length; (void)prot; (void)flags; (void)fd; (void)offset;
    return MAP_FAILED;
#endif
}

static inline int zero_posix_munmap(void* addr, size_t length) {
#if defined(ZERO_PLATFORM_WINDOWS)
    (void)length;
    return UnmapViewOfFile(addr) ? 0 : -1;
#elif !defined(ZERO_FREESTANDING) && !defined(ZERO_NO_POSIX) && !defined(ZERO_NOLIBC)
    return munmap(addr, length);
#else
    (void)addr; (void)length;
    return -1;
#endif
}

/* High-Resolution Clock & Sleep Abstraction */
static inline void zero_posix_usleep(uint64_t usec) {
#if defined(ZERO_PLATFORM_WINDOWS)
    Sleep((DWORD)((usec + 999) / 1000));
#elif !defined(ZERO_FREESTANDING) && !defined(ZERO_NO_POSIX) && !defined(ZERO_NOLIBC)
    #ifndef _POSIX_C_SOURCE
        #error "_POSIX_C_SOURCE not defined"
    #endif
    struct timespec req;
    req.tv_sec = (time_t)(usec / 1000000);
    req.tv_nsec = (long)((usec % 1000000) * 1000L);
    nanosleep(&req, NULL);
#else
    (void)usec;
#endif
}

static inline int zero_posix_sleep_ms(unsigned int ms) {
    zero_posix_usleep((uint64_t)ms * 1000ULL);
    return 0;
}

#if !defined(ZERO_FREESTANDING) && !defined(ZERO_NO_POSIX) && !defined(ZERO_NOLIBC)
#if defined(_POSIX_C_SOURCE) || defined(_GNU_SOURCE) || defined(_DEFAULT_SOURCE) || defined(__APPLE__) || defined(__linux__)
#include <time.h>
#endif
#endif

static inline int zero_posix_clock_gettime(int clk_id, struct timespec *ts) {
    if (!ts) return -1;
#if !defined(ZERO_FREESTANDING) && !defined(ZERO_NO_POSIX) && !defined(ZERO_NOLIBC) && !defined(ZERO_PLATFORM_WINDOWS)
#if defined(CLOCK_MONOTONIC)
    return clock_gettime(clk_id == 0 ? CLOCK_MONOTONIC : clk_id, ts);
#else
    return clock_gettime(clk_id, ts);
#endif
#else
    (void)clk_id;
    ts->tv_sec = 0;
    ts->tv_nsec = 0;
    return -1;
#endif
}

static inline int zero_posix_clock_gettime_sec_nsec(uint64_t *out_sec, uint64_t *out_nsec) {
#if !defined(ZERO_FREESTANDING) && !defined(ZERO_NO_POSIX) && !defined(ZERO_NOLIBC) && !defined(ZERO_PLATFORM_WINDOWS)
    struct timespec ts;
    int clk = 0;
#if defined(CLOCK_MONOTONIC)
    clk = CLOCK_MONOTONIC;
#endif
    if (clock_gettime(clk, &ts) == 0) {
        if (out_sec) *out_sec = (uint64_t)ts.tv_sec;
        if (out_nsec) *out_nsec = (uint64_t)ts.tv_nsec;
        return 0;
    }
#endif
    if (out_sec) *out_sec = 0;
    if (out_nsec) *out_nsec = 0;
    return -1;
}

static inline uint64_t zero_posix_clock_gettime_ns(void) {
    uint64_t sec = 0, nsec = 0;
    if (zero_posix_clock_gettime_sec_nsec(&sec, &nsec) == 0) {
        return sec * 1000000000ULL + nsec;
    }
    return 0;
}

/* Terminal Sizing Abstraction */
typedef struct ZeroPosixTermSize {
    int rows;
    int cols;
} ZeroPosixTermSize;

static inline int zero_posix_get_terminal_size(ZeroPosixTermSize* size) {
    if (!size) return -1;
#if defined(ZERO_PLATFORM_WINDOWS)
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi)) {
        size->cols = csbi.srWindow.Right - csbi.srWindow.Left + 1;
        size->rows = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
        return 0;
    }
#elif !defined(ZERO_FREESTANDING) && !defined(ZERO_NO_POSIX) && !defined(ZERO_NOLIBC) && defined(TIOCGWINSZ)
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        size->cols = (int)ws.ws_col;
        size->rows = (int)ws.ws_row;
        return 0;
    }
#endif
    size->rows = 24;
    size->cols = 80;
    return 0;
}

static inline int zero_posix_isatty(int fd) {
    if (fd < 0) return 0;
#if defined(ZERO_PLATFORM_WINDOWS)
    return _isatty(fd);
#elif !defined(ZERO_FREESTANDING) && !defined(ZERO_NO_POSIX) && !defined(ZERO_NOLIBC)
    return isatty(fd);
#else
    return (fd == STDIN_FILENO || fd == STDOUT_FILENO || fd == STDERR_FILENO) ? 1 : 0;
#endif
}

/* POSIX File I/O Primitive Wrappers & Partial-Write Protection */
static inline int zero_posix_open(const char *pathname, int flags, mode_t mode) {
#if !defined(ZERO_FREESTANDING) && !defined(ZERO_NO_POSIX) && !defined(ZERO_NOLIBC) && !defined(ZERO_PLATFORM_WINDOWS)
    return open(pathname, flags, mode);
#else
    (void)pathname; (void)flags; (void)mode;
    return -1;
#endif
}

static inline int zero_posix_close(int fd) {
#if !defined(ZERO_FREESTANDING) && !defined(ZERO_NO_POSIX) && !defined(ZERO_NOLIBC) && !defined(ZERO_PLATFORM_WINDOWS)
    return close(fd);
#else
    (void)fd;
    return -1;
#endif
}

static inline ssize_t zero_posix_read(int fd, void *buf, size_t count) {
#if !defined(ZERO_FREESTANDING) && !defined(ZERO_NO_POSIX) && !defined(ZERO_NOLIBC) && !defined(ZERO_PLATFORM_WINDOWS)
    return read(fd, buf, count);
#else
    (void)fd; (void)buf; (void)count;
    return -1;
#endif
}

static inline ssize_t zero_posix_write(int fd, const void *buf, size_t count) {
#if !defined(ZERO_FREESTANDING) && !defined(ZERO_NO_POSIX) && !defined(ZERO_NOLIBC) && !defined(ZERO_PLATFORM_WINDOWS)
    return write(fd, buf, count);
#else
    (void)fd; (void)buf; (void)count;
    return -1;
#endif
}

static inline ssize_t zero_posix_write_all(int fd, const void *buf, size_t count) {
    if (!buf) return -1;
    size_t written = 0;
    const uint8_t *ptr = (const uint8_t *)buf;
    while (written < count) {
        ssize_t n = zero_posix_write(fd, ptr + written, count - written);
        if (n <= 0) {
            int err = zero_posix_errno();
            if (n < 0 && (err == EINTR || err == EAGAIN)) continue;
            return written > 0 ? (ssize_t)written : -1;
        }
        written += (size_t)n;
    }
    return (ssize_t)written;
}

static inline int zero_posix_vprintf(const char *fmt, va_list ap) {
#if !defined(ZERO_FREESTANDING) && !defined(ZERO_NO_POSIX) && !defined(ZERO_NOLIBC)
    return vprintf(fmt, ap);
#else
    char buf[4096];
    int len = zero_posix_vsnprintf(buf, sizeof(buf), fmt, ap);
    if (len > 0) {
        (void)zero_posix_write_all(STDOUT_FILENO, buf, (size_t)len < sizeof(buf) ? (size_t)len : (sizeof(buf) - 1));
    }
    return len;
#endif
}

static inline int zero_posix_printf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int len = zero_posix_vprintf(fmt, args);
    va_end(args);
    return len;
}

static inline ssize_t zero_posix_read_all(int fd, void *buf, size_t count) {
    if (!buf) return -1;
    size_t read_bytes = 0;
    uint8_t *ptr = (uint8_t *)buf;
    while (read_bytes < count) {
        ssize_t n = zero_posix_read(fd, ptr + read_bytes, count - read_bytes);
        if (n <= 0) {
            int err = zero_posix_errno();
            if (n < 0 && (err == EINTR || err == EAGAIN)) continue;
            break; /* EOF or unrecoverable error */
        }
        read_bytes += (size_t)n;
    }
    return (ssize_t)read_bytes;
}

static inline off_t zero_posix_lseek(int fd, off_t offset, int whence) {
#if !defined(ZERO_FREESTANDING) && !defined(ZERO_NO_POSIX) && !defined(ZERO_NOLIBC) && !defined(ZERO_PLATFORM_WINDOWS)
    return lseek(fd, offset, whence);
#else
    (void)fd; (void)offset; (void)whence;
    return (off_t)-1;
#endif
}

static inline int zero_posix_ftruncate(int fd, off_t length) {
#if !defined(ZERO_FREESTANDING) && !defined(ZERO_NO_POSIX) && !defined(ZERO_NOLIBC) && !defined(ZERO_PLATFORM_WINDOWS)
    return ftruncate(fd, length);
#else
    (void)fd; (void)length;
    return -1;
#endif
}

static inline int zero_posix_fsync(int fd) {
#if !defined(ZERO_FREESTANDING) && !defined(ZERO_NO_POSIX) && !defined(ZERO_NOLIBC) && !defined(ZERO_PLATFORM_WINDOWS)
    return fsync(fd);
#else
    (void)fd;
    return -1;
#endif
}

static inline int zero_posix_pipe(int pipefd[2]) {
#if !defined(ZERO_FREESTANDING) && !defined(ZERO_NO_POSIX) && !defined(ZERO_NOLIBC) && !defined(ZERO_PLATFORM_WINDOWS)
    return pipe(pipefd);
#else
    if (pipefd) { pipefd[0] = -1; pipefd[1] = -1; }
    return -1;
#endif
}

static inline int zero_posix_dup2(int oldfd, int newfd) {
#if !defined(ZERO_FREESTANDING) && !defined(ZERO_NO_POSIX) && !defined(ZERO_NOLIBC) && !defined(ZERO_PLATFORM_WINDOWS)
    return dup2(oldfd, newfd);
#else
    (void)oldfd; (void)newfd;
    return -1;
#endif
}

static inline int zero_posix_set_nonblocking(int fd) {
#if !defined(ZERO_FREESTANDING) && !defined(ZERO_NO_POSIX) && !defined(ZERO_NOLIBC) && !defined(ZERO_PLATFORM_WINDOWS) && defined(F_GETFL) && defined(F_SETFL)
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#else
    (void)fd;
    return -1;
#endif
}

/* Entropy & Randomness Abstraction */
static inline ssize_t zero_posix_getrandom(void *buf, size_t buflen) {
    if (!buf || buflen == 0) return 0;
#if !defined(ZERO_FREESTANDING) && !defined(ZERO_NO_POSIX) && !defined(ZERO_NOLIBC) && !defined(ZERO_PLATFORM_WINDOWS)
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        ssize_t r = read(fd, buf, buflen);
        close(fd);
        if (r > 0) return r;
    }
    return -1;
#else
    (void)buf; (void)buflen;
    return -1;
#endif
}

/* High-Resolution Nanosleep Abstraction */
static inline int zero_posix_nanosleep(const struct timespec *req, struct timespec *rem) {
    if (!req) return -1;
#if defined(ZERO_PLATFORM_WINDOWS)
    uint64_t ms = (uint64_t)req->tv_sec * 1000ULL + (uint64_t)(req->tv_nsec / 1000000L);
    Sleep((DWORD)ms);
    if (rem) { rem->tv_sec = 0; rem->tv_nsec = 0; }
    return 0;
#elif !defined(ZERO_FREESTANDING) && !defined(ZERO_NO_POSIX) && !defined(ZERO_NOLIBC)
    return nanosleep(req, rem);
#else
    (void)rem;
    return -1;
#endif
}

/* Environment Variable Query Abstraction */
static inline const char *zero_posix_getenv(const char *name) {
    if (!name) return NULL;
#if !defined(ZERO_FREESTANDING) && !defined(ZERO_NO_POSIX) && !defined(ZERO_NOLIBC)
    return getenv(name);
#else
    (void)name;
    return NULL;
#endif
}

/* Memory Protection Modification Abstraction */
static inline int zero_posix_mprotect(void *addr, size_t len, int prot) {
    if (!addr || len == 0) return -1;
#if defined(ZERO_PLATFORM_WINDOWS)
    DWORD oldProt;
    DWORD flProtect = PAGE_NOACCESS;
    if ((prot & PROT_READ) && (prot & PROT_WRITE)) flProtect = PAGE_READWRITE;
    else if (prot & PROT_READ) flProtect = PAGE_READONLY;
    return VirtualProtect(addr, len, flProtect, &oldProt) ? 0 : -1;
#elif !defined(ZERO_FREESTANDING) && !defined(ZERO_NO_POSIX) && !defined(ZERO_NOLIBC)
    return mprotect(addr, len, prot);
#else
    (void)addr; (void)len; (void)prot;
    return -1;
#endif
}

/* Filesystem Metadata & Directory Management Abstractions */
static inline int zero_posix_stat(const char *path, struct stat *st) {
    if (!path || !st) return -1;
#if !defined(ZERO_FREESTANDING) && !defined(ZERO_NO_POSIX) && !defined(ZERO_NOLIBC)
    return stat(path, st);
#else
    (void)path; (void)st;
    return -1;
#endif
}

static inline int zero_posix_lstat(const char *path, struct stat *st) {
    if (!path || !st) return -1;
#if !defined(ZERO_FREESTANDING) && !defined(ZERO_NO_POSIX) && !defined(ZERO_NOLIBC)
#if defined(ZERO_PLATFORM_WINDOWS)
    return stat(path, st);
#else
    return lstat(path, st);
#endif
#else
    (void)path; (void)st;
    return -1;
#endif

#endif /* ZERO_CC_POSIX_MMAP_H */
