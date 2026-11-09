/* zero_rt OS boundary: raw syscalls via __zerocc_syscall.
 *
 * Syscall argument registers are handled by the compiler backend
 * (x86-64: eax/rdi/rsi/rdx/r10/r8/r9, aarch64: x8/x0-x5, RISC-V:
 * a7/a0-a5); this file is pure C.
 */
#include "rt_os.h"

int zeroc_rt_read(int fd, void *buf, uint64_t count) {
    return (int)__zerocc_syscall(RT_SYS_READ, (long)fd, (long)buf,
                                 (long)count, 0, 0, 0);
}

int zeroc_rt_write(int fd, const void *buf, uint64_t count) {
    return (int)__zerocc_syscall(RT_SYS_WRITE, (long)fd, (long)buf,
                                 (long)count, 0, 0, 0);
}

#if RT_HAS_BRK

/* The raw brk syscall returns the OLD break on success and -ENOMEM
 * (wrapped to a huge unsigned) on failure. The heap's contract is
 * "brk(end) returns end on success, the current break on failure", so
 * translate: query first, then compare the return against the query. */
uintptr_t zeroc_rt_brk(uintptr_t addr) {
    long cur = __zerocc_syscall(RT_SYS_BRK, 0, 0, 0, 0, 0, 0);
    if (addr == 0) return (uintptr_t)cur;
    long ret = __zerocc_syscall(RT_SYS_BRK, (long)addr, 0, 0, 0, 0, 0);
    if (ret < 0 && ret >= -4095) return (uintptr_t)cur;  /* -ENOMEM: failure */
    return addr;                          /* success: the requested break */
}

#else

/* aarch64 has no brk syscall: emulate the program break as a grow-only
 * cursor over a pre-mapped anonymous arena (64 MiB covers the test and
 * demo workloads; the heap's own 2^48 guard caps it long before). */
#define RT_ARENA_SIZE (64ULL * 1024 * 1024)

static uintptr_t arena_base = 0;
static uintptr_t arena_cur = 0;

uintptr_t zeroc_rt_brk(uintptr_t addr) {
    if (arena_base == 0) {
        long p = __zerocc_syscall(RT_SYS_MMAP, 0, RT_ARENA_SIZE,
                                  RT_PROT_READ | RT_PROT_WRITE,
                                  RT_MAP_PRIVATE | RT_MAP_ANONYMOUS,
                                  (long)-1, 0);
        if (p < 0 && p >= -4095) return 0;   /* mmap error */
        arena_base = (uintptr_t)p;
        arena_cur = (uintptr_t)p;
    }
    if (addr == 0) return arena_cur;
    if (addr < arena_cur || addr > arena_base + RT_ARENA_SIZE) return arena_cur;
    arena_cur = addr;
    return arena_cur;
}

#endif

int zeroc_rt_getpid(void) {
    return (int)__zerocc_syscall(RT_SYS_GETPID, 0, 0, 0, 0, 0, 0);
}

void zeroc_rt_exit_group(int code) {
    __zerocc_syscall(RT_SYS_EXIT_GROUP, (long)code, 0, 0, 0, 0, 0);
    for (;;) { } /* unreachable */
}

long zeroc_rt_clone(int (*fn)(void *), void *child_stack, unsigned long flags,
                    void *arg, int *ptid, void *tls, int *ctid) {
    /* Align stack to 16 bytes */
    uintptr_t sp = ((uintptr_t)child_stack) & ~15UL;
#if defined(__riscv) && __riscv_xlen == 32
    uint32_t *p = (uint32_t *)sp;
    *--p = 0; /* padding to maintain 16-byte alignment */
    *--p = 0;
    *--p = (uint32_t)(uintptr_t)arg;
    *--p = (uint32_t)(uintptr_t)fn;
    sp = (uintptr_t)p;
#else
    uint64_t *p = (uint64_t *)sp;
    *--p = (uint64_t)(uintptr_t)arg;
    *--p = (uint64_t)(uintptr_t)fn;
    sp = (uintptr_t)p;
#endif
    return __zerocc_clone(flags, (void *)sp, ptid, tls, ctid);
}

int zeroc_rt_futex_wait(int *uaddr, int val, const void *timeout) {
    long ret = __zerocc_syscall(RT_SYS_FUTEX, (long)uaddr, (long)RT_FUTEX_WAIT,
                                (long)val, (long)timeout, 0, 0);
#ifdef RT_SYS_FUTEX_TIME64
    if (ret == -38 /* -ENOSYS */) {
        ret = __zerocc_syscall(RT_SYS_FUTEX_TIME64, (long)uaddr, (long)RT_FUTEX_WAIT,
                               (long)val, (long)timeout, 0, 0);
    }
#endif
    return (int)ret;
}

int zeroc_rt_futex_wake(int *uaddr, int count) {
    long ret = __zerocc_syscall(RT_SYS_FUTEX, (long)uaddr, (long)RT_FUTEX_WAKE,
                                (long)count, 0, 0, 0);
#ifdef RT_SYS_FUTEX_TIME64
    if (ret == -38 /* -ENOSYS */) {
        ret = __zerocc_syscall(RT_SYS_FUTEX_TIME64, (long)uaddr, (long)RT_FUTEX_WAKE,
                               (long)count, 0, 0, 0);
    }
#endif
    return (int)ret;
}

void *zeroc_rt_mmap(void *addr, uint64_t length, int prot, int flags, int fd, int64_t offset) {
    long ret = __zerocc_syscall(RT_SYS_MMAP, (long)addr, (long)length,
                                (long)prot, (long)flags, (long)fd, (long)offset);
    if (ret < 0 && ret >= -4095) {
        return RT_MAP_FAILED;
    }
    return (void *)ret;
}

int zeroc_rt_munmap(void *addr, uint64_t length) {
    return (int)__zerocc_syscall(RT_SYS_MUNMAP, (long)addr, (long)length, 0, 0, 0, 0);
}

int zeroc_rt_mprotect(void *addr, uint64_t length, int prot) {
    return (int)__zerocc_syscall(RT_SYS_MPROTECT, (long)addr, (long)length, (long)prot, 0, 0, 0);
}

int zeroc_rt_sched_yield(void) {
    return (int)__zerocc_syscall(RT_SYS_SCHED_YIELD, 0, 0, 0, 0, 0, 0);
}

int zeroc_rt_gettid(void) {
    return (int)__zerocc_syscall(RT_SYS_GETTID, 0, 0, 0, 0, 0, 0);
}

void zeroc_rt_exit(int status) {
    __zerocc_syscall(RT_SYS_EXIT, (long)status, 0, 0, 0, 0, 0);
    for (;;) { } /* unreachable */
}

int zeroc_rt_nanosleep(const void *req, void *rem) {
    return (int)__zerocc_syscall(RT_SYS_NANOSLEEP, (long)req, (long)rem, 0, 0, 0, 0);
}
