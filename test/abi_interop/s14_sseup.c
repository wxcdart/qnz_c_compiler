/* ABI interop: s14 SSEup 16B structs (double + long long, both member orders)
 * as by-value ARGS and RETURN VALUES, both directions (Sprint 158 System V
 * class merge: return int half always in RAX, FP half in XMM0).
 * expect: 2.2500 103
 * expect: 4 5.5000
 */
#include <stdio.h>
typedef struct { double a; long long b; } M16;
typedef struct { long long a; double b; } M16b;
M16 zc_m16(M16, double); M16 gc_m16(M16, double);
M16b zc_m16b(M16b); M16b gc_m16b(M16b);
int main(void) {
    M16 m = {1.5, 100};
    m = zc_m16(m, 0.5);
    m = gc_m16(m, 0.25);
    printf("%.4f %lld\n", m.a, m.b);
    M16b n = {7, 2.5};
    n = zc_m16b(n);
    n = gc_m16b(n);
    printf("%lld %.4f\n", n.a, n.b);
    return 0;
}
