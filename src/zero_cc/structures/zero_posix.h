/* =========================================================================
 * zero_posix.h - POSIX Platform Abstraction Layer
 * Portable cross-platform abstractions for memory mapping, high-res clocks,
 * terminal sizing, sleep functions, and file I/O.
 * Can be compiled out on bare-metal RTOS with -DZERO_NO_POSIX or -DZERO_FREESTANDING.
 * ========================================================================= */

#ifndef ZERO_POSIX_H
#define ZERO_POSIX_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#if !defined(ZERO_FREESTANDING) && !defined(ZERO_NO_POSIX)
#if defined(_WIN32) || defined(_WIN64)
    #define ZERO_PLATFORM_WINDOWS 1
    #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
    #include <io.h>
    #include <process.h>
    #include <direct.h>
#else
    #define ZERO_PLATFORM_POSIX 1
    #ifndef _POSIX_C_SOURCE
        #define _POSIX_C_SOURCE 200809L
    #endif
#ifndef ZERO_NOLIBC
    #include <time.h>
    #include <unistd.h>
    #include <sys/types.h>
    #include <sys/stat.h>
    #include <sys/mman.h>
    #include <sys/ioctl.h>
    #include <fcntl.h>
    #include <errno.h>
    #include <stdio.h>
    #include <string.h>
#endif /* ZERO_NOLIBC */
#endif
#endif

#include <stdarg.h>

#ifndef zero_posix_malloc
#if defined(ZERO_NOLIBC) || defined(ZERO_FREESTANDING)
#include "zero_posix_baremetal.h"
#define ZERO_POSIX_BAREMETAL_INCLUDED 1
#else
#include <stdlib.h>
#define zero_posix_malloc(sz)       malloc(sz)
#define zero_posix_free(ptr)        free(ptr)
#define zero_posix_calloc(n, sz)    calloc((n), (sz))
#define zero_posix_realloc(ptr, sz) realloc((ptr), (sz))
#endif
#endif

#if defined(ZERO_NOLIBC) || defined(ZERO_FREESTANDING)
#include "zero_nolibc.h"
#define zero_posix_snprintf zero_nolibc_snprintf
#define zero_posix_vsnprintf zero_nolibc_vsnprintf
typedef struct ZeroPosixFile FILE;
#ifndef stdin
#define stdin  ((FILE*)(uintptr_t)1)
#endif
#ifndef stdout
#define stdout ((FILE*)(uintptr_t)2)
#endif
#ifndef stderr
#define stderr ((FILE*)(uintptr_t)3)
#endif
#else
#include <stdio.h>
#define zero_posix_snprintf snprintf
#define zero_posix_vsnprintf vsnprintf
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * POSIX Errno Constants & Error Management Abstractions
 * ========================================================================= */
