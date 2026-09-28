#ifndef ZERO_CC_POSIX_FILEIO_H
#define ZERO_CC_POSIX_FILEIO_H

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


#endif /* ZERO_CC_POSIX_FILEIO_H */
