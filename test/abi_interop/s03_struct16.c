/* ABI interop: s03 small (<=16B) by-value structs (shared main, compiled by BOTH compilers)
 * expect: 2 100
 * expect: 1 2 c
 * expect: 0 5 0 6
 * expect: 3.08 7.75
 */
#include <stdio.h>
typedef struct { int a; long long b; } S8;
typedef struct { int a; int b; char c; } S9;
typedef struct { int a, b, c, d; } S16;
typedef struct { double a, b; } S16f;
S8 zc_s8_in_out(S8, int); S8 gc_s8(S8, int);
S9 zc_s9(S9); S9 gc_s9(S9);
S16 zc_s16(S16, int); S16 gc_s16(S16, int);
S16f zc_s16f(S16f); S16f gc_s16f(S16f);
int main(void) {
    S8 a = {1, 100};
    a = zc_s8_in_out(a, 3);
    a = gc_s8(a, 2);
    printf("%d %lld\n", a.a, a.b);
    S9 b = {5, -3, 'a'};
    b = zc_s9(b);
    b = gc_s9(b);
    printf("%d %d %c\n", b.a, b.b, b.c);
    S16 c = {1, 2, 3, 4};
    c = zc_s16(c, 1);
    c = gc_s16(c, 2);
    printf("%d %d %d %d\n", c.a, c.b, c.c, c.d);
    S16f d = {9.0, 4.0};
    d = zc_s16f(d);
    d = gc_s16f(d);
    printf("%.2f %.2f\n", d.a, d.b);
    return 0;
}

