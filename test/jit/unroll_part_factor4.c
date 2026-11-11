// expect-partial: yes
/* Loop unrolling Phase 2 (Sprint 156): a body large enough (span >= 16
 * instructions) to select unroll factor 4 - all four remainder classes
 * (n mod 4 == 0..3). */
#include <stdio.h>

typedef struct { int a, b, c, d; } V4;

long f4(const V4 *v, unsigned n) {
    long s = 0;
    for (unsigned i = 0; i < n; i++) {
        int x = v[i].a * 3 + v[i].b - v[i].c;
        int y = x / 2 + v[i].d;
        s += (long)x * y;
        if (y & 1) s += 7;
    }
    return s;
}

int main(void) {
    V4 v[12] = {{1,2,3,4},{2,3,4,5},{3,4,5,6},{4,5,6,7},
                {5,6,7,8},{6,7,8,9},{7,8,9,10},{8,9,10,11},
                {9,10,11,12},{10,11,12,13},{11,12,13,14},{12,13,14,15}};
    long tot = 0;
    for (unsigned n = 0; n <= 11; n++) tot += f4(v, n);
    printf("%ld\n", tot);
    return 0;
}
