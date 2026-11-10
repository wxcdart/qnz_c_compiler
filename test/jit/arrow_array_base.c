/* Type System Hardening #4 burn-down (Sprint 123): `d->m` on an ARRAY
 * base.  GCC accepts it (GNU extension): `d->m` on `struct S d[N]` is
 * `d[0].m`.  The OKF listed it as an honest REJECT because the arrow
 * lowering allegedly loaded the array *value* instead of its address
 * ("the IR fix is deferred").  Verified fixed (local + global arrays,
 * read + write, subscripted base); this test pins it.
 *
 * Valid-C companion forms: `(&d[1])->m` and `d[0].m`.
 * Invalid (rejected by both gcc and zerocc, pinned by
 * test/diag/17_dot_on_array.c): `d.m` on an array base and `p->m` for
 * `struct S (*p)[N]`. */
#include <stdio.h>

struct S { int m; int n; };

struct S garr[2] = {{100, 1}, {200, 2}};

int main(void) {
    int r = 0;
    struct S d[3] = {{1, 10}, {2, 20}, {3, 30}};

    /* local array base: read + write */
    r += (d->m != 1);
    r += (d->n != 10);
    d->m = 42;
    r += (d[0].m != 42);
    r += (d[1].m != 2);

    /* global array base: read + write */
    r += (garr->m != 100);
    garr->m = 101;
    r += (garr[0].m != 101);
    r += (garr->n != 1);
    r += (garr[1].m != 200);

    /* subscripted array base */
    (&d[1])->m = 55;
    r += (d[1].m != 55);
    r += (d[1].n != 20);

    /* pointer to struct: the ordinary case still works */
    struct S *sp = &d[2];
    sp->m = 99;
    r += (d[2].m != 99);
    r += (sp->n != 30);

    printf("->m=%d g->m=%d d1->m=%d\n", d->m, garr->m, d[1].m);
    return r;
}
