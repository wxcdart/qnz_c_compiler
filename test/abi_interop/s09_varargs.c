/* ABI interop: s09 variadic functions both directions (shared main, compiled by BOTH compilers)
 * expect: 26 20.00
 * expect: 21
 * expect: 16 24.0
 */
#include <stdio.h>
int zc_vsum(int, ...); double zc_vavg(int, ...); int zc_mixed_va(const char *, int, double, ...);
long long gc_varange(int, ...); double gc_prod(int, ...);
int main(void) {
    printf("%d %.2f\n", zc_vsum(4, 3, 5, 7, 11), zc_vavg(3, 10, 20, 30));
    printf("%d\n", zc_mixed_va("ok", 5, 2.5, 3));
    printf("%lld %.1f\n", gc_varange(10, 1, 2, 3, -1), gc_prod(3, 2, 3, 4));
    return 0;
}