#ifndef EPERM
#define EPERM        1   /* Operation not permitted */
#endif
#ifndef ENOENT
#define ENOENT       2   /* No such file or directory */
#endif
#ifndef ESRCH
#define ESRCH        3   /* No such process */
#endif
#ifndef EINTR
#define EINTR        4   /* Interrupted system call */
#endif
#ifndef EIO
#define EIO          5   /* I/O error */
#endif
#ifndef ENXIO
#define ENXIO        6   /* No such device or address */
#endif
#ifndef E2BIG
#define E2BIG        7   /* Argument list too long */
#endif
#ifndef ENOEXEC
#define ENOEXEC      8   /* Exec format error */
#endif
#ifndef EBADF
#define EBADF        9   /* Bad file descriptor */
#endif
#ifndef ECHILD
#define ECHILD      10   /* No child processes */
#endif
#ifndef EAGAIN
#define EAGAIN      11   /* Try again / Resource temporarily unavailable */
#endif
#ifndef ENOMEM
#define ENOMEM      12   /* Out of memory */
#endif
#ifndef EACCES
#define EACCES      13   /* Permission denied */
#endif
#ifndef EFAULT
#define EFAULT      14   /* Bad address */
#endif
#ifndef EBUSY
#define EBUSY       16   /* Device or resource busy */
#endif
#ifndef EEXIST
#define EEXIST      17   /* File exists */
#endif
#ifndef EXDEV
#define EXDEV       18   /* Cross-device link */
#endif
#ifndef ENODEV
#define ENODEV      19   /* No such device */
#endif
#ifndef ENOTDIR
#define ENOTDIR     20   /* Not a directory */
#endif
#ifndef EISDIR
#define EISDIR      21   /* Is a directory */
#endif
#ifndef EINVAL
#define EINVAL      22   /* Invalid argument */
#endif
#ifndef ENFILE
#define ENFILE      23   /* File table overflow */
#endif
#ifndef EMFILE
#define EMFILE      24   /* Too many open files */
#endif
#ifndef ENOTTY
#define ENOTTY      25   /* Not a typewriter */
#endif
#ifndef ETXTBSY
#define ETXTBSY     26   /* Text file busy */
#endif
#ifndef EFBIG
#define EFBIG       27   /* File too large */
#endif
#ifndef ENOSPC
#define ENOSPC      28   /* No space left on device */
#endif
#ifndef ESPIPE
#define ESPIPE      29   /* Illegal seek */
#endif
#ifndef EROFS
#define EROFS       30   /* Read-only file system */
#endif
#ifndef EMLINK
#define EMLINK      31   /* Too many links */
#endif
#ifndef EPIPE
#define EPIPE       32   /* Broken pipe */
#endif
#ifndef EDOM
#define EDOM        33   /* Math argument out of domain of func */
#endif
#ifndef ERANGE
#define ERANGE      34   /* Math result not representable */
#endif
#ifndef EDEADLK
#define EDEADLK     35   /* Resource deadlock would occur */
#endif
#ifndef ENAMETOOLONG
#define ENAMETOOLONG 36  /* File name too long */
#endif
#ifndef ENOSYS
#define ENOSYS      38   /* Invalid system call number */
#endif
#ifndef ENOTEMPTY
#define ENOTEMPTY   39   /* Directory not empty */
#endif
#ifndef ELOOP
#define ELOOP       40   /* Too many symbolic links encountered */
#endif
#ifndef EWOULDBLOCK
#define EWOULDBLOCK EAGAIN
#endif
#ifndef ECONNRESET
#define ECONNRESET  104  /* Connection reset by peer */
#endif
#ifndef ENOBUFS
#define ENOBUFS     105  /* No buffer space available */
#endif
#ifndef EISCONN
#define EISCONN     106  /* Transport endpoint is already connected */
#endif
#ifndef ENOTCONN
#define ENOTCONN    107  /* Transport endpoint is not connected */
#endif
#ifndef ETIMEDOUT
#define ETIMEDOUT   110  /* Connection timed out */
#endif
#ifndef ECONNREFUSED
#define ECONNREFUSED 111 /* Connection refused */
#endif
#ifndef EHOSTUNREACH
#define EHOSTUNREACH 113 /* No route to host */
#endif
#ifndef EALREADY
#define EALREADY    114  /* Operation already in progress */
#endif
#ifndef EINPROGRESS
#define EINPROGRESS 115  /* Operation now in progress */
#endif

#if defined(ZERO_FREESTANDING) || defined(ZERO_NO_POSIX) || defined(ZERO_NOLIBC)
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_THREADS__)
static _Thread_local int g_zero_posix_errno = 0;
#else
static int g_zero_posix_errno = 0;
#endif
static inline int zero_posix_errno(void) {
    return g_zero_posix_errno;
}
static inline void zero_posix_set_errno(int err) {
    g_zero_posix_errno = err;
}
#elif defined(ZERO_PLATFORM_WINDOWS)
static inline int zero_posix_errno(void) {
    DWORD win_err = GetLastError();
    switch (win_err) {
        case ERROR_SUCCESS: return 0;
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND: return ENOENT;
        case ERROR_ACCESS_DENIED: return EACCES;
        case ERROR_TOO_MANY_OPEN_FILES: return EMFILE;
        case ERROR_NOT_ENOUGH_MEMORY:
        case ERROR_OUTOFMEMORY: return ENOMEM;
        case ERROR_INVALID_HANDLE: return EBADF;
        case ERROR_NOT_SAME_DEVICE: return EXDEV;
        case ERROR_FILE_EXISTS:
        case ERROR_ALREADY_EXISTS: return EEXIST;
        case ERROR_BROKEN_PIPE: return EPIPE;
        case ERROR_DISK_FULL: return ENOSPC;
        case ERROR_SHARING_VIOLATION:
        case ERROR_LOCK_VIOLATION: return EACCES;
        default: return (int)win_err;
    }
}
static inline void zero_posix_set_errno(int err) {
    SetLastError((DWORD)err);
}
#else
static inline int zero_posix_errno(void) {
    return errno;
}
static inline void zero_posix_set_errno(int err) {
    errno = err;
}
#endif

