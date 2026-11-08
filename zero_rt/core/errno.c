/* zero_rt/core/errno.c
 *
 * zero_rt's errno object.  The C standard exposes `errno` as a per-thread
 * int; zero_rt is single-threaded, so one process-global int suffices.
 * It is exposed through __errno_location() following the glibc convention
 * (glibc's <errno.h> maps `errno` to `(*__errno_location())`); zerocc's
 * virtual <errno.h> uses the same mapping, so host-linked and
 * --zerocc-link builds share one user-visible macro and the runtime only
 * provides a plain function + plain global (no TLS in the own runtime).
 */

static int zc_errno = 0;

int *__errno_location(void) {
    return &zc_errno;
}
