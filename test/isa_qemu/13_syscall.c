#include <stdio.h>

#if defined(__x86_64__)
#define ZC_SYS_WRITE 1
#else
#define ZC_SYS_WRITE 64
#endif

#ifdef __zerocc__
static long do_write(int fd, const char *buf, long len) {
    long a = 11, b = 22, c = 33;
    long rc = __zerocc_syscall(ZC_SYS_WRITE, fd, buf, len);
    if (a + b + c != 66) return -1;
    return rc;
}
#else
#include <unistd.h>
static long do_write(int fd, const char *buf, long len) {
    return (long)write(fd, buf, (size_t)len);
}
#endif

int main(void) {
    const char *msg = "isa-qemu-syscall-ok\n";
    long rc = do_write(1, msg, 20);
    printf("rc=%ld\n", rc);
    return (rc == 20) ? 0 : 1;
}