static inline const char *zero_posix_strerror(int errnum) {
#if !defined(ZERO_FREESTANDING) && !defined(ZERO_NO_POSIX) && !defined(ZERO_NOLIBC) && !defined(ZERO_PLATFORM_WINDOWS)
    return strerror(errnum);
#else
    switch (errnum) {
        case 0: return "Success";
        case EPERM: return "Operation not permitted";
        case ENOENT: return "No such file or directory";
        case ESRCH: return "No such process";
        case EINTR: return "Interrupted system call";
        case EIO: return "Input/output error";
        case ENXIO: return "No such device or address";
        case E2BIG: return "Argument list too long";
        case ENOEXEC: return "Exec format error";
        case EBADF: return "Bad file descriptor";
        case ECHILD: return "No child processes";
        case EAGAIN: return "Resource temporarily unavailable";
        case ENOMEM: return "Cannot allocate memory";
        case EACCES: return "Permission denied";
        case EFAULT: return "Bad address";
        case EBUSY: return "Device or resource busy";
        case EEXIST: return "File exists";
        case EXDEV: return "Invalid cross-device link";
        case ENODEV: return "No such device";
        case ENOTDIR: return "Not a directory";
        case EISDIR: return "Is a directory";
        case EINVAL: return "Invalid argument";
        case ENFILE: return "Too many open files in system";
        case EMFILE: return "Too many open files";
        case ENOTTY: return "Inappropriate ioctl for device";
        case ETXTBSY: return "Text file busy";
        case EFBIG: return "File too large";
        case ENOSPC: return "No space left on device";
        case ESPIPE: return "Illegal seek";
        case EROFS: return "Read-only file system";
        case EMLINK: return "Too many links";
        case EPIPE: return "Broken pipe";
        case EDOM: return "Numerical argument out of domain";
        case ERANGE: return "Numerical result out of range";
        case EDEADLK: return "Resource deadlock avoided";
        case ENAMETOOLONG: return "File name too long";
        case ENOSYS: return "Function not implemented";
        case ENOTEMPTY: return "Directory not empty";
        case ELOOP: return "Too many levels of symbolic links";
        case ECONNRESET: return "Connection reset by peer";
        case ENOBUFS: return "No buffer space available";
        case EISCONN: return "Transport endpoint is already connected";
        case ENOTCONN: return "Transport endpoint is not connected";
        case ETIMEDOUT: return "Connection timed out";
        case ECONNREFUSED: return "Connection refused";
        case EHOSTUNREACH: return "No route to host";
        case EALREADY: return "Operation already in progress";
        case EINPROGRESS: return "Operation now in progress";
        default: return "Unknown error";
    }
#endif
}

#define zero_errno zero_posix_errno
#define zero_strerror zero_posix_strerror
#define zero_perror zero_posix_perror

static inline ssize_t zero_posix_write_all(int fd, const void *buf, size_t count);

static inline void zero_posix_perror(const char *s) {
    int err = zero_posix_errno();
    const char *errstr = zero_posix_strerror(err);
    if (s && *s) {
        size_t slen = 0;
        while (s[slen]) slen++;
        (void)zero_posix_write_all(STDERR_FILENO, s, slen);
        (void)zero_posix_write_all(STDERR_FILENO, ": ", 2);
    }
    if (errstr) {
        size_t elen = 0;
        while (errstr[elen]) elen++;
        (void)zero_posix_write_all(STDERR_FILENO, errstr, elen);
    } else {
        (void)zero_posix_write_all(STDERR_FILENO, "Unknown error", 13);
    }
    (void)zero_posix_write_all(STDERR_FILENO, "\n", 1);
}

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
}

static inline int zero_posix_fstat(int fd, struct stat *st) {
    if (fd < 0 || !st) return -1;
#if !defined(ZERO_FREESTANDING) && !defined(ZERO_NO_POSIX) && !defined(ZERO_NOLIBC)
    return fstat(fd, st);
#else
    (void)fd; (void)st;
    return -1;
#endif
}

static inline int zero_posix_access(const char *path, int mode) {
    if (!path) return -1;
#if !defined(ZERO_FREESTANDING) && !defined(ZERO_NO_POSIX) && !defined(ZERO_NOLIBC)
    return access(path, mode);
#else
    (void)path; (void)mode;
    return -1;
#endif
}

