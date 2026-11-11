/* ABI interop: s15 variadic struct args (System V overflow model):
 *  - va_arg of a >16B struct (value in the overflow/stack area)
 *  - va_arg of a 16B SSEup struct after consumed (int,double) pairs
 *  - a >16B named struct param before the variadic args (memory arg
 *    consumes no register; va_start skips its stack slots)
 * both directions.
 * expect: 40 10
 * expect: 101.5000 101.5000
 * expect: 6 12
 */
#include <stdio.h>
typedef struct { long long v[4]; } Big;      /* 32B */
typedef struct { double a; long long b; } M16; /* 16B SSEup */
long long zc_bigsum(int, ...); long long gc_bigsum(int, ...);
double zc_msum(int, ...); double gc_msum(int, ...);
long long zc_nbig(Big, int, ...); long long gc_nbig(Big, int, ...);
int main(void) {
    Big b = {1, 2, 3, 4};
    printf("%lld %lld\n", zc_bigsum(2, 10, 20, b), gc_bigsum(0, b));
    M16 m = {1.5, 100};
    printf("%.4f %.4f\n", zc_msum(1, 7, 2.0, m), gc_msum(0, m));
    printf("%lld %lld\n", zc_nbig(b, 1, 5), gc_nbig(b, 2, 5, 6));
    return 0;
}
