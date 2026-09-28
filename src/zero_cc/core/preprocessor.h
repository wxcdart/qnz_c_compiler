#ifndef ZERO_CC_CORE_PREPROCESSOR_H
#define ZERO_CC_CORE_PREPROCESSOR_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>


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

#endif /* ZERO_CC_CORE_PREPROCESSOR_H */
