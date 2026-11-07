/* Sprint 122 Stage 1: the parenthesized declarator path must capture a
 * VLA parameter's runtime dimension expression so that `sizeof` on the
 * VLA param yields the runtime size.
 *
 * `int (*bar(char a[2][N]))(int)` is a function `bar` that takes a VLA
 * param `char a[2][N]` (2 rows of N cols, N known only at the call site)
 * and returns a function pointer `int (*)(int)`.  `sizeof(a[0])` is
 * `sizeof(char[N]) == N * sizeof(char)` - a runtime value that the
 * compiler fills from the per-param runtime size slot, which is set up
 * at function entry from `fn->param_dim_exprs` (the parsed dimension
 * expressions).
 *
 * Before Sprint 122 the declarator-stack path dropped the dimension
 * expression: it lives on the param NODE_DECL's `right` field in a
 * temporary arena that is freed at the end of the declarator parse, and
 * the parenthesized branch never copied it into the surviving `dim_arena`
 * that the function record owns.  So `sizeof(a[0])` came out as a
 * definition-time constant instead of the runtime N.  Stage 1 clones the
 * dim expr into `dim_arena`, matching the legacy plain-name path; Stage 2
 * routes the plain-name path through the same declarator stack.
 *
 * The argument is cast to `(char (*)[N])` because a VLA param's decay
 * type is runtime-dependent and the declarator path type-checks the call
 * strictly (the legacy plain-name path is lenient about the
 * `char *` -> `char (*)[N]` mismatch).
 *
 * Regression: without the fix, `sizeof(a[0])` != runtime N.
 * Compiled with zerocc (AOT + JIT) and host gcc; expected exit code 0.
 */
#include <string.h>

static int N = 1;

static int helper(int x) { return x; }

static int sz = 0;

int (*bar (char a[2][N]))(int)
{
    sz = (int)sizeof(a[0]);       /* runtime row size == N */
    return helper;
}

int main(void)
{
    N = 5;

    char x[16];
    memset(x, 0, sizeof x);
    int (*f)(int) = bar((char (*)[N])x);
    if (sz != 5)
        return 1;                      /* sizeof(a[0]) must be runtime 5 */
    if (f(3) != 3)
        return 2;                      /* returned fnptr must work */

    return 0;
}
