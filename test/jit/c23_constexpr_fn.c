/* C23 constexpr functions (ZeroCC extension beyond C23 6.7.2.4).
 *
 * C23 only allows constexpr on data declarations. ZeroCC additionally
 * accepts constexpr on functions and evaluates them at compile time
 * when called in a constant-expression context (CTFE).
 *
 * CTFE supports single-expression return bodies as well as if/else,
 * while, and for loops with local variable declarations and
 * assignments.
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

/* if/else in CTFE body */
constexpr int abs_val(int x) {
    if (x < 0) return -x;
    return x;
}
constexpr int r5 = abs_val(-42);       /* 42 */
constexpr int r6 = abs_val(42);        /* 42 */

/* while loop in CTFE body */
constexpr int count_to(int n) {
    int i = 0;
    while (i < n) {
        i = i + 1;
    }
    return i;
}
constexpr int r7 = count_to(10);       /* 10 */

/* for loop in CTFE body */
constexpr int sum_n(int n) {
    int s = 0;
    for (int i = 0; i < n; i = i + 1) {
        s = s + i;
    }
    return s;
}
constexpr int r8 = sum_n(10);          /* 0+1+...+9 = 45 */

/* Nested if/else with while */
constexpr int clamp(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}
constexpr int r9 = clamp(5, 10, 20);   /* 10 */
constexpr int r10 = clamp(15, 10, 20); /* 15 */
constexpr int r11 = clamp(25, 10, 20); /* 20 */

/* Runtime call (not constant context) */
int g(void) { return f_int(100); }     /* 101 */

int main(void) {
    if (r1 != 42) return 1;
    if (r2 != 30) return 2;
    if (r3 != 15) return 3;
    if (r4 != 26) return 4;
    if (r5 != 42) return 5;
    if (r6 != 42) return 6;
    if (r7 != 10) return 7;
    if (r8 != 45) return 8;
    if (r9 != 10) return 9;
    if (r10 != 15) return 10;
    if (r11 != 20) return 11;
    if (g() != 101) return 12;
    return 0;
}
#endif
