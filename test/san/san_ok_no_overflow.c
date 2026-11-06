/* zero_san v1 soundness: valid C must NEVER trap under -fsanitize=undefined.
 *
 * Covers the traced invariants:
 *  - mixed-width usual arithmetic conversions (int+long, long-int, int*long):
 *    the op computes in the WIDER type; a check at the narrower width would
 *    false-trap (regression: the original v1 used the left operand's width).
 *  - unsigned wrap (32/64-bit) and equal-width signed+unsigned: defined
 *    wrap, never checked.
 *  - 64-bit (width-8) results beyond the 16-bit range: the check must not
 *    truncate the value (regression: the width-extension post-pass mapped
 *    width 8 -> `width & 7` == 0 -> 16-bit zero-extend, corrupting the sum).
 *  - loops (LICM must not hoist the check), CSE-shaped duplicate adds,
 *    constant-folded adds.
 *
 * Exit 0 = sound; any nonzero rc or SIGILL = false-positive regression. */

long f_mixed(int i, long l) { return i + l; }
long g_mixed(long l, int i) { return l - i; }
long h_mixed(int i, long l) { return i * l; }
unsigned uwrap(unsigned a, unsigned b) { return a + b; }
unsigned long ulwrap(unsigned long a, unsigned long b) { return a + b; }
unsigned mixed_eq(int i, unsigned u) { return u + i; }
long f64(long a, long b) { long x = a + b; return x; }
long f64loop(int n) { long s = 0; for (int i = 0; i < n; i++) s = s + 12345L; return s; }
long cse_dup(long a, long b) { long x = a + b; long y = a + b; return x + y; }
int cfold(void) { int x = 2 + 3; long y = 100L + 200L; return (x == 5 && y == 300L) ? 0 : 1; }

int main(void) {
    if (f_mixed(2000000000, 2000000000L) != 4000000000L) return 1;
    if (g_mixed(2000000000L, 1) != 1999999999L) return 2;
    if (h_mixed(2000000000, 2) != 4000000000L) return 3;
    if (uwrap(0xFFFFFFFFu, 10u) != 9u) return 4;
    if (ulwrap(0xFFFFFFFFFFFFFFF0UL, 100UL) != 84UL) return 5;
    if (mixed_eq(-5, 0xFFFFFFFFu) != 0xFFFFFFFAu) return 6;
    if (f64(100000L, 200000L) != 300000L) return 7;
    if (f64(-100000L, -200000L) != -300000L) return 8;
    if (f64loop(10) != 123450L) return 9;
    if (cse_dup(50000L, 30000L) != 160000L) return 10;
    if (cfold() != 0) return 11;
    return 0;
}