static inline int zero_posix_unlink(const char *path) {
    if (!path) return -1;
#if !defined(ZERO_FREESTANDING) && !defined(ZERO_NO_POSIX) && !defined(ZERO_NOLIBC)
    return unlink(path);
#else
    (void)path;
    return -1;
#endif
}

static inline int zero_posix_rename(const char *oldpath, const char *newpath) {
    if (!oldpath || !newpath) return -1;
#if !defined(ZERO_FREESTANDING) && !defined(ZERO_NO_POSIX) && !defined(ZERO_NOLIBC)
    return rename(oldpath, newpath);
#else
    (void)oldpath; (void)newpath;
    return -1;
#endif
}

static inline int zero_posix_mkdir(const char *path, mode_t mode) {
    if (!path) return -1;
#if !defined(ZERO_FREESTANDING) && !defined(ZERO_NO_POSIX) && !defined(ZERO_NOLIBC)
#if defined(ZERO_PLATFORM_WINDOWS)
    (void)mode;
    return _mkdir(path);
#else
    return mkdir(path, mode);
#endif
#else
    (void)path; (void)mode;
    return -1;
#endif
}

static inline int zero_posix_chmod(const char *path, mode_t mode) {
    if (!path) return -1;
#if !defined(ZERO_FREESTANDING) && !defined(ZERO_NO_POSIX) && !defined(ZERO_NOLIBC)
#if defined(ZERO_PLATFORM_WINDOWS)
    (void)mode;
    return _chmod(path, _S_IREAD | _S_IWRITE);
#else
    return chmod(path, mode);
#endif
#else
    (void)path; (void)mode;
    return -1;
#endif
}

static inline int zero_posix_rmdir(const char *path) {
    if (!path) return -1;
#if !defined(ZERO_FREESTANDING) && !defined(ZERO_NO_POSIX) && !defined(ZERO_NOLIBC)
    return rmdir(path);
#else
    (void)path;
    return -1;
#endif
}

static inline char* zero_posix_getcwd(char *buf, size_t size) {
    if (!buf || size == 0) return NULL;
#if defined(ZERO_PLATFORM_WINDOWS)
    return _getcwd(buf, (int)size);
#elif !defined(ZERO_FREESTANDING) && !defined(ZERO_NO_POSIX) && !defined(ZERO_NOLIBC)
    return getcwd(buf, size);
#else
    (void)buf; (void)size;
    return NULL;
#endif
}

static inline int zero_posix_chdir(const char *path) {
    if (!path) return -1;
#if defined(ZERO_PLATFORM_WINDOWS)
    return _chdir(path);
#elif !defined(ZERO_FREESTANDING) && !defined(ZERO_NO_POSIX) && !defined(ZERO_NOLIBC)
    return chdir(path);
#else
    (void)path;
    return -1;
#endif
}

/* Process Execution & Termination Abstractions */
static inline int zero_posix_system(const char *command) {
    if (!command) return -1;
#if !defined(ZERO_FREESTANDING) && !defined(ZERO_NO_POSIX) && !defined(ZERO_NOLIBC)
    return system(command);
#else
    (void)command;
    return -1;
#endif
}

static inline void zero_posix_exit(int status) {
#if !defined(ZERO_FREESTANDING) && !defined(ZERO_NO_POSIX) && !defined(ZERO_NOLIBC)
    exit(status);
#elif defined(ZERO_NOLIBC)
    zero_nolibc_exit(status);
#else
    (void)status;
    for (;;) {}
#endif
}

/* Stream File I/O Abstractions */
static inline FILE* zero_posix_fopen(const char *path, const char *mode) {
    if (!path || !mode) return NULL;
#if !defined(ZERO_FREESTANDING) && !defined(ZERO_NO_POSIX) && !defined(ZERO_NOLIBC)
    return fopen(path, mode);
#else
    (void)path; (void)mode;
    return NULL;
#endif
}

static inline int zero_posix_fclose(FILE *stream) {
    if (!stream) return -1;
#if !defined(ZERO_FREESTANDING) && !defined(ZERO_NO_POSIX) && !defined(ZERO_NOLIBC)
    return fclose(stream);
#else
    (void)stream;
    return -1;
#endif
}

