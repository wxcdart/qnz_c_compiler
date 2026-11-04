/* Sprint 104: standalone `_Complex` (GNU: means `_Complex double`).
 *
 * C99 6.7.2.3 requires the real type to be named (`_Complex double`), but
 * GCC (gnu mode) accepts a bare `_Complex` as `_Complex double`.  ZeroCC
 * previously swallowed the declarator as a defaulted-int inner type, so a
 * plain `_Complex x;` was silently a complex-of-int (sizeof 4/8) instead of
 * complex double (sizeof 16) - a latent miscompile.  Now a standalone
 * `_Complex` in every declaration position (file-scope global, local,
 * function parameter, function return type) denotes `_Complex double`, and
 * `_Complex <base>` keeps the explicit base.  The K&R (old-style) param
 * shape scan must not count `_Complex` as a bare parameter name, or
 * `void f(_Complex z) { }` is mis-read as a two-name K&R list.
 *
 * c-torture: compile/pr99787-1.c, execute/20070614-1.c.  Compiled with
 * host gcc (make test) and zerocc -O0..-O3 (make test-opt); expected
 * exit code 0. */
#include <stdio.h>

_Complex fa;                 /* standalone -> complex double (16) */
_Complex float ff;           /* explicit base -> complex float (8) */

void f (_Complex z) { (void) z; }
_Complex bar (_Complex z) { return z; }
void g (void) { _Complex l; (void) l; }

int main (void)
{
    if (sizeof (fa) != 16)
        return 1;
    if (sizeof (ff) != 8)
        return 2;

    fa = 1.0 + 2.0i;
    if (__real__ (fa) != 1.0 || __imag__ (fa) != 2.0)
        return 3;

    if (bar (3.0 + 4.0i) != (3.0 + 4.0i))
        return 4;

    f (0);
    g ();
    return 0;
}
