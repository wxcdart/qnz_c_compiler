#include <stdio.h>
#include <assert.h>

/* Pointer arithmetic regression test (C11 6.5.6).
 *
 * Guards three pre-existing codegen bugs:
 *   1. ++/-- on a pointer-typed variable advanced by 1 byte instead of
 *      sizeof(*p) (`*ip = 110; ip++; *ip = 120;` miscompiling the store).
 *   2. `p + n` / `p - n` not scaled by the pointee size, and `p - q`
 *      yielding a byte difference instead of an element difference.
 *   3. ++/-- on non-ident lvalues (a[i]++, (*p)++, s.f++, p->f++)
 *      lowering to a silent no-op.
 * Run at -O0..-O3, JIT and AOT, by the optimizer sweep.
 */
struct P { int a; float f; int b; };

int g;
int *gp = &g;
struct P gsp = { 1, 1.0f, 3 };

int main(void) {
    /* char* step 1 (regression control) */
    {
        char s[] = "hello";
        char *c = s;
        c++;
        c++;
        assert(*c == 'l' && c - s == 2);
    }
    /* int** step 8 */
    {
        int x = 5, y = 6;
        int *rows[2] = { &x, &y };
        int **m = rows;
        m++;
        assert(*m == &y);
    }
    /* n + p (integer on the left) */
    {
        int a[3] = { 1, 2, 3 };
        int *p = a;
        assert((2 + p) == &a[2]);
    }
    /* p - q element difference (positive, negative, zero) */
    {
        int a[5];
        int *p = a, *q = a + 4;
        assert(q - p == 4);
        assert(p - q == -4);
        assert(p - p == 0);
    }
    /* array decay + int */
    {
        int a[3];
        assert(a + 2 == &a[2]);
    }
    /* address-of differences */
    {
        int a[3];
        assert(&a[2] - &a[0] == 2);
    }
    /* global pointer variable ++ (scaled) */
    {
        g = 0;
        gp++;
        assert(gp == &g + 1);
        gp = &g;
    }
    /* struct member ++ (dot, prefix + postfix, float member) */
    {
        struct P s = { 10, 2.0f, 30 };
        s.a++;
        assert(s.a == 11);
        s.a = 10;
        ++s.b;
        assert(s.b == 31);
        struct P f = { 0, 2.0f, 0 };
        f.f++;
        assert(f.f == 3.0f);
    }
    /* arrow member ++ through a global struct pointer */
    {
        struct P *pp = &gsp;
        pp->a++;
        assert(gsp.a == 2);
        gsp.a = 1;
    }
    /* combined *ip++ and a[t++] (the original repro) */
    {
        int a[3] = { 10, 20, 30 };
        int *ip = a;
        *ip = 110;
        ip++;
        *ip = 120;
        assert(a[0] == 110 && a[1] == 120 && a[2] == 30);
    }
    {
        int v[3] = { 10, 20, 30 };
        int *ip = v;
        int r = *ip++;
        assert(r == 10 && ip == &v[1]);
        int t = 0;
        int s2 = v[t++];
        assert(s2 == 10 && t == 1);
    }
    /* prefix/postfix pointer values */
    {
        int a[3] = { 7, 8, 9 };
        int *p = a;
        int *old = p++;
        assert(old == &a[0] && p == &a[1]);
        int *old2 = --p;
        assert(old2 == &a[0]);
    }
    /* long* step 8 */
    {
        long l[2] = { 1, 2 };
        long *lp = l;
        lp++;
        assert(lp == &l[1]);
    }
    /* pointer decrement and compound step */
    {
        int a[4] = { 1, 2, 3, 4 };
        int *p = a + 3;
        p--;
        assert(*p == 3);
        p += 1;
        assert(p == a + 3);
        p -= 2;
        assert(p == a + 1);
        p--;
        assert(p == a);
    }

    printf("Pointer arithmetic test passed\n");
    return 0;
}
