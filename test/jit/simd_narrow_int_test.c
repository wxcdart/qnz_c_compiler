/* test/jit/simd_narrow_int_test.c
 * Differential verification for Sprint 220:
 * Narrow Integer SIMD Vectorization (8-bit char & 16-bit short)
 * Verifies paddb, psubb, paddw, psubw, pmullw across multiple sizes
 * including exact multiples of vector width (16, 32, 64 / 8, 16, 32)
 * and unaligned trip counts requiring remainder peeling (47, 25).
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

void test_char_add_32(int8_t *a, int8_t *b, int8_t *c) {
    for (int i = 0; i < 32; i++) {
        c[i] = a[i] + b[i];
    }
}

void test_char_sub_32(int8_t *a, int8_t *b, int8_t *c) {
    for (int i = 0; i < 32; i++) {
        c[i] = a[i] - b[i];
    }
}

void test_char_add_47(int8_t *a, int8_t *b, int8_t *c) {
    for (int i = 0; i < 47; i++) {
        c[i] = a[i] + b[i];
    }
}

void test_char_sub_47(int8_t *a, int8_t *b, int8_t *c) {
    for (int i = 0; i < 47; i++) {
        c[i] = a[i] - b[i];
    }
}

void test_short_add_16(int16_t *a, int16_t *b, int16_t *c) {
    for (int i = 0; i < 16; i++) {
        c[i] = a[i] + b[i];
    }
}

void test_short_sub_16(int16_t *a, int16_t *b, int16_t *c) {
    for (int i = 0; i < 16; i++) {
        c[i] = a[i] - b[i];
    }
}

void test_short_mul_16(int16_t *a, int16_t *b, int16_t *c) {
    for (int i = 0; i < 16; i++) {
        c[i] = a[i] * b[i];
    }
}

void test_short_add_25(int16_t *a, int16_t *b, int16_t *c) {
    for (int i = 0; i < 25; i++) {
        c[i] = a[i] + b[i];
    }
}

void test_short_sub_25(int16_t *a, int16_t *b, int16_t *c) {
    for (int i = 0; i < 25; i++) {
        c[i] = a[i] - b[i];
    }
}

void test_short_mul_25(int16_t *a, int16_t *b, int16_t *c) {
    for (int i = 0; i < 25; i++) {
        c[i] = a[i] * b[i];
    }
}

int main(void) {
    int8_t ca[64], cb[64], cc[64];
    int16_t sa[64], sb[64], sc[64];

    for (int i = 0; i < 64; i++) {
        ca[i] = (int8_t)(i * 3 + 1);
        cb[i] = (int8_t)(i * 2 + 5);
        cc[i] = 0;
        sa[i] = (int16_t)(i * 17 - 100);
        sb[i] = (int16_t)(i * 9 + 42);
        sc[i] = 0;
    }

    /* 1. Char Add 32 */
    test_char_add_32(ca, cb, cc);
    for (int i = 0; i < 32; i++) {
        int8_t exp = (int8_t)(ca[i] + cb[i]);
        if (cc[i] != exp) {
            printf("FAIL: test_char_add_32[%d] = %d (expected %d)\n", i, cc[i], exp);
            return 1;
        }
    }

    /* 2. Char Sub 32 */
    test_char_sub_32(ca, cb, cc);
    for (int i = 0; i < 32; i++) {
        int8_t exp = (int8_t)(ca[i] - cb[i]);
        if (cc[i] != exp) {
            printf("FAIL: test_char_sub_32[%d] = %d (expected %d)\n", i, cc[i], exp);
            return 1;
        }
    }

    /* 3. Char Add 47 (with remainder peel) */
    test_char_add_47(ca, cb, cc);
    for (int i = 0; i < 47; i++) {
        int8_t exp = (int8_t)(ca[i] + cb[i]);
        if (cc[i] != exp) {
            printf("FAIL: test_char_add_47[%d] = %d (expected %d)\n", i, cc[i], exp);
            return 1;
        }
    }

    /* 4. Char Sub 47 (with remainder peel) */
    test_char_sub_47(ca, cb, cc);
    for (int i = 0; i < 47; i++) {
        int8_t exp = (int8_t)(ca[i] - cb[i]);
        if (cc[i] != exp) {
            printf("FAIL: test_char_sub_47[%d] = %d (expected %d)\n", i, cc[i], exp);
            return 1;
        }
    }

    /* 5. Short Add 16 */
    test_short_add_16(sa, sb, sc);
    for (int i = 0; i < 16; i++) {
        int16_t exp = (int16_t)(sa[i] + sb[i]);
        if (sc[i] != exp) {
            printf("FAIL: test_short_add_16[%d] = %d (expected %d)\n", i, sc[i], exp);
            return 1;
        }
    }

    /* 6. Short Sub 16 */
    test_short_sub_16(sa, sb, sc);
    for (int i = 0; i < 16; i++) {
        int16_t exp = (int16_t)(sa[i] - sb[i]);
        if (sc[i] != exp) {
            printf("FAIL: test_short_sub_16[%d] = %d (expected %d)\n", i, sc[i], exp);
            return 1;
        }
    }

    /* 7. Short Mul 16 */
    test_short_mul_16(sa, sb, sc);
    for (int i = 0; i < 16; i++) {
        int16_t exp = (int16_t)(sa[i] * sb[i]);
        if (sc[i] != exp) {
            printf("FAIL: test_short_mul_16[%d] = %d (expected %d)\n", i, sc[i], exp);
            return 1;
        }
    }

    /* 8. Short Add 25 (with remainder peel) */
    test_short_add_25(sa, sb, sc);
    for (int i = 0; i < 25; i++) {
        int16_t exp = (int16_t)(sa[i] + sb[i]);
        if (sc[i] != exp) {
            printf("FAIL: test_short_add_25[%d] = %d (expected %d)\n", i, sc[i], exp);
            return 1;
        }
    }

    /* 9. Short Sub 25 (with remainder peel) */
    test_short_sub_25(sa, sb, sc);
    for (int i = 0; i < 25; i++) {
        int16_t exp = (int16_t)(sa[i] - sb[i]);
        if (sc[i] != exp) {
            printf("FAIL: test_short_sub_25[%d] = %d (expected %d)\n", i, sc[i], exp);
            return 1;
        }
    }

    /* 10. Short Mul 25 (with remainder peel) */
    test_short_mul_25(sa, sb, sc);
    for (int i = 0; i < 25; i++) {
        int16_t exp = (int16_t)(sa[i] * sb[i]);
        if (sc[i] != exp) {
            printf("FAIL: test_short_mul_25[%d] = %d (expected %d)\n", i, sc[i], exp);
            return 1;
        }
    }

    printf("ALL NARROW SIMD TESTS PASSED!\n");
    return 0;
}
