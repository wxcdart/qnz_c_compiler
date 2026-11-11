/* ABI interop: s06 big struct args mixed with scalars, packing (shared main, compiled by BOTH compilers)
 * expect: 188
 * expect: 310
 * expect: 274
 */
#include <stdio.h>
typedef struct { long long a, b, c; } S24m;
long long zc_mixed1(int, S24m, int, double);
long long gc_mixed1(int, S24m, int, double);
long long zc_mixed2(S24m, S24m, long long);
long long gc_mixed2(S24m, S24m, long long);
long long zc_mixed3(S24m, S24m, S24m, int);
long long gc_mixed3(S24m, S24m, S24m, int);
int main(void) {
    S24m m = {10, 20, 30};
    S24m m2 = {40, 50, 60};
    S24m m3 = {70, 80, 90};
    printf("%lld\n", zc_mixed1(1, m, 4, 2.5) + gc_mixed1(3, m2, 8, 1.5));
    printf("%lld\n", zc_mixed2(m, m2, 7) + gc_mixed2(m2, m3, 11));
    printf("%lld\n", zc_mixed3(m, m2, m3, 2) + gc_mixed3(m3, m, m2, 4));
    return 0;
}

