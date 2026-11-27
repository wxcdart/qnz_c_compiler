/* test/jit/rv32_varargs.c - RV32 variadic functions verification (Sprint 262) */
#include <stdarg.h>

#if defined(__GNUC__) && !defined(__zerocc__)
#define NOINLINE __attribute__((noinline))
#else
#define NOINLINE
#endif

/* 1. Sum up to 10 integer arguments (spans registers a0..a7 and caller stack) */
NOINLINE int sum_ints(int count, ...) {
    va_list ap;
    va_start(ap, count);
    int total = 0;
    for (int i = 0; i < count; i++) {
        total += va_arg(ap, int);
    }
    va_end(ap);
    return total;
}

/* 2. Mixed 32-bit and 64-bit variadic arguments with ILP32 alignment */
NOINLINE long long sum_mixed(int a, long long b, ...) {
    va_list ap;
    va_start(ap, b);
    int c = va_arg(ap, int);
    long long d = va_arg(ap, long long);
    int e = va_arg(ap, int);
    va_end(ap);
    return (long long)a + b + (long long)c + d + (long long)e;
}

/* 3. Pointer and string variadic arguments */
NOINLINE int count_chars(int n, ...) {
    va_list ap;
    va_start(ap, n);
    int total = 0;
    for (int i = 0; i < n; i++) {
        const char *s = va_arg(ap, const char *);
        while (*s++) total++;
    }
    va_end(ap);
    return total;
}

int main(void) {
    /* Test 1: In-register args only (4 args) */
    int s1 = sum_ints(4, 10, 20, 30, 40);
    if (s1 != 100) return 1;

    /* Test 2: In-register + stack-passed args (10 args: 7 in GPRs, 3 on stack) */
    int s2 = sum_ints(10, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10);
    if (s2 != 55) return 2;

    /* Test 3: Mixed int and long long */
    long long s3 = sum_mixed(10, 20LL, 30, 40LL, 50);
    if (s3 != 150LL) return 3;

    /* Test 4: String pointers */
    int s4 = count_chars(3, "abc", "defg", "h");
    if (s4 != 8) return 4;

    return 0;
}
