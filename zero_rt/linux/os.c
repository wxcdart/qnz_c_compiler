/* zero_rt Linux x86-64 OS boundary: raw syscalls via __zerocc_syscall.
 *
 * SysV syscall argument registers (eax, rdi, rsi, rdx, r10, r8, r9) are
 * handled by the compiler backend; this file is pure C.
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

/* brk(0) returns the current program break; brk(addr) sets it. Failures
 * come back as a negative errno in rax, which wraps to a huge unsigned -
 * callers compare against the requested value. */
uint64_t zeroc_rt_brk(uint64_t addr) {
    return (uint64_t)__zerocc_syscall(RT_SYS_BRK, (long)addr, 0, 0, 0, 0, 0);
}

int zeroc_rt_getpid(void) {
    return (int)__zerocc_syscall(RT_SYS_GETPID, 0, 0, 0, 0, 0, 0);
}

void zeroc_rt_exit_group(int code) {
    __zerocc_syscall(RT_SYS_EXIT_GROUP, (long)code, 0, 0, 0, 0, 0);
    for (;;) { } /* unreachable */
}
