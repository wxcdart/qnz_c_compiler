/* zero_san v1 (Sprint 161): compound assignment signed overflow must trap
 * (SIGILL, rc 132) under -fsanitize=undefined. Guards the += -= *= OP_OVF_TRAP
 * checks at width 4 (int) across -O levels. */
#include <limits.h>

int main(void) {
    int x = INT_MAX;
    x += 1;              /* INT_MAX + 1 overflows int: must trap */
    (void)x;
    return 0;
}
