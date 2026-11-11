/* zero_san v1 (Sprint 161): ++/-- signed long overflow must trap (SIGILL,
 * rc 132) via the 64-bit OF-flag check (width 8). */
#include <limits.h>

int main(void) {
    long lg = LONG_MAX;
    lg++;                /* LONG_MAX + 1 overflows long: must trap */
    (void)lg;
    return 0;
}
