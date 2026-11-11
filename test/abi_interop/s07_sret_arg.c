/* ABI interop: s07 sret return + big struct arg (shared main, compiled by BOTH compilers)
 * expect: 3 2028 3.041
 * expect: 101 200 301.000
 */
#include <stdio.h>
typedef struct { int a; long long b; double c; } S24;
typedef struct { long long a, b; double c; } S24m;
S24 zc_sret_id(S24); S24 gc_sret_id(S24);
S24m zc_sret_mixed(int, S24m, double); S24m gc_sret_mixed(int, S24m, double);
int main(void) {
    S24 s = {4, 1004, 9.0};
    s = zc_sret_id(s);
    s = gc_sret_id(s);
    printf("%d %lld %.3f\n", s.a, s.b, s.c);
    S24m m = {100, 200, 300};
    m = zc_sret_mixed(3, m, 1.5);
    m = gc_sret_mixed(2, m, 2.5);
    printf("%lld %lld %.3f\n", m.a, m.b, m.c);
    return 0;
}

