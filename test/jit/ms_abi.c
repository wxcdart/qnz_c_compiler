
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>

#if defined(__GNUC__) && !defined(__ZEROCC__)
typedef __builtin_ms_va_list zc_va_list;
#define zc_va_start(ap, last) __builtin_ms_va_start(ap, last)
#define zc_va_arg(ap, type)   __builtin_va_arg(ap, type)
#define zc_va_end(ap)         __builtin_ms_va_end(ap)
#else
typedef va_list zc_va_list;
#define zc_va_start(ap, last) va_start(ap, last)
#define zc_va_arg(ap, type)   va_arg(ap, type)
#define zc_va_end(ap)         va_end(ap)
#endif

#define MS_ABI __attribute__((ms_abi))

/* Test 1: 4 integer arguments (RCX, RDX, R8, R9) */
MS_ABI int add4(int a, int b, int c, int d) {
    return a + b * 2 + c * 3 + d * 4;
}

/* Test 2: FP arguments in XMM0..XMM3 */
MS_ABI double addfp(double a, double b, double c, double d) {
    return a + b * 2.0 + c * 3.0 + d * 4.0;
}

/* Test 3: Interleaved int and fp in unified slots 0..3 */
MS_ABI double interleave(int a, double b, int c, double d) {
    return (double)a + b + (double)c + d;
}

/* Test 4: Stack arguments (slots 4+) */
MS_ABI int add8(int a, int b, int c, int d, int e, int f, int g, int h) {
    return a + b + c + d + e + f + g + h;
}

/* Test 5: Structs */
typedef struct {
    int x;
    int y;
} SmallStruct; /* 8 bytes -> returns in RAX */

typedef struct {
    int a, b, c;
} MediumStruct; /* 12 bytes -> uses sret */

MS_ABI SmallStruct make_small(int x, int y) {
    SmallStruct s;
    s.x = x;
    s.y = y;
    return s;
}

MS_ABI MediumStruct make_medium(int a, int b, int c) {
    MediumStruct m;
    m.a = a;
    m.b = b;
    m.c = c;
    return m;
}

MS_ABI int take_medium(MediumStruct m) {
    return m.a + m.b + m.c;
}

/* Test 6: Variadic */
MS_ABI int ms_sum(int count, ...) {
    zc_va_list ap;
    zc_va_start(ap, count);
    int total = 0;
    for (int i = 0; i < count; i++) {
        total += zc_va_arg(ap, int);
    }
    zc_va_end(ap);
    return total;
}

/* Test 7: Callee-saved rsi/rdi preservation check */
MS_ABI int clobber_caller_regs(int x) {
    /* Touch rsi and rdi inside ms_abi function */
    __asm__ __volatile__("movq $0x11223344, %%rsi; movq $0x55667788, %%rdi" ::: "rsi", "rdi");
    return x * 2;
}

int main(void) {
    /* 1 */
    int r1 = add4(1, 2, 3, 4);
    if (r1 != 1 + 4 + 9 + 16) {
        printf("Test 1 failed: %d\n", r1);
        return 1;
    }

    /* 2 */
    double r2 = addfp(1.0, 2.0, 3.0, 4.0);
    if (r2 != 1.0 + 4.0 + 9.0 + 16.0) {
        printf("Test 2 failed: %f\n", r2);
        return 2;
    }

    /* 3 */
    double r3 = interleave(10, 20.0, 30, 40.0);
    if (r3 != 100.0) {
        printf("Test 3 failed: %f\n", r3);
        return 3;
    }

    /* 4 */
    int r4 = add8(1, 2, 3, 4, 5, 6, 7, 8);
    if (r4 != 36) {
        printf("Test 4 failed: %d\n", r4);
        return 4;
    }

    /* 5 */
    SmallStruct s = make_small(123, 456);
    if (s.x != 123 || s.y != 456) {
        printf("Test 5a failed: %d %d\n", s.x, s.y);
        return 5;
    }

    MediumStruct m = make_medium(10, 20, 30);
    if (m.a != 10 || m.b != 20 || m.c != 30) {
        printf("Test 5b failed: %d %d %d\n", m.a, m.b, m.c);
        return 6;
    }

    int tm = take_medium(m);
    if (tm != 60) {
        printf("Test 5c failed: %d\n", tm);
        return 7;
    }

    /* 6 */
    int r6 = ms_sum(4, 10, 20, 30, 40);
    if (r6 != 100) {
        printf("Test 6 failed: %d\n", r6);
        return 8;
    }

    /* 7: function pointer call */
    int (MS_ABI *pfn)(int, int, int, int) = add4;
    int r7 = pfn(10, 20, 30, 40);
    if (r7 != 10 + 40 + 90 + 160) {
        printf("Test 7 failed: %d\n", r7);
        return 9;
    }

    /* 8: rsi/rdi preservation inside ms_abi function */
    int r8 = clobber_caller_regs(21);
    if (r8 != 42) {
        printf("Test 8 failed: %d\n", r8);
        return 10;
    }

    printf("ALL MS_ABI TESTS PASSED!\n");
    return 0;
}
