#ifndef ZERO_CC_POSIX_ERROR_H
#define ZERO_CC_POSIX_ERROR_H

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


#endif /* ZERO_CC_POSIX_ERROR_H */
