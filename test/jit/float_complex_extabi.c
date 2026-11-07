/* Type System Hardening #4 burn-down (Sprint 123): float _Complex across
 * an EXTERNAL (non-zerocc) call boundary.  The System V AMD64 ABI passes
 * a float _Complex in ONE xmm register as two FLOATS (low half in the
 * low 32 bits).  If the caller marshals the halves as doubles (8 bytes
 * each), the libm callee reads garbage - the Sprint 115l "known
 * residual" (csinf(1.0F+2.0iF) against libm).  Verified fixed; this test
 * pins the float _Complex argument AND return through glibc's csinf
 * (weak export, present on the standard build host).
 *
 * The double _Complex control (csin) exercises the same codegen path at
 * 16-byte granularity and must stay exact. */
#include <stdio.h>
#include <complex.h>

extern float _Complex csinf(float _Complex);
extern double _Complex csin(double _Complex);

static int near(double a, double b, double tol) {
    double d = a - b;
    if (d < 0) d = -d;
    return d <= tol;
}

int main(void) {
    int r = 0;

    /* float _Complex argument + return (the documented repro) */
    float _Complex r1 = csinf(1.0f + 2.0f * I);
    /* csinf(x+iy) = sin(x)cosh(y) + i cos(x)sinh(y)
     * csinf(1+2i) = sin(1)cosh(2) + i cos(1)sinh(2)
     *             ~=  3.1657785 + 1.9596010i  (float precision) */
    float _Complex r2 = csinf(0.5f - 0.25f * I);
    /* csinf(0.5-0.25i) = sin(0.5)cosh(0.25) - i cos(0.5)sinh(0.25)
     *                 ~=  0.4944858 - 0.2216882i */

    double fr1 = __real__(r1), fi1 = __imag__(r1);
    double fr2 = __real__(r2), fi2 = __imag__(r2);

    /* NaN/Inf check: finite in, finite out */
    r += (fr1 != fr1) + (fi1 != fi1) + (fr2 != fr2) + (fi2 != fi2);
    r += !(near(fr1, 3.1657785, 2e-3));
    r += !(near(fi1, 1.9596010, 2e-3));
    r += !(near(fr2, 0.4944858, 2e-3));
    r += !(near(fi2, -0.2216882, 2e-3));

    /* double _Complex control: the same math at double precision -
     * float and double results must agree to float tolerance. */
    double _Complex rc = csin(1.0 + 2.0 * I);
    /* csin(1+2i) = sin(1)cosh(2) + i cos(1)sinh(2) (double) */
    r += !(near(__real__(rc), 3.1657785132, 1e-6));
    r += !(near(__imag__(rc), 1.9596010414, 1e-6));

    printf("r1 %.6g %.6g\n", fr1, fi1);
    printf("r2 %.6g %.6g\n", fr2, fi2);
    return r;
}
