// expect-unroll: yes
// expect-partial: yes
/* Loop unrolling Phase 2 (Sprint 156): a single function mixing a
 * constant-trip loop (full unroll, Phase 1) and a runtime-bound loop
 * (partial unroll + peel, Phase 2) - both counters must fire and both
 * transforms must be semantically exact. */
#include <stdio.h>

long mixed(unsigned n) {
    long a = 0;
    for (int i = 0; i < 5; i++) a += i * 2;     /* constant trip: full unroll */
    long b = 0;
    for (unsigned i = 0; i < n; i++) b += i * 3; /* runtime bound: partial */
    return a + b;
}

int main(void) {
    long t = 0;
    for (unsigned n = 0; n <= 7; n++) t += mixed(n);
    printf("%ld\n", t);
    return 0;
}
