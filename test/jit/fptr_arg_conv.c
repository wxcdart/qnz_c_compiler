/* Sprint 100: an indirect call through a function pointer must marshal
 * arguments against the POINTEE's declared prototype, not the argument
 * expression's current value class.  Without the pointee signature the
 * int->double conversion is lost and `f(33, 11)` passes 33 in a GPR
 * instead of a double in XMM0.  Regression for c-torture
 * execute/930702-1.  Compiled with zerocc (AOT + JIT) and host gcc;
 * expected exit code 0. */
#include <math.h>
#include <stdlib.h>

static int got_d = 0, got_i = 0;

static int callee (double d, int i)
{
    if (isnan (d))
        return 0;
    got_d = (int) d;
    got_i = i;
    return (int) d + i;
}

int main (void)
{
    int (*f) (double, int) = callee;
    int r1 = f (33, 11);          /* int must be converted to double */
    int r2 = callee (33.0, 11);
    if (r1 != 44 || r2 != 44)
        return 1;
    if (got_d != 33 || got_i != 11)
        return 2;
    return 0;
}
