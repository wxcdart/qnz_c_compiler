/* C23 constexpr functions (ZeroCC extension beyond C23 6.7.2.4).
 *
 * C23 only allows constexpr on data declarations. ZeroCC additionally
 * accepts constexpr on functions and evaluates them at compile time
 * when called in a constant-expression context (CTFE).
 *
 * This file is only meaningful under ZeroCC; host gcc rejects
 * constexpr on functions in C23 mode, so it is skipped under gcc
 * via the __ZEROCC__ guard used by the test harness.
 */

#ifndef __ZEROCC__
int main(void) { return 0; }
#else

constexpr int f_int(int x) { return x + 1; }
constexpr int f_add(int a, int b) { return a + b; }
constexpr int f_mul(int a, int b) { return a * b; }

/* CTFE in a constant-expression initializer */
constexpr int r1 = f_int(41);          /* 42 */
constexpr int r2 = f_add(10, 20);      /* 30 */
constexpr int r3 = f_mul(3, f_int(4)); /* 3 * 5 = 15 */
constexpr int r4 = f_add(f_mul(2, 3), f_mul(4, 5)); /* 6 + 20 = 26 */

/* Runtime call (not constant context) */
int g(void) { return f_int(100); }     /* 101 */

int main(void) {
    if (r1 != 42) return 1;
    if (r2 != 30) return 2;
    if (r3 != 15) return 3;
    if (r4 != 26) return 4;
    if (g() != 101) return 5;
    return 0;
}
#endif
