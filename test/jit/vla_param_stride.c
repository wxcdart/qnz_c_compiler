/* Sprint 100: two VLA corner cases.
 *
 * 1. VLA parameter row stride: `char a[2][N]` / `int a[2][N]` - the
 *    row size N is only known at the call site (the global N changes
 *    between the declaration and the call).  The callee must scale the
 *    first index by the RUNTIME row size (dim * leaf element size),
 *    not by a definition-time constant or an uninitialised slot.
 *    Regression for c-torture execute/pr22061-1.
 *
 * 2. Mirror-form subscript: `N[(char*)x]` (C a[b] == b[a]) - the
 *    pointer operand is the INDEX.  The element size comes from the
 *    pointer's pointee (1 byte), so the integer base is added
 *    unscaled.  Without the annotation the pointer is scaled by the
 *    int's sizeof and the dereference goes wild.  Same test as
 *    c-torture pr22061-1's `N[(char*)x] != N` check.
 *
 * Compiled with zerocc (AOT + JIT) and host gcc; expected exit code 0. */
#include <string.h>
#include <stdlib.h>

static int N = 1;

void bar (char a[2][N]) { a[1][0] = N; }
void bar_int (int a[2][N]) { a[1][0] = N; }

int main (void)
{
    N = 4;

    char x[16];
    memset (x, 0, sizeof x);
    bar (x);
    if (N[(char *) x] != N)
        return 1;                      /* x[4] must hold 4 */

    int y[16];
    for (int i = 0; i < 16; i++)
        y[i] = i;
    bar_int (y);
    if (N[(int *) y] != N)
        return 2;                      /* y[4] must hold 4 */

    /* plain VLA-parameter write through a normal subscript too */
    char w[16];
    memset (w, 0, sizeof w);
    bar (w);
    if (w[4] != 4 || w[0] != 0 || w[1] != 0)
        return 3;

    return 0;
}
