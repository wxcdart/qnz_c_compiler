/* Designated-initializer sizing of unsized arrays (C99 6.7.8p12 / C23 6.7.10).
 *
 * `T a[] = { [N] = ... }` must infer the bound from the MAXIMUM designator
 * index (N+1), not from the number of initializer elements. The implicit
 * elements are zero-initialized. Before the fix (Sprint 132) zerocc counted
 * elements (2) and rejected `[3]` with "array initializer index 3 exceeds
 * array bound 2". gcc-15 (all standards) infers 4.
 *
 * Runs under make test (zerocc JIT) and the test-opt sweep (JIT + AOT at
 * -O0..-O3); also host-gcc-compiled by make all.
 */
#include <stdio.h>

struct S { int x, y; };

/* file scope: bound = max designator index + 1, not element count */
int fa[] = { [3] = 7, [0] = 1 };
char fc[] = { [3] = 'x', [1] = 'b' };
struct S fs[] = { [3] = { 5, 6 }, [0] = { 9, 8 } };
/* range designator: [1 ... 4] spans indices 1..4 -> bound 5 */
int fr[] = { [1 ... 4] = 0, [0] = 9 };
/* plain (non-designated) inference must be unchanged */
int fp[] = { 1, 2, 3 };
int pta[] = { 1, 2, }; /* trailing comma does not add an element */

int main(void)
{
    int fails = 0;

    /* local scope: same rule */
    int la[] = { [3] = 7, [0] = 1 };
    if (sizeof(la) != 4 * sizeof(int)) { printf("local sizeof = %zu\n", sizeof(la)); fails++; }
    if (la[0] != 1 || la[3] != 7 || la[1] != 0 || la[2] != 0) { printf("local values\n"); fails++; }

    if (sizeof(fa) != 4 * sizeof(int)) { printf("sizeof(fa) = %zu\n", sizeof(fa)); fails++; }
    if (fa[0] != 1 || fa[3] != 7 || fa[1] != 0) { printf("fa values\n"); fails++; }

    if (sizeof(fc) != 4) { printf("sizeof(fc) = %zu\n", sizeof(fc)); fails++; }
    if (fc[0] != 0 || fc[1] != 'b' || fc[3] != 'x') { printf("fc values\n"); fails++; }

    if (sizeof(fs) != 4 * sizeof(struct S)) { printf("sizeof(fs) = %zu\n", sizeof(fs)); fails++; }
    if (fs[0].x != 9 || fs[0].y != 8) { printf("fs[0]\n"); fails++; }
    if (fs[3].x != 5 || fs[3].y != 6) { printf("fs[3]\n"); fails++; }
    /* implicit middle rows are zero-initialized */
    if (fs[1].x != 0 || fs[1].y != 0 || fs[2].x != 0 || fs[2].y != 0) { printf("fs middle rows\n"); fails++; }

    if (sizeof(fr) != 5 * sizeof(int)) { printf("sizeof(fr) = %zu\n", sizeof(fr)); fails++; }
    if (fr[0] != 9 || fr[1] != 0 || fr[4] != 0) { printf("fr values\n"); fails++; }

    if (sizeof(fp) != 3 * sizeof(int)) { printf("sizeof(fp) = %zu\n", sizeof(fp)); fails++; }
    if (sizeof(pta) != 2 * sizeof(int)) { printf("sizeof(pta) = %zu\n", sizeof(pta)); fails++; }

    if (fails) { printf("designator_array_size: %d FAIL\n", fails); return 1; }
    printf("designator_array_size: OK\n");
    return 0;
}
