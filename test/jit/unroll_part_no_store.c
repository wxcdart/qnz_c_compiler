// expect-partial: no
/* Loop unrolling Phase 2 (Sprint 156): REJECTION case - the bound slot is
 * STORED by the loop body, so it is not loop-invariant; partial unrolling
 * must refuse (the pass stays off and the verifier confirms
 * "partially unrolled: 0"). */
#include <stdio.h>

long consume(unsigned n, const int *a) {
    long s = 0;
    for (unsigned i = 0; i < n; i++) {
        s += a[i];
        n--;                       /* clobbers the bound mid-loop */
    }
    return s;
}

int main(void) {
    int a[5] = {1, 2, 3, 4, 5};
    /* i=0: 0<5 s=1 n=4 | i=1: 1<4 s=3 n=3 | i=2: 2<3 s=6 n=2 | i=3: 3<2 stop */
    printf("%ld\n", consume(5, a));   /* 6 */
    return 0;
}
