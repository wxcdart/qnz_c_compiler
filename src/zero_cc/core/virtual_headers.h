#ifndef ZERO_CC_CORE_VIRTUAL_HEADERS_H
#define ZERO_CC_CORE_VIRTUAL_HEADERS_H

#include <string.h>
#include <stdlib.h>

/* Freestanding C99 math.h (double + float families; the `long double`
 * family is intentionally omitted - ZeroCC models long double as double,
 * so calling libm's 16-byte long-double entry points would be an ABI
 * mismatch). Replaces the system glibc math.h in clean runs, whose
 * __MATH_PRECNAME/__MATH_TGAMAC macro machinery is unparseable. */
static inline const char *zc_builtin_math_h(void) {
    return
        "#ifndef _MATH_H\n#define _MATH_H 1\n"
        "#define M_PI 3.14159265358979323846\n"
        "#define M_E 2.7182818284590452354\n"
        "#define HUGE_VAL 1e309\n"
        "#define INFINITY (__builtin_inf())\n"
        "#define NAN (__builtin_nan(\"\"))\n"
        "double sin(double);\ndouble cos(double);\ndouble tan(double);\n"
        "double asin(double);\ndouble acos(double);\ndouble atan(double);\n"
        "double atan2(double, double);\n"
        "double pow(double, double);\n"
        "double exp(double);\ndouble exp2(double);\ndouble expm1(double);\n"
        "double log(double);\ndouble log10(double);\ndouble log2(double);\ndouble log1p(double);\n"
        "double floor(double);\ndouble ceil(double);\ndouble round(double);\n"
        "double nearbyint(double);\ndouble rint(double);\ndouble trunc(double);\n"
        "double fmod(double, double);\ndouble remainder(double, double);\n"
        "double fdim(double, double);\ndouble fmin(double, double);\ndouble fmax(double, double);\n"
        "double cbrt(double);\ndouble sqrt(double);\ndouble hypot(double, double);\n"
        "double copysign(double, double);\n"
        "double frexp(double, int *);\ndouble ldexp(double, int);\n"
        "double modf(double, double *);\n"
        "int isinf(double);\nint isnan(double);\n"
        "float sinf(float);\nfloat cosf(float);\nfloat tanf(float);\n"
        "float atan2f(float, float);\n"
        "float powf(float, float);\n"
        "float expf(float);\nfloat exp2f(float);\nfloat expm1f(float);\n"
        "float logf(float);\nfloat log10f(float);\nfloat log2f(float);\nfloat log1pf(float);\n"
        "float floorf(float);\nfloat ceilf(float);\nfloat roundf(float);\n"
        "float nearbyintf(float);\nfloat rintf(float);\nfloat truncf(float);\n"
        "float fmodf(float, float);\nfloat remainderf(float, float);\n"
        "float fdimf(float, float);\nfloat fminf(float, float);\nfloat fmaxf(float, float);\n"
        "float cbrtf(float);\nfloat sqrtf(float);\nfloat hypotf(float, float);\n"
        "float copysignf(float, float);\n"
        "float frexpf(float, int *);\nfloat ldexpf(float, int);\n"
        "float modff(float, float *);\n"
        "int isinff(float);\nint isnanf(float);\n"
        "#endif\n";
}

