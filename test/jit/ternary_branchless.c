/* Ternary / conditional-select coverage (Sprint 39: branchless pass, -O3).
 *
 * Exercises the shapes the branchless-select transform matches (and the
 * guards that must reject): int/long/char/pointer arms, SEXT/ZEXT arms,
 * float arms (must stay branched - XMM guard), a condition reused after
 * the select, nested ternaries, and values that must not be clobbered.
 *
 * Verified by tools/opt_sweep.py against the -O0 baseline at every -O
 * level (JIT + AOT); gcc -O2 is the informational reference.
 */
#include <stdio.h>

int pick_int(int c, int a, int b) { return c ? a : b; }
long pick_long(long c, long a, long b) { return c ? a : b; }
char pick_char(char c, char a, char b) { return c ? a : b; }
int *pick_ptr(int c, int *a, int *b) { return c ? a : b; }
int pick_sext(int c, int a, int b) { return (c ? a : b) + (c ? 100 : -100); }
/* NOTE: float/double ternaries are NOT exercised here: zerocc's -O0
 * lowering of a float conditional expression is broken (the result temp
 * is allocated GPR-class while the arms FLOAD into XMM; the trailing
 * (int) cast lowers to SEXT32 instead of CVTTSS2SI).  Tracked as a
 * pre-existing frontend/lowering bug, independent of the branchless
 * pass, which rejects FP arms via its provably-GPR guard. */
int pick_nested(int c1, int c2, int a, int b, int d) {
    return c1 ? (c2 ? a : b) : d;
}
int pick_cond_reuse(int c, int a, int b) {
    int r = c ? a : b;
    return r + (c ? 1 : 2);   /* condition still live after the select */
}
int pick_both(const int *p, int n, int c) {
    int lo = 0, hi = 0;
    int i;
    for (i = 0; i < n; i++) {
        int v = p[i];
        lo = v < lo ? v : lo;
        hi = v > hi ? v : hi;
    }
    return c ? hi - lo : lo - hi;
}

int main(void) {
    int data[8] = {5, -2, 9, 0, -7, 3, 8, 1};
    int r = 0;
    int i;

    if (pick_int(1, 7, 8) != 7) r |= 1;
    if (pick_int(0, 7, 8) != 8) r |= 2;
    if (pick_int(1, -3, 8) != -3) r |= 4;
    if (pick_long(1, 10000000000L, -10000000000L) != 10000000000L) r |= 8;
    if (pick_long(0, 10000000000L, -10000000000L) != -10000000000L) r |= 16;
    if (pick_char(1, 'x', 'y') != 'x') r |= 32;
    if (pick_char(0, 'x', 'y') != 'y') r |= 64;

    int x = 1, y = 2;
    if (pick_ptr(1, &x, &y) != &x) r |= 128;
    if (pick_ptr(0, &x, &y) != &y) r |= 256;

    if (pick_sext(1, 5, 9) != 105) r |= 512;
    if (pick_sext(0, 5, 9) != -91) r |= 1024;

    if (pick_nested(1, 1, 1, 2, 3) != 1) r |= 32768;
    if (pick_nested(1, 0, 1, 2, 3) != 2) r |= 65536;
    if (pick_nested(0, 1, 1, 2, 3) != 3) r |= 131072;

    if (pick_cond_reuse(1, 10, 20) != 11) r |= 262144;
    if (pick_cond_reuse(0, 10, 20) != 22) r |= 524288;

    if (pick_both(data, 8, 1) != 16) r |= 1048576;   /* hi - lo = 9 - (-7) */
    if (pick_both(data, 8, 0) != -16) r |= 2097152;  /* lo - hi = -7 - 9  */

    for (i = 0; i < 1000; i++) {
        int c = (i % 3) == 0;
        int v = pick_int(c, i, i * 2);
        int expect = c ? i : i * 2;
        if (v != expect) { r |= 4194304; break; }
    }

    if (r != 0) {
        printf("ternary_branchless FAILED: r=0x%x\n", r);
        return 1;
    }
    printf("ternary_branchless passed\n");
    return 0;
}
