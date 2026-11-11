// expect-partial: yes
/* Loop unrolling Phase 2 (Sprint 156): for-form continue in a partially
 * unrolled loop - a continue must re-aim at the SAME copy's step label
 * (the current iteration's i++ still runs), and the interior loop tests
 * skipped between copies are provably true. */
#include <stdio.h>

long even_sum(const int *a, unsigned n) {
    long s = 0;
    for (unsigned i = 0; i < n; i++) {
        if (a[i] & 1) continue;
        s += a[i];
    }
    return s;
}

int main(void) {
    int a[11] = {2,1,4,3,6,5,8,7,10,9,12};
    long t = even_sum(a, 11);      /* 2+4+6+8+10+12 = 42 */
    t += even_sum(a, 0);           /* 0 */
    t += even_sum(a, 1);           /* 2 (only even element) */
    printf("%ld\n", t);
    return 0;
}
