/* zero_rt internal OS-boundary layer.
 *
 * Every OS operation goes through the __zerocc_syscall builtin (the only
 * machine-code surface of the runtime; everything above this layer is
 * plain C compiled by ZeroCC itself).
 *
 * Per-ISA syscall tables: the Makefile defines RT_TARGET_AARCH64 or
 * RT_TARGET_RISCV when cross-building zero_rt.a for those targets; the
 * default (no define) is the x86-64 table.
 */
#ifndef ZERO_RT_OS_H
#define ZERO_RT_OS_H

#include <stdint.h>

#ifndef RT_TARGET_AARCH64
#if defined(__aarch64__)
#define RT_TARGET_AARCH64 1
#else
#define RT_TARGET_AARCH64 0
#endif
#endif
#ifndef RT_TARGET_RISCV
#if defined(__riscv)
#define RT_TARGET_RISCV 1
#else
#define RT_TARGET_RISCV 0
#endif
#endif

#if RT_TARGET_AARCH64
/* Linux aarch64 syscall numbers. */
#define RT_SYS_READ        63
#define RT_SYS_WRITE       64
#define RT_SYS_EXIT        93
#define RT_SYS_EXIT_GROUP  94
#define RT_SYS_FUTEX       98
#define RT_SYS_NANOSLEEP   101
#define RT_SYS_SCHED_YIELD 124
#define RT_SYS_GETPID      172
#define RT_SYS_GETTID      178
#define RT_SYS_BRK         214
#define RT_SYS_MUNMAP      215
#define RT_SYS_CLONE       220
#define RT_SYS_MMAP        222
#define RT_SYS_MPROTECT    226
#define RT_HAS_BRK         1
#elif RT_TARGET_RISCV
/* Linux RISC-V (rv32 and rv64 share the syscall table). */
#define RT_SYS_READ        63
#define RT_SYS_WRITE       64
#define RT_SYS_EXIT        93
#define RT_SYS_EXIT_GROUP  94
#define RT_SYS_FUTEX       98
#define RT_SYS_NANOSLEEP   101
#define RT_SYS_SCHED_YIELD 124
#define RT_SYS_GETPID      172
#define RT_SYS_GETTID      178
#define RT_SYS_BRK         214
#define RT_SYS_MUNMAP      215
#define RT_SYS_CLONE       220
#define RT_SYS_MMAP        222
#define RT_SYS_MPROTECT    226
#define RT_SYS_FUTEX_TIME64 422
#define RT_HAS_BRK         1
#else
/* Linux x86-64 syscall numbers. */
#define RT_SYS_READ        0
#define RT_SYS_WRITE       1
#define RT_SYS_MMAP        9
#define RT_SYS_MPROTECT    10
#define RT_SYS_MUNMAP      11
#define RT_SYS_BRK         12
#define RT_SYS_SCHED_YIELD 24
#define RT_SYS_NANOSLEEP   35
#define RT_SYS_GETPID      39
#define RT_SYS_CLONE       56
#define RT_SYS_EXIT        60
#define RT_SYS_ARCH_PRCTL  158
#define RT_ARCH_SET_FS     0x1002
#define RT_SYS_GETTID      186
#define RT_SYS_FUTEX       202
#define RT_SYS_EXIT_GROUP  231
#define RT_HAS_BRK         1
#endif

/* mmap(2) and mprotect(2) constants */
#define RT_PROT_NONE      0
#define RT_PROT_READ      1
#define RT_PROT_WRITE     2
#define RT_PROT_EXEC      4
#define RT_MAP_SHARED     0x01
#define RT_MAP_PRIVATE    0x02
#define RT_MAP_ANONYMOUS  0x20
#define RT_MAP_FAILED     ((void *)-1)

/* Linux clone(2) flags */
#define RT_CLONE_VM             0x00000100UL
#define RT_CLONE_FS             0x00000200UL
#define RT_CLONE_FILES          0x00000400UL
#define RT_CLONE_SIGHAND        0x00000800UL
#define RT_CLONE_THREAD         0x00010000UL
#define RT_CLONE_SYSVSEM        0x00040000UL
#define RT_CLONE_SETTLS         0x00080000UL
#define RT_CLONE_PARENT_SETTID  0x00100000UL
#define RT_CLONE_CHILD_CLEARTID 0x00200000UL

/* Linux futex(2) operations */
#define RT_FUTEX_WAIT           0
#define RT_FUTEX_WAKE           1
#define RT_FUTEX_PRIVATE_FLAG   128
#define RT_FUTEX_WAIT_PRIVATE   (RT_FUTEX_WAIT | RT_FUTEX_PRIVATE_FLAG)
#define RT_FUTEX_WAKE_PRIVATE   (RT_FUTEX_WAKE | RT_FUTEX_PRIVATE_FLAG)

int       zeroc_rt_read(int fd, void *buf, uint64_t count);
int       zeroc_rt_write(int fd, const void *buf, uint64_t count);
uintptr_t zeroc_rt_brk(uintptr_t addr);      /* 0 = query current break */
int       zeroc_rt_getpid(void);
void      zeroc_rt_exit_group(int code);    /* does not return */
int       zeroc_rt_arch_prctl(int code, unsigned long addr);

/* Threading and memory management OS boundary */
long      zeroc_rt_clone(int (*fn)(void *), void *child_stack, unsigned long flags,
                         void *arg, int *ptid, void *tls, int *ctid);
int       zeroc_rt_futex_wait(int *uaddr, int val, const void *timeout);
int       zeroc_rt_futex_wake(int *uaddr, int count);
void     *zeroc_rt_mmap(void *addr, uint64_t length, int prot, int flags, int fd, int64_t offset);
int       zeroc_rt_munmap(void *addr, uint64_t length);
int       zeroc_rt_mprotect(void *addr, uint64_t length, int prot);
int       zeroc_rt_sched_yield(void);
int       zeroc_rt_gettid(void);
void      zeroc_rt_exit(int status);        /* does not return */
int       zeroc_rt_nanosleep(const void *req, void *rem);

#endif /* ZERO_RT_OS_H */
