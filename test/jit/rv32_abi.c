#include <stdio.h>

/* Phase 2: RV32 ILP32 psABI 64-bit integer calling convention tests */

/* 1. Basic 64-bit argument and return */
long long add64(long long a, long long b) {
    return a + b;
}

unsigned long long sub64_u(unsigned long long a, unsigned long long b) {
    return a - b;
}

/* 2. Even-odd register alignment:
 * a0: int a
 * a1: skipped (odd)
 * a2:a3: long long b
 * a4: int c
 * a5: skipped (odd)
 * a6:a7: long long d
 */
long long mixed_regs(int a, long long b, int c, long long d) {
    return (long long)a + b + (long long)c + d;
}

/* 3. Register exhaustion and stack passing:
 * a0..a6: 7 ints
 * a7: only 1 register remains! psABI requires 2*XLEN scalar to be passed
 *     on the stack if fewer than 2 registers remain.
 * Stack slot 0:1: long long h
 * Stack slot 2: int i
 * Stack slot 3: skipped (odd slot skipped for 8-byte alignment)
 * Stack slot 4:5: long long j
 */
long long many_args(int a, int b, int c, int d, int e, int f, int g,
                    long long h, int i, long long j) {
    return (long long)a + b + c + d + e + f + g + h + i + j;
}

/* 4. Function taking struct or multiple 64-bit arguments crossing register window */
long long six_i64(long long a, long long b, long long c, long long d, long long e) {
    /* a in a0:a1
     * b in a2:a3
     * c in a4:a5
     * d in a6:a7
     * e on stack (slot 0:1)
     */
    return a + b + c + d + e;
}

int main(void) {
    /* Test 1: basic 64-bit add/sub */
    long long r1 = add64(0x100000002LL, 0x200000003LL);
    if (r1 != 0x300000005LL) {
        printf("FAIL: test 1 add64 expected 0x300000005, got %llx\n", r1);
        return 1;
    }

    unsigned long long r2 = sub64_u(0x500000007ULL, 0x200000003ULL);
    if (r2 != 0x300000004ULL) {
        printf("FAIL: test 2 sub64_u expected 0x300000004, got %llx\n", r2);
        return 2;
    }

    /* Test 2: even-odd alignment */
    long long r3 = mixed_regs(10, 0x100000000LL, 20, 0x200000000LL);
    if (r3 != 0x30000001ELL) { /* 10 + 20 = 30 = 0x1E */
        printf("FAIL: test 3 mixed_regs expected 0x30000001E, got %llx\n", r3);
        return 3;
    }

    /* Test 3: stack passing with odd register skipped / stack alignment */
    long long r4 = many_args(1, 2, 3, 4, 5, 6, 7,
                             0x100000000LL, 10, 0x200000000LL);
    /* 1+2+3+4+5+6+7+10 = 38 = 0x26; 0x100000000 + 0x200000000 = 0x300000000 */
    if (r4 != 0x300000026LL) {
        printf("FAIL: test 4 many_args expected 0x300000026, got %llx\n", r4);
        return 4;
    }

    /* Test 4: 5 x 64-bit values (4 in regs, 1 on stack) */
    long long r5 = six_i64(1LL, 2LL, 3LL, 4LL, 0x100000000LL);
    if (r5 != 0x10000000ALL) {
        printf("FAIL: test 5 six_i64 expected 0x10000000A, got %llx\n", r5);
        return 5;
    }

    /* Test 5: variadic forwarding (printf %lld) */
    printf("OK: rv32 i64 ABI: %lld %llu %d\n", r1, r2, 42);
    return 0;
}
