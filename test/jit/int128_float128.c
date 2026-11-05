/* __float128 (binary128) support - Sprint 112.
 *
 * ZeroCC models __float128 as a 16-byte, 16-aligned object whose value
 * travels through the 64-bit double value model (same object/value model
 * as long double).  All test values are exactly representable in double
 * precision, so output is identical to a true binary128 implementation.
 */
#include <stdio.h>
#include <string.h>

#define CHK(cond, ...) do { \
    if (!(cond)) { printf("FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); return 1; } \
} while (0)

__float128 g_f = 4.5;
__float128 g_f_int = 7;           /* integer-initialized */

typedef struct {
    int a;
    __float128 f;                 /* 8-byte hole, then 16 bytes @ offset 8 */
    int b;
} S;

__float128 fadd(__float128 a, __float128 b) { return a + b; }
__float128 fsub(__float128 a, __float128 b) { return a - b; }
__float128 fmul(__float128 a, __float128 b) { return a * b; }
__float128 fdiv(__float128 a, __float128 b) { return a / b; }
__float128 ident(__float128 a) { return a; }
__float128 twoargs(__float128 a, int i, __float128 b) { return a * 2.0 + b * i; }

int main(void) {
    /* 1. sizeof / alignof */
    CHK(sizeof(__float128) == 16, "sizeof=%zu", sizeof(__float128));
    CHK(_Alignof(__float128) == 16, "align=%zu", _Alignof(__float128));
    CHK(sizeof(S) == 48, "sizeof(S)=%zu", sizeof(S));

    /* 2. locals + arithmetic (all double-exact values) */
    __float128 x = 1.5;
    __float128 y = x + 2.25;
    CHK((double)y == 3.75, "y=%.17g", (double)y);
    y = y - x;
    CHK((double)y == 2.25, "y=%.17g", (double)y);
    y = y * 4.0;
    CHK((double)y == 9.0, "y=%.17g", (double)y);
    y = y / 3.0;
    CHK((double)y == 3.0, "y=%.17g", (double)y);
    y = y * y;
    CHK((double)y == 9.0, "y=%.17g", (double)y);

    /* 3. comparisons */
    __float128 p = 2.5, q = 2.5, r = 3.5;
    CHK(p == q, "==");
    CHK(p != r, "!=");
    CHK(p < r, "<");
    CHK(r > p, ">");
    CHK(p <= q, "<=");
    CHK(r >= p, ">=");

    /* 4. integer initializers + compound assignment */
    __float128 i1 = 5;
    CHK((double)i1 == 5.0, "i1=%.17g", (double)i1);
    i1 += 2.5;
    CHK((double)i1 == 7.5, "i1=%.17g", (double)i1);
    i1 *= 2.0;
    CHK((double)i1 == 15.0, "i1=%.17g", (double)i1);
    i1 /= 5.0;
    CHK((double)i1 == 3.0, "i1=%.17g", (double)i1);

    /* 5. casts: float128 <-> double, float128 -> int */
    double d1 = (double)x;
    CHK(d1 == 1.5, "d1=%.17g", d1);
    __float128 d2 = (__float128)2.75;
    CHK((double)d2 == 2.75, "d2=%.17g", (double)d2);
    int iv = (__float128)9.0;   /* float -> int conversion */
    CHK(iv == 9, "iv=%d", iv);
    __float128 from_int = (__float128)4;
    CHK((double)from_int == 4.0, "from_int=%.17g", (double)from_int);

    /* 6. globals */
    CHK((double)g_f == 4.5, "g_f=%.17g", (double)g_f);
    CHK((double)g_f_int == 7.0, "g_f_int=%.17g", (double)g_f_int);
    g_f = g_f + 0.25;
    CHK((double)g_f == 4.75, "g_f=%.17g", (double)g_f);
    __float128 gcopy = g_f;
    CHK((double)gcopy == 4.75, "gcopy=%.17g", (double)gcopy);

    /* 7. struct members */
    S s;
    s.a = 1;
    s.f = 3.25;
    s.b = 2;
    CHK(((int*)&s.f - (int*)&s.a) == 4, "f offset=%ld", (long)((int*)&s.f - (int*)&s.a));
    CHK((double)s.f == 3.25, "s.f=%.17g", (double)s.f);
    s.f = s.f + 0.75;
    CHK((double)s.f == 4.0, "s.f=%.17g", (double)s.f);
    CHK(s.a == 1 && s.b == 2, "neighbors clobbered: %d %d", s.a, s.b);

    /* 8. function calls: pass by value */
    __float128 rv = fadd(1.25, 2.75);
    CHK((double)rv == 4.0, "fadd=%.17g", (double)rv);
    rv = fsub(5.0, 1.25);
    CHK((double)rv == 3.75, "fsub=%.17g", (double)rv);
    rv = fmul(2.5, 4.0);
    CHK((double)rv == 10.0, "fmul=%.17g", (double)rv);
    rv = fdiv(9.0, 2.0);
    CHK((double)rv == 4.5, "fdiv=%.17g", (double)rv);
    rv = ident(6.5);
    CHK((double)rv == 6.5, "ident=%.17g", (double)rv);
    rv = twoargs(1.0, 3, 2.0);        /* 1.0*2 + 2.0*3 = 8.0 */
    CHK((double)rv == 8.0, "twoargs=%.17g", (double)rv);

    /* 9. return value round trips */
    rv = fadd(ident(1.0), fmul(2.0, 3.0));
    CHK((double)rv == 7.0, "nested=%.17g", (double)rv);

    /* 10. array of __float128 */
    __float128 arr[3];
    arr[0] = 1.0; arr[1] = 2.5; arr[2] = 4.25;
    __float128 sum = arr[0];
    int i;
    for (i = 1; i < 3; i++) sum = sum + arr[i];
    CHK((double)sum == 7.75, "sum=%.17g", (double)sum);

    /* 11. pointer load/store */
    __float128 *pp = &g_f;
    CHK((double)*pp == 4.75, "*pp=%.17g", (double)*pp);
    *pp = 8.0;
    CHK((double)g_f == 8.0, "g_f=%.17g", (double)g_f);

    /* 12. ternary + conditional expressions */
    __float128 t = (1 < 2) ? 5.5 : 0.5;
    CHK((double)t == 5.5, "t=%.17g", (double)t);
    t = (1 > 2) ? 5.5 : 0.25;
    CHK((double)t == 0.25, "t=%.17g", (double)t);

    /* 13. (byte layout of the upper 8 bytes is implementation-defined for
     *     the double-value model - not checked here) */

    /* 14. struct containing __float128 passed by value */
    S s2 = { 10, 2.5, 20 };
    CHK((double)s2.f == 2.5, "s2.f=%.17g", (double)s2.f);
    S s3 = s2;                       /* aggregate copy */
    CHK(s3.a == 10 && (double)s3.f == 2.5 && s3.b == 20, "s3 copy");

    /* 15. printf round-trip of the value */
    __float128 pv = 3.5;
    printf("%.1f\n", (double)pv);

    printf("F128-OK\n");
    return 0;
}