static inline size_t zero_posix_fread(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    if (!ptr || !stream || size == 0 || nmemb == 0) return 0;
#if !defined(ZERO_FREESTANDING) && !defined(ZERO_NO_POSIX) && !defined(ZERO_NOLIBC)
    return fread(ptr, size, nmemb, stream);
#else
    (void)ptr; (void)size; (void)nmemb; (void)stream;
    return 0;
#endif
}

static inline size_t zero_posix_fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream) {
    if (!ptr || !stream || size == 0 || nmemb == 0) return 0;
#if !defined(ZERO_FREESTANDING) && !defined(ZERO_NO_POSIX) && !defined(ZERO_NOLIBC)
    return fwrite(ptr, size, nmemb, stream);
#else
    (void)ptr; (void)size; (void)nmemb; (void)stream;
    return 0;
#endif
}

static inline char* zero_posix_fgets(char *s, int size, FILE *stream) {
    if (!s || !stream || size <= 0) return NULL;
#if !defined(ZERO_FREESTANDING) && !defined(ZERO_NO_POSIX) && !defined(ZERO_NOLIBC)
    return fgets(s, size, stream);
#else
    (void)s; (void)size; (void)stream;
    return NULL;
#endif
}

static inline int zero_posix_fputs(const char *s, FILE *stream) {
    if (!s || !stream) return -1;
#if !defined(ZERO_FREESTANDING) && !defined(ZERO_NO_POSIX) && !defined(ZERO_NOLIBC)
    return fputs(s, stream);
#else
    (void)s; (void)stream;
    return -1;
#endif
}

static inline int zero_posix_vfprintf(FILE *stream, const char *fmt, va_list ap) {
    if (!stream || !fmt) return -1;
#if !defined(ZERO_FREESTANDING) && !defined(ZERO_NO_POSIX) && !defined(ZERO_NOLIBC)
    return vfprintf(stream, fmt, ap);
#else
    char buf[4096];
    int len = zero_posix_vsnprintf(buf, sizeof(buf), fmt, ap);
    if (len > 0) {
        int fd = (stream == stderr) ? STDERR_FILENO : STDOUT_FILENO;
        (void)zero_posix_write_all(fd, buf, (size_t)len < sizeof(buf) ? (size_t)len : (sizeof(buf) - 1));
    }
    return len;
#endif
}

static inline int zero_posix_fprintf(FILE *stream, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int ret = zero_posix_vfprintf(stream, fmt, args);
    va_end(args);
    return ret;
}

static inline FILE* zero_posix_stdin(void) {
    return stdin;
}

static inline FILE* zero_posix_stdout(void) {
    return stdout;
}

static inline FILE* zero_posix_stderr(void) {
    return stderr;
}

#ifndef SEEK_SET
#define SEEK_SET 0
#endif
#ifndef SEEK_CUR
#define SEEK_CUR 1
#endif
#ifndef SEEK_END
#define SEEK_END 2
#endif

static inline int zero_posix_fseek(FILE *stream, long offset, int whence) {
    if (!stream) return -1;
#if !defined(ZERO_FREESTANDING) && !defined(ZERO_NO_POSIX) && !defined(ZERO_NOLIBC)
    return fseek(stream, offset, whence);
#else
    (void)stream; (void)offset; (void)whence;
    return -1;
#endif
}

static inline long zero_posix_ftell(FILE *stream) {
    if (!stream) return -1L;
#if !defined(ZERO_FREESTANDING) && !defined(ZERO_NO_POSIX) && !defined(ZERO_NOLIBC)
    return ftell(stream);
#else
    (void)stream;
    return -1L;
#endif
}

static inline void zero_posix_rewind(FILE *stream) {
    if (!stream) return;
#if !defined(ZERO_FREESTANDING) && !defined(ZERO_NO_POSIX) && !defined(ZERO_NOLIBC)
    rewind(stream);
#else
    (void)stream;
#endif
}

static inline int zero_posix_fflush(FILE *stream) {
#if !defined(ZERO_FREESTANDING) && !defined(ZERO_NO_POSIX) && !defined(ZERO_NOLIBC)
    return fflush(stream);
#else
    (void)stream;
    return 0;
#endif
}

static inline int zero_posix_feof(FILE *stream) {
    if (!stream) return 1;
#if !defined(ZERO_FREESTANDING) && !defined(ZERO_NO_POSIX) && !defined(ZERO_NOLIBC)
    return feof(stream);
#else
    (void)stream;
    return 1;
#endif
}

