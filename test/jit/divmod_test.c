#include <stdio.h>
#include <assert.h>

/* Signed integer division / modulo regression test.
 *
 * Guards the x86-64 machine-emitter divmod dst-aliasing bug: the quotient
 * of idiv lives in rax and the remainder in rdx, but the old code skipped
 * the result move whenever the destination temp was allocated rax/rdx.
 * `8/4` with the result temp on rdx silently returned 8%4 == 0.
 * Loop-driven forms (not constant-foldable) are included so the sweep's
 * -O1..-O3 passes exercise the real idiv path too.
 */
int main(void) {
    /* direct power-of-two cases (the original repro) */
    assert(8 / 4 == 2);
    assert(-8 / 4 == -2);
    assert(16 / 2 == 8);
    assert(100 / 10 == 10);
    assert(7 / 3 == 2);
    assert(10 / 4 == 2);
    assert(-10 / 4 == -2);
    assert(-7 / 3 == -2);

    /* remainder */
    assert(8 % 4 == 0);
    assert(7 % 3 == 1);
    assert(-7 % 3 == -1);
    assert(100 % 10 == 0);
    assert(16 % 2 == 0);

    /* loop-driven (no constant folding) */
    int v[16] = { 8, 4, 16, 2, 100, 10, 7, 3, 9, 4, 20, 5, -8, 4, -100, 10 };
    int r[8];
    for (int i = 0; i < 8; i++) r[i] = v[2 * i] / v[2 * i + 1];
    assert(r[0] == 2);   /* 8/4 */
    assert(r[1] == 8);   /* 16/2 */
    assert(r[2] == 10);  /* 100/10 */
    assert(r[3] == 2);   /* 7/3 */
    assert(r[4] == 2);   /* 9/4 */
    assert(r[5] == 4);   /* 20/5 */
    assert(r[6] == -2);  /* -8/4 */
    assert(r[7] == -10); /* -100/10 */

    int m[8];
    for (int i = 0; i < 8; i++) m[i] = v[2 * i] % v[2 * i + 1];
    assert(m[0] == 0);
    assert(m[1] == 0);
    assert(m[2] == 0);
    assert(m[3] == 1);
    assert(m[4] == 1);
    assert(m[5] == 0);
    assert(m[6] == 0);
    assert(m[7] == 0);

    /* division used as a pointer-difference building block (ptr - ptr) */
    int a[5];
    assert(&a[4] - &a[0] == 4);
    assert(&a[0] - &a[4] == -4);

    printf("Division/modulo test passed\n");
    return 0;
}