static inline char* zero_cc_cpp_get_virtual_header(const char *name) {
    if (!name) return NULL;
    if (strcmp(name, "stddef.h") == 0) {
        return strdup(
            "#ifndef _STDDEF_H\n#define _STDDEF_H 1\n"
            "typedef unsigned long size_t;\n"
            "typedef long ptrdiff_t;\n"
            "typedef __WCHAR_TYPE__ wchar_t;\n"
            "#define NULL ((void*)0)\n"
            "#define offsetof(type, member) __builtin_offsetof(type, member)\n"
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
    if (strcmp(name, "inttypes.h") == 0) {
        /* Freestanding inttypes: the stdint types plus PRI* formatting
         * macros (the only addition over stdint.h). */
        return strdup(
            "#ifndef _INTTYPES_H\n#define _INTTYPES_H 1\n"
            "#include <stdint.h>\n"
            "#define PRId8 \"hd\"\n#define PRId16 \"hd\"\n#define PRId32 \"d\"\n#define PRId64 \"ld\"\n"
            "#define PRIu8 \"hu\"\n#define PRIu16 \"hu\"\n#define PRIu32 \"u\"\n#define PRIu64 \"lu\"\n"
            "#define PRIx8 \"hx\"\n#define PRIx16 \"hx\"\n#define PRIx32 \"x\"\n#define PRIx64 \"lx\"\n"
            "#define PRIX8 \"hX\"\n#define PRIX16 \"hX\"\n#define PRIX32 \"X\"\n#define PRIX64 \"lX\"\n"
            "#define SCNd8 \"hd\"\n#define SCNd16 \"hd\"\n#define SCNd32 \"d\"\n#define SCNd64 \"ld\"\n"
            "#define SCNu8 \"hu\"\n#define SCNu16 \"hu\"\n#define SCNu32 \"u\"\n#define SCNu64 \"lu\"\n"
            "#define SCNx8 \"hx\"\n#define SCNx16 \"hx\"\n#define SCNx32 \"x\"\n#define SCNx64 \"lx\"\n"
            "#endif\n"
        );
    }
    if (strcmp(name, "stdlib.h") == 0) {
        return strdup(
            "#ifndef _STDLIB_H\n#define _STDLIB_H 1\n"
            "typedef unsigned long size_t;\n"
            "void* malloc(size_t size);\nvoid free(void* ptr);\nvoid* calloc(size_t nmemb, size_t size);\n"
            "void* realloc(void* ptr, size_t size);\nvoid exit(int status);\nint atexit(void (*func)(void));\nint system(const char *command);\n"
            "long strtoll(const char *nptr, char **endptr, int base);\nlong strtol(const char *nptr, char **endptr, int base);\n"
            "unsigned long strtoul(const char *nptr, char **endptr, int base);\ndouble strtod(const char *nptr, char **endptr);\n"
            "unsigned long long strtoull(const char *nptr, char **endptr, int base);\nlong long atoll(const char *nptr);\n"
            "double atof(const char *nptr);\nvoid abort(void);\n"
            "char* getenv(const char *name);\n"
            "void* qsort(void *base, size_t nmemb, size_t size, int (*compar)(const void *, const void *));\n"
            "void* bsearch(const void *key, const void *base, size_t nmemb, size_t size, int (*compar)(const void *, const void *));\n"
            "int atoi(const char *nptr);\nint abs(int j);\n"
            "char* tmpnam(char *s);\n"
            "#endif\n"
        );
    }
    if (strcmp(name, "string.h") == 0) {
        return strdup(
            "#ifndef _STRING_H\n#define _STRING_H 1\n"
            "typedef unsigned long size_t;\n"
            "void* memcpy(void *dest, const void *src, size_t n);\n"
            "void* memmove(void *dest, const void *src, size_t n);\n"
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
    if (strcmp(name, "limits.h") == 0) {
        return strdup(
            "#ifndef _LIMITS_H\n#define _LIMITS_H 1\n"
            "#define PATH_MAX 4096\n"
            "#define NAME_MAX 255\n"
            "#define CHAR_BIT 8\n"
            "#define SCHAR_MIN (-128)\n#define SCHAR_MAX 127\n"
            "#define UCHAR_MAX 255\n"
            "#define CHAR_MIN (-128)\n#define CHAR_MAX 127\n"
            "#define SHRT_MIN (-32768)\n#define SHRT_MAX 32767\n"
            "#define USHRT_MAX 65535\n"
            "#define INT_MIN (-2147483647-1)\n#define INT_MAX 2147483647\n"
            "#define UINT_MAX 4294967295U\n"
            "#define LONG_MIN (-9223372036854775807L-1)\n#define LONG_MAX 9223372036854775807L\n"
            "#define ULONG_MAX 18446744073709551615UL\n"
            "#define LLONG_MIN (-9223372036854775807LL-1)\n#define LLONG_MAX 9223372036854775807LL\n"
            "#define ULLONG_MAX 18446744073709551615ULL\n"
            "#define SIZE_MAX ((size_t)-1)\n"
            "#define PTRDIFF_MIN (-9223372036854775807L-1)\n#define PTRDIFF_MAX 9223372036854775807L\n"
            "#define SIZE_T_MAX ((size_t)-1)\n"
            "#define INT8_MIN (-128)\n#define INT8_MAX 127\n"
            "#define UINT8_MAX 255\n"
            "#define INT16_MIN (-32768)\n#define INT16_MAX 32767\n"
            "#define UINT16_MAX 65535\n"
            "#define INT32_MIN (-2147483647-1)\n#define INT32_MAX 2147483647\n"
            "#define UINT32_MAX 4294967295U\n"
            "#define INT64_MIN (-9223372036854775807LL-1)\n#define INT64_MAX 9223372036854775807LL\n"
            "#define UINT64_MAX 18446744073709551615ULL\n"
            "#endif\n"
        );
    }
    if (strcmp(name, "assert.h") == 0) {
        /* Freestanding assert: evaluated in every build (no NDEBUG knob in
         * Phase 0) - a failed assertion calls abort(). */
        return strdup(
            "#ifndef _ASSERT_H\n#define _ASSERT_H 1\n"
            "#include <stdlib.h>\n"
            "#define assert(expr) ((expr) ? (void)0 : abort())\n"
            "#endif\n"
        );
    }
    if (strcmp(name, "errno.h") == 0) {
        return strdup(
            "#ifndef _ERRNO_H\n#define _ERRNO_H 1\n"
            "extern int errno;\n"
            "#define EPERM 1\n#define ENOENT 2\n#define EINTR 4\n#define EIO 5\n#define EBADF 9\n"
            "#define EAGAIN 11\n#define ENOMEM 12\n#define EACCES 13\n#define EEXIST 17\n"
            "#define EINVAL 22\n#define ENFILE 23\n#define EMFILE 24\n#define ENOSPC 28\n"
            "#define EPIPE 32\n#define EROFS 30\n#define ENOSYS 38\n#define ENOTDIR 20\n#define EISDIR 21\n"
            "#define ERANGE 34\n#define ENAMETOOLONG 36\n#define ETIMEDOUT 110\n#define ECONNREFUSED 111\n"
            "#endif\n"
        );
    }
    if (strcmp(name, "math.h") == 0) {
        return strdup(zc_builtin_math_h());
    }
    if (strcmp(name, "ctype.h") == 0) {
        return strdup(
            "#ifndef _CTYPE_H\n#define _CTYPE_H 1\n"
            "int isalnum(int c);\nint isalpha(int c);\nint isdigit(int c);\nint isxdigit(int c);\n"
            "int isspace(int c);\nint ispunct(int c);\nint isupper(int c);\nint islower(int c);\n"
            "int isprint(int c);\nint isgraph(int c);\nint iscntrl(int c);\n"
            "int tolower(int c);\nint toupper(int c);\n"
            "#endif\n"
        );
    }
    if (strcmp(name, "stdarg.h") == 0) {
        return strdup(
            "#ifndef _STDARG_H\n#define _STDARG_H 1\n"
            /* ABI-correct va_list (Sprint 49): the callee prologue spills all
             * argument registers into a 16-slot save area (6 integer regs,
             * then 8 FP regs) above the locals. va_start points the two
             * streams (gp/fp) just past the named parameters' slots; when a
             * stream is exhausted (ptr == end) reads continue from ovf, the
             * shared stack-overflow area (fp+16), in argument order. */
            "typedef struct { unsigned long long *gp; unsigned long long *gp_end; unsigned long long *fp; unsigned long long *fp_end; unsigned long long *ovf; } va_list;\n"
            "#define va_start(ap, last) __builtin_va_start(ap, last)\n"
            "#define va_arg(ap, type) __builtin_va_arg(ap, type)\n"
            /* Evaluate the argument (C99 7.15.1.1: va_end(ap) may evaluate
             * ap - tests like c-torture va-arg-21 rely on the side effect,
             * e.g. va_end(**ap_ptr++) advancing ap_ptr). */
            "#define va_end(ap) ((void)(ap))\n"
            "#define va_copy(dest, src) ((void)((dest) = (src)))\n"
            "#define __va_copy(dest, src) ((void)((dest) = (src)))\n"
            "#define __builtin_va_copy(dest, src) ((void)((dest) = (src)))\n"
            "#endif\n"
        );
    }
    if (strcmp(name, "setjmp.h") == 0) {
        return strdup(
            "#ifndef _SETJMP_H\n#define _SETJMP_H 1\n"
            /* jmp_buf: 5 x 8 bytes = 40 bytes, matching the zc_setjmp/
             * zc_longjmp runtime buffer layout (RSP, RBP, RA, RBX, R12).
             * An array so `setjmp(env)` decays to the buffer's address. The
             * macros map to the __builtin_setjmp/__builtin_longjmp builtins,
             * which are lowered to calls to the host runtime. */
            "typedef unsigned long long jmp_buf[5];\n"
            "#define setjmp(env) __builtin_setjmp(env)\n"
            "#define longjmp(env, val) __builtin_longjmp(env, val)\n"
            "#endif\n"
        );
    }
    if (strcmp(name, "time.h") == 0) {
        return strdup(
            "#ifndef _TIME_H\n#define _TIME_H 1\n"
            "typedef long time_t;\n"
            "struct tm { int tm_sec; int tm_min; int tm_hour; int tm_mday; int tm_mon; int tm_year; int tm_wday; int tm_yday; int tm_isdst; };\n"
            "time_t time(time_t *t);\n"
            "double difftime(time_t stop, time_t start);\n"
            "struct tm *gmtime(const time_t *t);\n"
            "struct tm *localtime(const time_t *t);\n"
            "char *ctime(const time_t *t);\n"
            "#endif\n"
        );
    }
    if (strcmp(name, "unistd.h") == 0) {
        return strdup(
            "#ifndef _UNISTD_H\n#define _UNISTD_H 1\n"
            "typedef unsigned long size_t;\n"
            "int write(int fd, const void *buf, size_t count);\n"
            "int read(int fd, void *buf, size_t count);\n"
            "int close(int fd);\n"
            "long getpid(void);\n"
            "int access(const char *path, int mode);\n"
            "int pipe(int pipefd[2]);\n"
            "int dup(int oldfd);\n"
            "int dup2(int oldfd, int newfd);\n"
            "void *sbrk(int incr);\n"
            "#define F_OK 0\n#define R_OK 4\n#define W_OK 2\n#define X_OK 1\n"
            "#define STDIN_FILENO 0\n#define STDOUT_FILENO 1\n#define STDERR_FILENO 2\n"
            "#endif\n"
        );
    }
    if (strcmp(name, "fcntl.h") == 0) {
        return strdup(
            "#ifndef _FCNTL_H\n#define _FCNTL_H 1\n"
            "typedef unsigned long size_t;\n"
            "int open(const char *path, int flags, ...);\n"
            "int creat(const char *path, int mode);\n"
            "#define O_RDONLY 0\n#define O_WRONLY 1\n#define O_RDWR 2\n"
            "#define O_CREAT 64\n#define O_EXCL 128\n#define O_TRUNC 512\n#define O_APPEND 1024\n"
            "#define O_NONBLOCK 2048\n"
            "#endif\n"
        );
    }
    if (strcmp(name, "sys/types.h") == 0) {
        return strdup(
            "#ifndef _SYS_TYPES_H\n#define _SYS_TYPES_H 1\n"
            "typedef unsigned long size_t;\n"
            "typedef long ssize_t;\n"
            "typedef unsigned int uid_t;\n"
            "typedef unsigned int gid_t;\n"
            "typedef long off_t;\n"
            "typedef unsigned int mode_t;\n"
            "typedef long pid_t;\n"
            "#endif\n"
        );
    }
    if (strcmp(name, "sys/stat.h") == 0) {
        return strdup(
            "#ifndef _SYS_STAT_H\n#define _SYS_STAT_H 1\n"
            "typedef unsigned long size_t;\n"
            "struct stat { unsigned long st_mode; unsigned long st_size; long st_mtime; };\n"
            "int stat(const char *path, struct stat *buf);\n"
            "int lstat(const char *path, struct stat *buf);\n"
            "int mkdir(const char *path, int mode);\n"
            "int chmod(const char *path, int mode);\n"
            "#define S_ISREG(m) (((m) & 61440) == 32768)\n"
            "#define S_ISDIR(m) (((m) & 61440) == 16384)\n"
            "#define S_IRWXU 493\n#define S_IRUSR 256\n#define S_IWUSR 128\n#define S_IXUSR 64\n"
            "#define S_IRWXG 56\n#define S_IRGRP 32\n#define S_IWGRP 16\n#define S_IXGRP 8\n"
            "#define S_IRWXO 7\n#define S_IROTH 4\n#define S_IWOTH 2\n#define S_IXOTH 1\n"
            "#endif\n"
        );
    }
    if (strcmp(name, "sys/mman.h") == 0) {
        return strdup(
            "#ifndef _SYS_MMAN_H\n#define _SYS_MMAN_H 1\n"
            "typedef unsigned long size_t;\n"
            "#define PROT_NONE 0\n#define PROT_READ 1\n#define PROT_WRITE 2\n#define PROT_EXEC 4\n"
            "#define MAP_PRIVATE 2\n#define MAP_SHARED 1\n#define MAP_ANONYMOUS 32\n#define MAP_ANON 32\n"
            "#define MAP_FAILED ((void*)-1)\n"
            "void *mmap(void *addr, size_t length, int prot, int flags, int fd, long offset);\n"
            "int munmap(void *addr, size_t length);\n"
            "int mprotect(void *addr, size_t len, int prot);\n"
            "#endif\n"
        );
    }
    if (strcmp(name, "dlfcn.h") == 0) {
        return strdup(
            "#ifndef _DLFCN_H\n#define _DLFCN_H 1\n"
            "#define RTLD_LAZY 1\n#define RTLD_NOW 2\n#define RTLD_DEFAULT ((void*)0)\n"
            "void* dlopen(const char *filename, int flags);\n"
            "void* dlsym(void *handle, const char *symbol);\n"
            "int dlclose(void *handle);\n"
            "char* dlerror(void);\n"
            "#endif\n"
        );
    }
    if (strcmp(name, "stdio.h") == 0) {
        return strdup(
            "#ifndef _STDIO_H\n#define _STDIO_H 1\n"
            "#include <stdarg.h>\n"
            "typedef unsigned long size_t;\n"
            "typedef struct FILE FILE;\n"
            "extern FILE *stdin, *stdout, *stderr;\n"
            "#define SEEK_SET 0\n#define SEEK_CUR 1\n#define SEEK_END 2\n#define EOF (-1)\n"
            "int printf(const char *format, ...);\n"
            "int fprintf(FILE *stream, const char *format, ...);\n"
            "int sprintf(char *str, const char *format, ...);\n"
            "int snprintf(char *str, size_t size, const char *format, ...);\n"
            "int puts(const char *s);\nint putchar(int c);\n"
            "void perror(const char *s);\n"
            "int vprintf(const char *format, va_list ap);\n"
            "int vfprintf(FILE *stream, const char *format, va_list ap);\n"
            "int vscanf(const char *format, va_list ap);\n"
            "int vfscanf(FILE *stream, const char *format, va_list ap);\n"
            "int scanf(const char *format, ...);\n"
            "int fscanf(FILE *stream, const char *format, ...);\n"
            "int sscanf(const char *s, const char *format, ...);\n"
            "int remove(const char *filename);\nvoid rewind(FILE *stream);\n"
            "int fputs(const char *s, FILE *stream);\nint fgetc(FILE *stream);\n"
            "int getchar(void);\n"
            "FILE* fopen(const char *pathname, const char *mode);\n"
            "int fclose(FILE *stream);\n"
            "size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream);\n"
            "size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream);\n"
            "int fseek(FILE *stream, long offset, int whence);\n"
            "long ftell(FILE *stream);\nint fflush(FILE *stream);\n"
            "#endif\n"
        );
    }
    if (strcmp(name, "math.h") == 0) {
        return strdup(zc_builtin_math_h());
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
            "#define DBL_MAX 1.7976931348623157081452742373170435679807e+308\n#define DBL_MIN 2.22507385850720138309e-308\n#define DBL_EPSILON 2.22044604925031308085e-16\n"
            "#define FLT_MAX 3.40282346638528859812e+38F\n#define FLT_MIN 1.17549435082228750797e-38F\n#define FLT_EPSILON 1.19209289550781250000e-7F\n"
            "#define LDBL_MAX 1.7976931348623157081452742373170435679807e+308L\n#define LDBL_MIN 2.22507385850720138309e-308L\n#define LDBL_EPSILON 2.22044604925031308085e-16L\n"
            "#define LDBL_MAX_EXP 1024\n#define LDBL_MIN_EXP (-1021)\n#define LDBL_MANT_DIG 53\n#define LDBL_DIG 15\n"
            "#endif\n"
        );
    }
    if (strcmp(name, "stdnoreturn.h") == 0) {
        /* C11 7.2: the `noreturn` keyword. zerocc lexes `_Noreturn` natively
         * (TK_NORETURN), so map the standard spelling onto it. */
        return strdup(
            "#ifndef _STDNORETURN_H\n#define _STDNORETURN_H 1\n"
            "#define noreturn _Noreturn\n"
            "#endif\n"
        );
    }
    if (strcmp(name, "stdatomic.h") == 0) {
        /* C11 7.17 atomics. The embedded text mirrors include/stdatomic.h
         * (kept in sync manually); the atomic_* macros lower through
         * zerocc's __atomic_* builtin family. */
        return strdup(
            "#ifndef _ZERO_CC_STDATOMIC_H\n#define _ZERO_CC_STDATOMIC_H\n"
            "\n"
            "#include <stdbool.h>\n"
            "#include <stddef.h>\n"
            "#include <stdint.h>\n"
            "\n"
            "/* ---- C11 7.17.1: atomic integer types ---- */\n"
            "typedef _Atomic bool           atomic_bool;\n"
            "typedef _Atomic char           atomic_char;\n"
            "typedef _Atomic signed char    atomic_schar;\n"
            "typedef _Atomic unsigned char  atomic_uchar;\n"
            "typedef _Atomic short          atomic_short;\n"
            "typedef _Atomic unsigned short atomic_ushort;\n"
            "typedef _Atomic int            atomic_int;\n"
            "typedef _Atomic unsigned int   atomic_uint;\n"
            "typedef _Atomic long           atomic_long;\n"
            "typedef _Atomic unsigned long  atomic_ulong;\n"
            "typedef _Atomic long long      atomic_llong;\n"
            "typedef _Atomic unsigned long long atomic_ullong;\n"
            "typedef _Atomic int8_t         atomic_int_least8_t;\n"
            "typedef _Atomic uint8_t        atomic_uint_least8_t;\n"
            "typedef _Atomic int16_t        atomic_int_least16_t;\n"
            "typedef _Atomic uint16_t       atomic_uint_least16_t;\n"
            "typedef _Atomic int32_t        atomic_int_least32_t;\n"
            "typedef _Atomic uint32_t       atomic_uint_least32_t;\n"
            "typedef _Atomic int64_t        atomic_int_least64_t;\n"
            "typedef _Atomic uint64_t       atomic_uint_least64_t;\n"
            "typedef _Atomic int_fast8_t    atomic_int_fast8_t;\n"
            "typedef _Atomic uint_fast8_t   atomic_uint_fast8_t;\n"
            "typedef _Atomic int_fast16_t   atomic_int_fast16_t;\n"
            "typedef _Atomic uint_fast16_t  atomic_uint_fast16_t;\n"
            "typedef _Atomic int_fast32_t   atomic_int_fast32_t;\n"
            "typedef _Atomic uint_fast32_t  atomic_uint_fast32_t;\n"
            "typedef _Atomic int_fast64_t   atomic_int_fast64_t;\n"
            "typedef _Atomic uint_fast64_t  atomic_uint_fast64_t;\n"
            "typedef _Atomic intptr_t       atomic_intptr_t;\n"
            "typedef _Atomic uintptr_t      atomic_uintptr_t;\n"
            "typedef _Atomic size_t         atomic_size_t;\n"
            "typedef _Atomic ptrdiff_t      atomic_ptrdiff_t;\n"
            "\n"
            "/* C11 7.17.6: atomic flags */\n"
            "typedef _Atomic int atomic_flag;\n"
            "#define ATOMIC_FLAG_INIT 0\n"
            "\n"
            "/* ---- C11 7.17.1: initializers ---- */\n"
            "#define ATOMIC_VAR_INIT(val) (val)\n"
            "#define ATOMIC_INIT(val)     ATOMIC_VAR_INIT(val) /* C11: obsolescent */\n"
            "\n"
            "#define atomic_init(obj, desired)          atomic_store(obj, desired)\n"
            "\n"
            "/* ---- C11 7.17.2: lock-free query ---- */\n"
            "#define atomic_is_lock_free(obj)           __atomic_is_lock_free(sizeof *(obj), 0)\n"
            "\n"
            "/* GCC compatibility (2 = lock-free, 1 = available, 0 = unavailable) */\n"
            "#define __GCC_ATOMIC_BOOL_LOCK_FREE        2\n"
            "#define __GCC_ATOMIC_CHAR_LOCK_FREE        2\n"
            "#define __GCC_ATOMIC_CHAR16_T_LOCK_FREE    2\n"
            "#define __GCC_ATOMIC_CHAR32_T_LOCK_FREE    2\n"
            "#define __GCC_ATOMIC_WCHAR_T_LOCK_FREE     2\n"
            "#define __GCC_ATOMIC_SHORT_LOCK_FREE       2\n"
            "#define __GCC_ATOMIC_INT_LOCK_FREE         2\n"
            "#define __GCC_ATOMIC_LONG_LOCK_FREE        2\n"
            "#define __GCC_ATOMIC_LLONG_LOCK_FREE       2\n"
            "#define __GCC_ATOMIC_POINTER_LOCK_FREE     2\n"
            "#define __GCC_ATOMIC_TEST_AND_SET_TRUEVAL  1\n"
            "\n"
            "/* ---- C11 7.17.3: atomic operations ---- */\n"
            "#define atomic_load(obj)                   __atomic_load_n(obj, __ATOMIC_SEQ_CST)\n"
            "#define atomic_load_explicit(obj, order)   __atomic_load_n(obj, order)\n"
            "#define atomic_store(obj, desired)         __atomic_store_n(obj, desired, __ATOMIC_SEQ_CST)\n"
            "#define atomic_store_explicit(obj, desired, order) __atomic_store_n(obj, desired, order)\n"
            "#define atomic_exchange(obj, desired)      __atomic_exchange_n(obj, desired, __ATOMIC_SEQ_CST)\n"
            "#define atomic_exchange_explicit(obj, desired, order) __atomic_exchange_n(obj, desired, order)\n"
            "\n"
            "#define atomic_compare_exchange_strong(obj, expected, desired) __atomic_compare_exchange_n(obj, expected, desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_ACQUIRE)\n"
            "#define atomic_compare_exchange_weak(obj, expected, desired) atomic_compare_exchange_strong(obj, expected, desired)\n"
            "#define atomic_compare_exchange_strong_explicit(obj, expected, desired, success, failure) __atomic_compare_exchange_n(obj, expected, desired, 0, success, failure)\n"
            "#define atomic_compare_exchange_weak_explicit(obj, expected, desired, success, failure) atomic_compare_exchange_strong_explicit(obj, expected, desired, success, failure)\n"
            "\n"
            "#define atomic_fetch_add(obj, arg)         __atomic_fetch_add(obj, arg, __ATOMIC_SEQ_CST)\n"
            "#define atomic_fetch_add_explicit(obj, arg, order) __atomic_fetch_add(obj, arg, order)\n"
            "#define atomic_fetch_sub(obj, arg)         __atomic_fetch_sub(obj, arg, __ATOMIC_SEQ_CST)\n"
            "#define atomic_fetch_sub_explicit(obj, arg, order) __atomic_fetch_sub(obj, arg, order)\n"
            "#define atomic_fetch_or(obj, arg)          __atomic_fetch_or(obj, arg, __ATOMIC_SEQ_CST)\n"
            "#define atomic_fetch_or_explicit(obj, arg, order) __atomic_fetch_or(obj, arg, order)\n"
            "#define atomic_fetch_and(obj, arg)         __atomic_fetch_and(obj, arg, __ATOMIC_SEQ_CST)\n"
            "#define atomic_fetch_and_explicit(obj, arg, order) __atomic_fetch_and(obj, arg, order)\n"
            "#define atomic_fetch_xor(obj, arg)         __atomic_fetch_xor(obj, arg, __ATOMIC_SEQ_CST)\n"
            "#define atomic_fetch_xor_explicit(obj, arg, order) __atomic_fetch_xor(obj, arg, order)\n"
            "\n"
            "#define atomic_add_fetch(obj, arg)         __atomic_add_fetch(obj, arg, __ATOMIC_SEQ_CST)\n"
            "#define atomic_add_fetch_explicit(obj, arg, order) __atomic_add_fetch(obj, arg, order)\n"
            "#define atomic_sub_fetch(obj, arg)         __atomic_sub_fetch(obj, arg, __ATOMIC_SEQ_CST)\n"
            "#define atomic_sub_fetch_explicit(obj, arg, order) __atomic_sub_fetch(obj, arg, order)\n"
            "#define atomic_or_fetch(obj, arg)          __atomic_or_fetch(obj, arg, __ATOMIC_SEQ_CST)\n"
            "#define atomic_or_fetch_explicit(obj, arg, order) __atomic_or_fetch(obj, arg, order)\n"
            "#define atomic_and_fetch(obj, arg)         __atomic_and_fetch(obj, arg, __ATOMIC_SEQ_CST)\n"
            "#define atomic_and_fetch_explicit(obj, arg, order) __atomic_and_fetch(obj, arg, order)\n"
            "#define atomic_xor_fetch(obj, arg)         __atomic_xor_fetch(obj, arg, __ATOMIC_SEQ_CST)\n"
            "#define atomic_xor_fetch_explicit(obj, arg, order) __atomic_xor_fetch(obj, arg, order)\n"
            "\n"
            "/* ---- C11 7.17.6: atomic flags ---- */\n"
            "#define atomic_flag_test_and_set(obj)      __atomic_test_and_set(obj)\n"
            "#define atomic_flag_clear(obj)             __atomic_clear(obj)\n"
            "\n"
            "#endif /* _ZERO_CC_STDATOMIC_H */\n"
        );
    }
    if (strcmp(name, "threads.h") == 0) {
        /* C11 7.26 threads (freestanding subset): thrd_t/mtx_t aliases over
         * the pthread surface + the thrd_ and mtx_ macros. Mirrors
         * include/threads.h (kept in sync manually). */
        return strdup(
            "#ifndef _ZERO_CC_THREADS_H\n#define _ZERO_CC_THREADS_H\n"
            "\n"
            "typedef unsigned long thrd_t;\n"
            "typedef union {\n"
            "    char __size[40];\n"
            "    long __align;\n"
            "} mtx_t;\n"
            "\n"
            "enum {\n"
            "    thrd_success  = 0,\n"
            "    thrd_error    = 1,\n"
            "    thrd_busy     = 2,\n"
            "    thrd_nomem    = 3,\n"
            "    thrd_timedout = 4\n"
            "};\n"
            "\n"
            "enum {\n"
            "    mtx_plain     = 0,\n"
            "    mtx_recursive = 1,\n"
            "    mtx_timed     = 2\n"
            "};\n"
            "\n"
            "int pthread_create(thrd_t *thread, void *attr, void *start_routine, void *arg);\n"
            "int pthread_join(thrd_t thread, void **retval);\n"
            "int pthread_mutex_init(mtx_t *mutex, void *attr);\n"
            "int pthread_mutex_lock(mtx_t *mutex);\n"
            "int pthread_mutex_unlock(mtx_t *mutex);\n"
            "int pthread_mutex_destroy(mtx_t *mutex);\n"
            "\n"
            "#define thrd_create(t, fn, arg) (pthread_create((t), (void *)0, (void *)(fn), (void *)(arg)) == 0 ? thrd_success : thrd_error)\n"
            "#define thrd_join(t, res)       (pthread_join((t), (void **)(res)) == 0 ? thrd_success : thrd_error)\n"
            "#define mtx_init(m, type)       (pthread_mutex_init((m), (void *)0) == 0 ? thrd_success : thrd_error)\n"
            "#define mtx_lock(m)             (pthread_mutex_lock(m) == 0 ? thrd_success : thrd_error)\n"
            "#define mtx_unlock(m)           (pthread_mutex_unlock(m) == 0 ? thrd_success : thrd_error)\n"
            "#define mtx_destroy(m)          pthread_mutex_destroy(m)\n"
            "\n"
            "#endif /* _ZERO_CC_THREADS_H */\n"
        );
    }
    return NULL;
}

#endif /* ZERO_CC_CORE_VIRTUAL_HEADERS_H */
