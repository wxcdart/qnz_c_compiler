// expect-partial: yes
/* Loop unrolling Phase 2 (Sprint 156): runtime-bound unsigned loop
 * (parameter n) - partial unroll + remainder peel, factor 2. */
#include <stdio.h>

long sum_arr(const int *a, unsigned n) {
    long s = 0;
    for (unsigned i = 0; i < n; i++) s += a[i];
    return s;
}

int main(void) {
    int a[10] = {1,2,3,4,5,6,7,8,9,10};
    long tot = 0;
    for (unsigned n = 0; n <= 9; n++) tot += sum_arr(a, n);
    /* sum of prefix sums: 0+1+3+6+10+15+21+28+36+45 = 165 */
    printf("%ld\n", tot);
    return 0;
}
