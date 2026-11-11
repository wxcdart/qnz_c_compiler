/* ABI interop: s04 >16B struct returns (sret) (shared main, compiled by BOTH compilers)
 * expect: 11 1499 0.5
 * expect: 6 11 106
 */
#include <stdio.h>
typedef struct { int a[2]; long long b[2]; double c[2]; } S32;
typedef struct { long long v[6]; int tag; } S48;
S32 zc_make32(long); S32 gc_make32(long);
S48 zc_bump48(S48, long); S48 gc_bump48(S48, long);
int main(void) {
    S32 z = zc_make32(5), g = gc_make32(5);
    printf("%d %lld %.1f\n", z.a[1] + g.a[0], z.b[0] + g.b[1], z.c[1] + g.c[0]);
    S48 s;
    for (int i = 0; i < 6; i++) s.v[i] = i;
    s.tag = 100;
    s = zc_bump48(s, 10);
    s = gc_bump48(s, 4);
    printf("%lld %lld %d\n", s.v[0], s.v[5], s.tag);
    return 0;
}

