// expect-unroll: yes
/* Loop unrolling (Sprint 150): trip-count boundary.
 * 8 (== LU_MAX_TRIP, unrolls) and 9 (just over the cap, stays a loop).
 * Also run with -fsanitize=undefined: the unrolled copies keep their
 * overflow checks, and this program (no overflow) must never trap. */
#include <stdio.h>

int f8(void) {
    int s = 0;
    for (int i = 0; i < 8; i++) s += i * 100 + 7;
    return s;
}

int f9(void) {
    int s = 0;
    for (int i = 0; i < 9; i++) s += i * 100 + 7;
    return s;
}

int main(void) {
    printf("%d %d\n", f8(), f9());
    return 0;
}
