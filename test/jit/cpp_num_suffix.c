/* Sprint 100: numeric literals are atomic preprocessing tokens.  A
 * macro whose name matches a float/integer suffix letter must NOT be
 * expanded inside a pp-number: with `#define F 140`, `0.0F` must stay
 * 0.0F and must not become the text `0.0140` (which silently re-scales
 * every float literal in the file).  Regression for c-torture
 * execute/cmpsf-1.  Compiled with zerocc (AOT + JIT) and host gcc;
 * expected exit code 0. */
#include <limits.h>

#define F 140
#define T 13

static int feq (float x, float y)
{
    if (x == y)
        return T;
    return F;
}

int main (void)
{
    float args[] = { 0.0F, 1.0F, 2.0F, 3.0F, 4.0F };

    /* The macro must still expand in identifier position. */
    if (feq (0.0F, 0.0F) != T)
        return 1;
    if (feq (0.0F, 1.0F) != F)
        return 2;

    /* The initialisers must hold the literal values, not macro
     * mangled ones (0.0F -> 0.0, 1.0F -> 1.0, ...). */
    if (args[0] != 0.0F)
        return 3;
    if (args[1] != 1.0F)
        return 4;
    if (args[2] != 2.0F)
        return 5;
    if (args[3] != 3.0F)
        return 6;
    if (args[4] != 4.0F)
        return 7;

    /* hex digits / integer suffixes are pp-number letters too */
    if (0x1F != 31)
        return 8;
    if (100UL != 100UL)
        return 9;

    return 0;
}
