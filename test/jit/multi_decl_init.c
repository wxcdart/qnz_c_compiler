/* Type System Hardening #4 burn-down (Sprint 123): multi-declarator
 * initializers must all survive - `char a[] = "ozz", b[] = "oxx";` used
 * to drop the SECOND declarator's initializer at -O0 (Sprint 115j side
 * finding; the OKF/plan listed it as open).  Verified fixed; this test
 * pins the shapes: char[]/int[]/struct/union, global and local,
 * mixed initialized/uninitialized. */
#include <stdio.h>

/* global multi-declarator initializers (the original repro) */
char a[] = "ozz", b[] = "oxx";
char x1[] = "aa", x2[] = "bb";
int ia1[2] = {1, 2}, ia2[2] = {3, 4};

struct P { int v; int w; };
struct P p1 = {10, 1}, p2 = {20, 2};

union U { int i; char c[2]; };
union U u1 = {7}, u2 = {8};

int main(void) {
    int r = 0;

    /* char[]: both strings complete + NUL-terminated */
    r += !(a[0] == 'o' && a[1] == 'z' && a[2] == 'z' && a[3] == 0);
    r += !(b[0] == 'o' && b[1] == 'x' && b[2] == 'x' && b[3] == 0);
    r += !(x1[0] == 'a' && x1[2] == 0 && x2[0] == 'b' && x2[2] == 0);

    /* int[]: every element of BOTH declarators */
    r += !(ia1[0] == 1 && ia1[1] == 2);
    r += !(ia2[0] == 3 && ia2[1] == 4);

    /* struct: every member of BOTH declarators */
    r += !(p1.v == 10 && p1.w == 1);
    r += !(p2.v == 20 && p2.w == 2);

    /* union: both initializers visible through the same member */
    r += !(u1.i == 7);
    r += !(u2.i == 8);

    /* local multi-declarator initializers */
    {
        char l1[] = "ll", l2[] = "mm";
        int r1[2] = {5, 6}, r2[2] = {7, 8};
        r += !(l1[0] == 'l' && l1[2] == 0);
        r += !(l2[0] == 'm' && l2[2] == 0);
        r += !(r1[0] == 5 && r1[1] == 6);
        r += !(r2[0] == 7 && r2[1] == 8);
    }

    /* mixed: the second declarator uninitialized must NOT be given the
     * first declarator's size inference - it is a plain zero-length-free
     * char[4] (automatic storage: contents indeterminate, so only the
     * WRITTEN element is checked). */
    {
        char m1[] = "qq", m2[4];
        m2[0] = 'Z';
        r += !(m1[0] == 'q' && m1[2] == 0);
        r += !(m2[0] == 'Z');
    }

    /* three comma declarators, all unsized + string-initialized */
    {
        char a3[] = "aa", b3[] = "bb", c3[] = "cc";
        r += !(a3[0] == 'a' && a3[2] == 0);
        r += !(b3[0] == 'b' && b3[2] == 0);
        r += !(c3[0] == 'c' && c3[2] == 0);
    }

    /* static locals, comma list (each gets its own data object) */
    {
        static char sa[] = "ss", sb[] = "tt";
        r += !(sa[0] == 's' && sa[2] == 0);
        r += !(sb[0] == 't' && sb[2] == 0);
    }

    /* unsized struct arrays, comma list, brace-initialized */
    {
        struct T2 { int x; int y; };
        struct T2 t2[] = {{5, 6}, {7, 8}};
        r += !(t2[0].x == 5 && t2[0].y == 6);
        r += !(t2[1].x == 7 && t2[1].y == 8);
    }

    /* unsized int arrays, comma list, brace-initialized (unequal sizes) */
    {
        int i1[] = {1, 2}, i2[] = {3, 4, 5};
        r += !(i1[0] == 1 && i1[1] == 2);
        r += !(i2[0] == 3 && i2[2] == 5);
    }

    printf("%s %s %d %d %d %d\n", a, b, ia2[1], p2.v, u2.i, ia1[0]);
    return r;
}
