/*
 * SIMD Loop Collapsing & Outer Loop Vectorization Test (Sprint 222)
 *
 * Verifies that perfectly nested 2D countable loops with non-vectorizable
 * inner trip counts (e.g. 2 or 3) are collapsed into single flat loops
 * and auto-vectorized across outer loop boundaries.
 */

#include <stdio.h>
#include <stdlib.h>

static void test_add2d_int(int *c, const int *a, const int *b) {
    for (int i = 0; i < 16; i++) {
        for (int j = 0; j < 3; j++) {
            c[i * 3 + j] = a[i * 3 + j] + b[i * 3 + j];
        }
    }
}

static void test_add2d_short(short *c, const short *a, const short *b) {
    for (int i = 0; i < 8; i++) {
        for (int j = 0; j < 3; j++) {
            c[i * 3 + j] = (short)(a[i * 3 + j] + b[i * 3 + j]);
        }
    }
}

static void test_sub2d_char(signed char *c, const signed char *a, const signed char *b) {
    for (int i = 0; i < 8; i++) {
        for (int j = 0; j < 2; j++) {
            c[i * 2 + j] = (signed char)(a[i * 2 + j] - b[i * 2 + j]);
        }
    }
}

static void test_mul2d_int(int *c, const int *a, const int *b) {
    for (int i = 0; i < 16; i++) {
        for (int j = 0; j < 2; j++) {
            c[i * 2 + j] = a[i * 2 + j] * b[i * 2 + j];
        }
    }
}

static void test_xor2d_uint(unsigned int *c, const unsigned int *a, const unsigned int *b) {
    for (int i = 0; i < 8; i++) {
        for (int j = 0; j < 4; j++) {
            c[i * 4 + j] = a[i * 4 + j] ^ b[i * 4 + j];
        }
    }
}

int main(void) {
    /* 1. 2D int addition (16 x 3 = 48) */
    int a_i[48], b_i[48], c_i[48];
    for (int i = 0; i < 48; i++) {
        a_i[i] = i * 2 + 1;
        b_i[i] = i * 3 + 5;
        c_i[i] = 0;
    }
    test_add2d_int(c_i, a_i, b_i);
    for (int i = 0; i < 48; i++) {
        int expected = a_i[i] + b_i[i];
        if (c_i[i] != expected) {
            printf("FAIL: add2d_int[%d] = %d (expected %d)\n", i, c_i[i], expected);
            return 1;
        }
    }

    /* 2. 2D short addition (8 x 3 = 24) */
    short a_s[24], b_s[24], c_s[24];
    for (int i = 0; i < 24; i++) {
        a_s[i] = (short)(i * 5 + 1);
        b_s[i] = (short)(i * 2 + 3);
        c_s[i] = 0;
    }
    test_add2d_short(c_s, a_s, b_s);
    for (int i = 0; i < 24; i++) {
        short expected = (short)(a_s[i] + b_s[i]);
        if (c_s[i] != expected) {
            printf("FAIL: add2d_short[%d] = %d (expected %d)\n", i, c_s[i], expected);
            return 1;
        }
    }

    /* 3. 2D signed char subtraction (8 x 2 = 16) */
    signed char a_c[16], b_c[16], c_c[16];
    for (int i = 0; i < 16; i++) {
        a_c[i] = (signed char)(i * 3);
        b_c[i] = (signed char)(i + 1);
        c_c[i] = 0;
    }
    test_sub2d_char(c_c, a_c, b_c);
    for (int i = 0; i < 16; i++) {
        signed char expected = (signed char)(a_c[i] - b_c[i]);
        if (c_c[i] != expected) {
            printf("FAIL: sub2d_char[%d] = %d (expected %d)\n", i, c_c[i], expected);
            return 1;
        }
    }

    /* 4. 2D int multiplication (16 x 2 = 32) */
    int a_m[32], b_m[32], c_m[32];
    for (int i = 0; i < 32; i++) {
        a_m[i] = i + 2;
        b_m[i] = i + 3;
        c_m[i] = 0;
    }
    test_mul2d_int(c_m, a_m, b_m);
    for (int i = 0; i < 32; i++) {
        int expected = a_m[i] * b_m[i];
        if (c_m[i] != expected) {
            printf("FAIL: mul2d_int[%d] = %d (expected %d)\n", i, c_m[i], expected);
            return 1;
        }
    }

    /* 5. 2D uint bitwise XOR (8 x 4 = 32) */
    unsigned int a_x[32], b_x[32], c_x[32];
    for (int i = 0; i < 32; i++) {
        a_x[i] = (unsigned int)(0x12345678U ^ (unsigned int)i);
        b_x[i] = (unsigned int)(0xABCDEFF0U + (unsigned int)i * 7);
        c_x[i] = 0;
    }
    test_xor2d_uint(c_x, a_x, b_x);
    for (int i = 0; i < 32; i++) {
        unsigned int expected = a_x[i] ^ b_x[i];
        if (c_x[i] != expected) {
            printf("FAIL: xor2d_uint[%d] = %u (expected %u)\n", i, c_x[i], expected);
            return 1;
        }
    }

    printf("ALL SIMD LOOP COLLAPSE TESTS PASSED\n");
    return 0;
}
