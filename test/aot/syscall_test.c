/* AOT test: __zerocc_syscall through zerocc's object emission.
 *
 * make test builds this with host gcc (the __zerocc__ branch is inactive,
 * the libc write path runs); the test-opt sweep and AOT-mode runs compile
 * it with zerocc at -O0..-O3 in both JIT and AOT modes, where the raw
 * syscall intrinsic is emitted and executed.
 */
#include <stdio.h>

#ifdef __zerocc__
/* ZeroCC path: raw write(1, ...) through the OS boundary builtin. */
int main(void) {
    const char *msg = "zerocc-aot-syscall-ok\n";
    long rc = __zerocc_syscall(1, 1, msg, 22);
    if (rc != 22) return 1;
    return 0;
}
#else
/* gcc path (make test builds with the host compiler): same check via libc. */
#include <unistd.h>
int main(void) {
    const char *msg = "zerocc-aot-syscall-ok\n";
    long rc = write(1, msg, 22);
    if (rc != 22) return 1;
    return 0;
}
#endif
