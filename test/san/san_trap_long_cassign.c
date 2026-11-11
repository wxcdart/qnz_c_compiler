/* zero_san v1 (Sprint 161): compound assignment signed long overflow must
 * trap (SIGILL, rc 132) via the 64-bit OF-flag check (width 8). */
#include <limits.h>

int main(void) {
    long x = LONG_MAX;
    x += 1L;             /* LONG_MAX + 1 overflows long: must trap */
    (void)x;
    return 0;
}
