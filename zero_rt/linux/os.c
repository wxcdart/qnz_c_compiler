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
uint64_t zeroc_rt_brk(uint64_t addr) {
    uint64_t cur = (uint64_t)__zerocc_syscall(RT_SYS_BRK, 0, 0, 0, 0, 0, 0);
    if (addr == 0) return cur;
    uint64_t ret = (uint64_t)__zerocc_syscall(RT_SYS_BRK, (long)addr, 0, 0, 0, 0, 0);
    if (ret > (1ULL << 48)) return cur;  /* -ENOMEM wrapped: failure */
    return addr;                          /* success: the requested break */
}

#else

/* aarch64 has no brk syscall: emulate the program break as a grow-only
 * cursor over a pre-mapped anonymous arena (64 MiB covers the test and
 * demo workloads; the heap's own 2^48 guard caps it long before). */
#define RT_ARENA_SIZE (64ULL * 1024 * 1024)

static uint64_t arena_base = 0;
static uint64_t arena_cur = 0;

uint64_t zeroc_rt_brk(uint64_t addr) {
    if (arena_base == 0) {
        uint64_t p = (uint64_t)__zerocc_syscall(RT_SYS_MMAP, 0, RT_ARENA_SIZE,
                                                RT_PROT_READ | RT_PROT_WRITE,
                                                RT_MAP_PRIVATE | RT_MAP_ANONYMOUS,
                                                (long)-1, 0);
        if (p > (1ULL << 48)) return 0;   /* mmap error (wrapped) */
        arena_base = p;
        arena_cur = p;
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