static inline int zero_posix_ferror(FILE *stream) {
    if (!stream) return -1;
#if !defined(ZERO_FREESTANDING) && !defined(ZERO_NO_POSIX) && !defined(ZERO_NOLIBC)
    return ferror(stream);
#else
    (void)stream;
    return 0;
#endif
}

static inline int zero_posix_fileno(FILE *stream) {
    if (!stream) return -1;
#if defined(ZERO_PLATFORM_WINDOWS)
    return _fileno(stream);
#elif !defined(ZERO_FREESTANDING) && !defined(ZERO_NO_POSIX) && !defined(ZERO_NOLIBC)
    return fileno(stream);
#else
    (void)stream;
    return -1;
#endif
}

static inline int64_t zero_posix_file_size(const char *path) {
    if (!path) return -1;
    struct stat st;
    if (zero_posix_stat(path, &st) == 0) {
        return (int64_t)st.st_size;
    }
    return -1;
}

static inline int64_t zero_posix_fd_size(int fd) {
    if (fd < 0) return -1;
    struct stat st;
    if (zero_posix_fstat(fd, &st) == 0) {
        return (int64_t)st.st_size;
    }
    return -1;
}

/* String Tokenization & Parsing Abstractions */
static inline char* zero_posix_strtok_r(char *str, const char *delim, char **saveptr) {
#if !defined(ZERO_FREESTANDING) && !defined(ZERO_NO_POSIX) && !defined(ZERO_NOLIBC)
#if defined(ZERO_PLATFORM_WINDOWS)
    return strtok_s(str, delim, saveptr);
#else
    return strtok_r(str, delim, saveptr);
#endif
#else
    if (!saveptr) return NULL;
    char *s = str ? str : *saveptr;
    if (!s) return NULL;
    while (*s && strchr(delim, *s)) s++;
    if (!*s) { *saveptr = NULL; return NULL; }
    char *tok = s;
    while (*s && !strchr(delim, *s)) s++;
    if (*s) { *s = '\0'; *saveptr = s + 1; }
    else { *saveptr = NULL; }
    return tok;
#endif
}

static inline long zero_posix_strtol(const char *nptr, char **endptr, int base) {
    if (!nptr) return 0;
#if !defined(ZERO_FREESTANDING) && !defined(ZERO_NO_POSIX) && !defined(ZERO_NOLIBC)
    return strtol(nptr, endptr, base);
#else
    (void)nptr; (void)endptr; (void)base;
    return 0;
#endif
}

static inline unsigned long zero_posix_strtoul(const char *nptr, char **endptr, int base) {
    if (!nptr) return 0;
#if !defined(ZERO_FREESTANDING) && !defined(ZERO_NO_POSIX) && !defined(ZERO_NOLIBC)
    return strtoul(nptr, endptr, base);
#else
    (void)nptr; (void)endptr; (void)base;
    return 0;
#endif
}

/* Sorting & Searching Abstractions */
static inline void zero_posix_qsort(void *base, size_t nmemb, size_t size, int (*compar)(const void *, const void *)) {
    if (!base || nmemb == 0 || size == 0 || !compar) return;
#if !defined(ZERO_FREESTANDING) && !defined(ZERO_NO_POSIX) && !defined(ZERO_NOLIBC)
    qsort(base, nmemb, size, compar);
#else
    (void)base; (void)nmemb; (void)size; (void)compar;
#endif
}

static inline void* zero_posix_bsearch(const void *key, const void *base, size_t nmemb, size_t size, int (*compar)(const void *, const void *)) {
    if (!key || !base || nmemb == 0 || size == 0 || !compar) return NULL;
#if !defined(ZERO_FREESTANDING) && !defined(ZERO_NO_POSIX) && !defined(ZERO_NOLIBC)
    return bsearch(key, base, nmemb, size, compar);
#else
    (void)key; (void)base; (void)nmemb; (void)size; (void)compar;
    return NULL;
#endif
}

/* Program Abort & Core Memory / String Primitive Wrappers */
static inline void zero_posix_abort(void) {
#if !defined(ZERO_FREESTANDING) && !defined(ZERO_NO_POSIX) && !defined(ZERO_NOLIBC)
    abort();
#elif defined(ZERO_NOLIBC)
    zero_nolibc_exit(137);
#else
    for (;;) {}
#endif
}

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


#endif /* ZERO_POSIX_H */
