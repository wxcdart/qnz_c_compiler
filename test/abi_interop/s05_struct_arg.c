/* ABI interop: s05 >16B struct args by value (memory args) (shared main, compiled by BOTH compilers)
 * expect: 8 101 3.00 48
 * expect: 7 17 1.50
 * expect: 1 61 235 49
 */
#include <stdio.h>
typedef struct { int a; long long b; double c; } S24;
typedef struct { int a[2]; long long b[2]; double c[2]; } S32;
typedef struct { long long v[6]; int tag; } S48;
S24 zc_s24(S24, int); S24 gc_s24(S24, int);
S32 zc_s32(S32, long); S32 gc_s32(S32, long);
S48 zc_s48(S48, long); S48 gc_s48(S48, long);
int zc_sum24(const S24 *); long long gc_sum48(const S48 *);
int main(void) {
    S24 s = {7, 100, 2.0};
    s = zc_s24(s, 3);
    s = gc_s24(s, 2);
    printf("%d %lld %.2f %d\n", s.a, s.b, s.c, zc_sum24(&s));

    S32 t;
    for (int i = 0; i < 2; i++) { t.a[i] = i; t.b[i] = 10 + i; t.c[i] = 0.5; }
    t = zc_s32(t, 4);
    t = gc_s32(t, 1);
    printf("%d %lld %.2f\n", t.a[0] + t.a[1], t.b[0] + t.b[1], t.c[0] + t.c[1]);

    S48 u;
    for (int i = 0; i < 6; i++) u.v[i] = i * 10;
    u.tag = 50;
    u = zc_s48(u, 2);
    u = gc_s48(u, 1);
    printf("%lld %lld %lld %d\n", u.v[0], u.v[5], gc_sum48(&u), u.tag);
    return 0;
}

