/* ABI interop: s13 large (256B/1K/4K) by-value struct args (shared main, compiled by BOTH compilers)
 * expect: 1056
 * expect: 729
 * expect: 8 -126720
 */
#include <stdio.h>
typedef struct { long long v[32]; } S256;
typedef struct { long long v[128]; } S1K;
typedef struct { long long v[512]; } S4K;
long long zc_fill(S256, long long); long long gc_sum256(S256, long long);
long long zc_fill1k(S1K, long long); long long gc_half1k(S1K, long long);
S4K zc_flip4k(S4K, long long); S4K gc_add4k(S4K, long long);
int main(void) {
    S256 a;
    for (int i = 0; i < 32; i++) a.v[i] = i;
    printf("%lld\n", zc_fill(a, 3) + gc_sum256(a, 1));

    S1K b;
    for (int i = 0; i < 128; i++) b.v[i] = i * 2;
    printf("%lld\n", zc_fill1k(b, 10) + gc_half1k(b, 2));

    S4K c;
    for (int i = 0; i < 512; i++) c.v[i] = i;
    c = zc_flip4k(c, 7);
    c = gc_add4k(c, 1);
    long long t = 0;
    for (int i = 0; i < 512; i++) t += c.v[i];
    printf("%lld %lld\n", c.v[0], t);
    return 0;
}

