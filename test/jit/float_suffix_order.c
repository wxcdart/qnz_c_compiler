/* Sprint 105: floating-literal suffix order (GCC accepts i/I and f/F/l/L in
 * either order).
 *
 * C99 6.14.2.2 / GCC: a floating imaginary literal may carry the imaginary
 * suffix `i`/`I` combined with the float suffix `f`/`F` (or long-double
 * `l`/`L`) in EITHER order - GCC accepts `1.0iF`, `1.0fi`, `1.0Fi`, `1.0li`,
 * etc.  The lexer previously consumed the suffixes only in `i`-before-`f`
 * order, so `1.0fi` (f-then-i) left the `i` dangling and mis-lexed the
 * literal.  Now both orders parse, the value is the IMAGINARY part, and the
 * float bit selects the 4-byte (float) vs 8-byte (double) half.
 *
 * c-torture: execute/complex-5.c, compile/attr-complex-method{,-2}.c.
 * Compiled with host gcc (make test) and zerocc -O0..-O3 (make test-opt);
 * expected exit code 0. */
#include <stdio.h>

/* Both suffix orders must agree and be the imaginary part. */
_Complex a = 1.0fi;   /* f then i */
_Complex b = 1.0iF;   /* i then F */
_Complex c = 2.0Fi;   /* F then i */
_Complex d = 3.0i;    /* imaginary only (double) */
_Complex e = 1.0 + 14.0 * (1.0fi);   /* paren + mul, f-then-i */

int main (void)
{
    if (__real__ (a) != 0.0 || __imag__ (a) != 1.0)
        return 1;
    if (__real__ (b) != 0.0 || __imag__ (b) != 1.0)
        return 2;
    if (__real__ (c) != 0.0 || __imag__ (c) != 2.0)
        return 3;
    if (__real__ (d) != 0.0 || __imag__ (d) != 3.0)
        return 4;
    if (__real__ (e) != 1.0 || __imag__ (e) != 14.0)
        return 5;
    return 0;
}
