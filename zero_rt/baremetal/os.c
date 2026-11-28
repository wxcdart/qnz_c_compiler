/* zero_rt/baremetal/os.c - Baremetal OS boundary layer.
 *
 * Implements the zeroc_rt_* OS interface in a freestanding environment
 * without operating system kernel syscalls:
 * - zeroc_rt_write: directed to UART driver and in-memory trace buffer
 * - zeroc_rt_brk: bump-pointer heap allocator over RAM
 * - zeroc_rt_exit / zeroc_rt_exit_group: recording status and CPU halt
 * - Threading / futex stubs: safe single-core no-ops
 */
#include "rt_os.h"
#include "baremetal/uart.h"

extern unsigned char _ebss;
extern unsigned char _stack_top;

volatile int g_baremetal_exit_code = 0;
volatile int g_baremetal_has_exited = 0;

static uintptr_t s_bm_brk_cur = 0;

int zeroc_rt_write(int fd, const void *buf, uint64_t count) {
    (void)fd;
    const char *s = (const char *)buf;
    for (uint64_t i = 0; i < count; i++) {
        uart_putc(s[i]);
    }
    return (int)count;
}

int zeroc_rt_read(int fd, void *buf, uint64_t count) {
    (void)fd;
    char *p = (char *)buf;
    uint64_t n = 0;
    while (n < count) {
        int ch = uart_getc();
        if (ch < 0) break;
        p[n++] = (char)ch;
    }
    return (int)n;
}

uintptr_t zeroc_rt_brk(uintptr_t addr) {
    uintptr_t heap_start = ((uintptr_t)&_ebss + 15) & ~15UL;
    /* Reserve 4KB for stack below _stack_top */
    uintptr_t stack_guard = (uintptr_t)&_stack_top > 4096 ? ((uintptr_t)&_stack_top - 4096) : 0;

    if (s_bm_brk_cur == 0) {
        s_bm_brk_cur = heap_start;
    }
    if (addr == 0) {
        return s_bm_brk_cur;
    }
    if (addr < heap_start || (stack_guard && addr > stack_guard)) {
        return s_bm_brk_cur; /* out of bounds, return current break */
    }
    s_bm_brk_cur = addr;
    return s_bm_brk_cur;
}

int zeroc_rt_getpid(void) {
    return 1;
}

void zeroc_rt_exit_group(int code) {
    g_baremetal_exit_code = code;
    g_baremetal_has_exited = 1;

    for (;;) { }
}

void zeroc_rt_exit(int status) {
    zeroc_rt_exit_group(status);
}

int zeroc_rt_arch_prctl(int code, unsigned long addr) {
    (void)code; (void)addr;
    return -1;
}

long zeroc_rt_clone(int (*fn)(void *), void *child_stack, unsigned long flags,
                    void *arg, int *ptid, void *tls, int *ctid) {
    (void)fn; (void)child_stack; (void)flags; (void)arg;
    (void)ptid; (void)tls; (void)ctid;
    return -1;
}

int zeroc_rt_futex_wait(int *uaddr, int val, const void *timeout) {
    (void)uaddr; (void)val; (void)timeout;
    return 0;
}

int zeroc_rt_futex_wake(int *uaddr, int count) {
    (void)uaddr; (void)count;
    return 0;
}

void *zeroc_rt_mmap(void *addr, size_t length, int prot, int flags, int fd, long offset) {
    (void)addr; (void)length; (void)prot; (void)flags; (void)fd; (void)offset;
    return (void *)-1;
}

int zeroc_rt_munmap(void *addr, uint64_t length) {
    (void)addr; (void)length;
    return -1;
}

int zeroc_rt_mprotect(void *addr, uint64_t length, int prot) {
    (void)addr; (void)length; (void)prot;
    return 0;
}

int zeroc_rt_sched_yield(void) {
    return 0;
}

int zeroc_rt_gettid(void) {
    return 1;
}

int zeroc_rt_nanosleep(const void *req, void *rem) {
    (void)req; (void)rem;
    return 0;
}
