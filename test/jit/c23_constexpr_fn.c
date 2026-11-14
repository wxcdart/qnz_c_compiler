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

/* Floating-point CTFE */
constexpr float f_abs(float x) {
    if (x < 0.0f) return -x;
    return x;
}
constexpr float rf1 = f_abs(-3.5f);     /* 3.5f */
constexpr float rf2 = f_abs(2.25f);     /* 2.25f */

constexpr double d_mul_add(double a, double b, double c) {
    return a * b + c;
}
constexpr double rd1 = d_mul_add(2.5, 4.0, 1.5); /* 10.0 + 1.5 = 11.5 */

constexpr float f_sum_loop(int n) {
    float s = 0.0f;
    for (int i = 1; i <= n; i = i + 1) {
        s = s + 0.5f;
    }
    return s;
}
constexpr float rf3 = f_sum_loop(4);    /* 2.0f */

/* Runtime call (not constant context) */
int g(void) { return f_int(100); }     /* 101 */

/* --- Pointer, String Literal & Array CTFE --- */

/* 1. String literal indexing and pointer traversal (constexpr strlen) */
constexpr int cx_strlen(const char *s) {
    int n = 0;
    while (s[n] != '\0') {
        n++;
    }
    return n;
}
constexpr int slen1 = cx_strlen("hello, world!"); /* 13 */
constexpr int slen2 = cx_strlen("");              /* 0 */

/* 2. Pointer dereference and pointer increment (*p, p++) */
constexpr int cx_strlen_ptr(const char *p) {
    const char *start = p;
    while (*p) {
        p++;
    }
    return (int)(p - start);
}
constexpr int slen3 = cx_strlen_ptr("constexpr"); /* 9 */

/* 3. String comparison via pointers (constexpr strcmp) */
constexpr int cx_strcmp(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}
constexpr int scmp_eq = cx_strcmp("alpha", "alpha"); /* 0 */
constexpr int scmp_lt = cx_strcmp("abc", "abd");     /* < 0 */
constexpr int scmp_gt = cx_strcmp("abe", "abd");     /* > 0 */

/* 4. Global array decay & indexing in CTFE (constexpr sum) */
constexpr int g_primes[6] = {2, 3, 5, 7, 11, 13};

constexpr int cx_sum_arr(const int *arr, int n) {
    int total = 0;
    for (int i = 0; i < n; i++) {
        total += arr[i];
    }
    return total;
}
constexpr int prime_sum = cx_sum_arr(g_primes, 6); /* 41 */
constexpr int prime_tail_sum = cx_sum_arr(g_primes + 2, 4); /* 5+7+11+13 = 36 */

/* 5. Compound literal array passed to constexpr function */
constexpr int clit_sum = cx_sum_arr((const int[]){10, 20, 30, 40}, 4); /* 100 */

/* 6. Local array inside constexpr function body + mutation via subscript & pointers */
constexpr int cx_fib_table(int n) {
    int fib[12] = {0, 1};
    for (int i = 2; i <= n; i++) {
        fib[i] = fib[i - 1] + fib[i - 2];
    }
    return fib[n];
}
constexpr int fib10 = cx_fib_table(10); /* 55 */

/* 7. Pointer out-parameter mutation (&x, *p = ...) between nested constexpr calls */
constexpr void cx_swap(int *a, int *b) {
    int tmp = *a;
    *a = *b;
    *b = tmp;
}

constexpr int cx_reverse_dot(void) {
    int v[4] = {1, 2, 3, 4};
    cx_swap(&v[0], &v[3]);
    cx_swap(&v[1], &v[2]);
    /* Now v is {4, 3, 2, 1}: compute 4*1 + 3*2 + 2*3 + 1*4 = 20 */
    return v[0] * 1 + v[1] * 2 + v[2] * 3 + v[3] * 4;
}
constexpr int rev_dot = cx_reverse_dot(); /* 20 */

/* 8. constexpr function returning a pointer (strchr) */
constexpr const char *cx_strchr(const char *s, int ch) {
    while (*s) {
        if (*s == ch) return s;
        s++;
    }
    return nullptr;
}
constexpr int cx_strchr_idx(const char *s, int ch) {
    const char *p = cx_strchr(s, ch);
    return p ? (int)(p - s) : -1;
}
constexpr int found_idx = cx_strchr_idx("abcdef", 'd');          /* 3 */
constexpr int found_ch  = *cx_strchr("abcdef", 'd');             /* 'd' */
constexpr int found_nxt = cx_strchr("abcdef", 'd')[1];           /* 'e' */
constexpr int not_found = (cx_strchr("abcdef", 'z') == nullptr); /* 1 */

/* 9. Array dimension sized by pointer-based constexpr function */
int sized_by_strlen[cx_strlen("c23_ctfe")]; /* 8 ints = 32 bytes */

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
    if (rf1 != 3.5f) return 13;
    if (rf2 != 2.25f) return 14;
    if (rd1 != 11.5) return 15;
    if (rf3 != 2.0f) return 16;
    if (slen1 != 13) return 17;
    if (slen2 != 0) return 18;
    if (slen3 != 9) return 19;
    if (scmp_eq != 0) return 20;
    if (!(scmp_lt < 0)) return 21;
    if (!(scmp_gt > 0)) return 22;
    if (prime_sum != 41) return 23;
    if (prime_tail_sum != 36) return 24;
    if (clit_sum != 100) return 25;
    if (fib10 != 55) return 26;
    if (rev_dot != 20) return 27;
    if (found_idx != 3) return 28;
    if (found_ch != 'd') return 31;
    if (found_nxt != 'e') return 32;
    if (not_found != 1) return 29;
    if (sizeof(sized_by_strlen) != 8 * sizeof(int)) return 30;
    return 0;
}
#endif
