/* ABI interop: s08 function pointers across the boundary (shared main, compiled by BOTH compilers)
 * expect: 21 34
 * expect: 22 70 3.0
 */
#include <stdio.h>
typedef struct { long long a, b; double c; } S24;
int zc_apply(int (*f)(int, int), int, int); int zc_zadd(int, int);
S24 (*zc_pick(int))(S24);
int gc_apply(int (*f)(int, int), int, int); int gc_gmul(int, int);
S24 gc_transform(S24);
int main(void) {
    printf("%d %d\n", zc_apply(gc_gmul, 4, 5), gc_apply(zc_zadd, 4, 5));
    S24 s = {10, 20, 1.0};
    S24 (*f)(S24) = zc_pick(0);
    s = f(s);
    s = gc_transform(s);
    s = zc_pick(1)(s);
    printf("%lld %lld %.1f\n", s.a, s.b, s.c);
    return 0;
}

