#include <stdio.h>
#include <stdint.h>

void test_int32_add(int32_t *a, int32_t *b, int32_t *c) {
    for (int i = 0; i < 32; i++) {
        c[i] = a[i] + b[i];
    }
}

void test_int32_sub(int32_t *a, int32_t *b, int32_t *c) {
    for (int i = 0; i < 32; i++) {
        c[i] = a[i] - b[i];
    }
}

void test_int32_mul(int32_t *a, int32_t *b, int32_t *c) {
    for (int i = 0; i < 32; i++) {
        c[i] = a[i] * b[i];
    }
}

void test_int16_add(int16_t *a, int16_t *b, int16_t *c) {
    for (int i = 0; i < 32; i++) {
        c[i] = a[i] + b[i];
    }
}

void test_int16_sub(int16_t *a, int16_t *b, int16_t *c) {
    for (int i = 0; i < 32; i++) {
        c[i] = a[i] - b[i];
    }
}

void test_int16_mul(int16_t *a, int16_t *b, int16_t *c) {
    for (int i = 0; i < 32; i++) {
        c[i] = a[i] * b[i];
    }
}

void test_int8_add(int8_t *a, int8_t *b, int8_t *c) {
    for (int i = 0; i < 64; i++) {
        c[i] = a[i] + b[i];
    }
}

void test_int8_sub(int8_t *a, int8_t *b, int8_t *c) {
    for (int i = 0; i < 64; i++) {
        c[i] = a[i] - b[i];
    }
}

int main(void) {
    /* Test 1: Int32 ops (32 elements = 4 AVX2 256-bit vectors) */
    int32_t a32[32], b32[32], c32[32];
    for (int i = 0; i < 32; i++) {
        a32[i] = i * 7 + 3;
        b32[i] = i * 3 + 1;
    }
    test_int32_add(a32, b32, c32);
    for (int i = 0; i < 32; i++) {
        if (c32[i] != a32[i] + b32[i]) {
            printf("FAIL: int32 add at %d: %d != %d\n", i, c32[i], a32[i] + b32[i]);
            return 1;
        }
    }

    test_int32_sub(a32, b32, c32);
    for (int i = 0; i < 32; i++) {
        if (c32[i] != a32[i] - b32[i]) {
            printf("FAIL: int32 sub at %d: %d != %d\n", i, c32[i], a32[i] - b32[i]);
            return 2;
        }
    }

    test_int32_mul(a32, b32, c32);
    for (int i = 0; i < 32; i++) {
        if (c32[i] != a32[i] * b32[i]) {
            printf("FAIL: int32 mul at %d: %d != %d\n", i, c32[i], a32[i] * b32[i]);
            return 3;
        }
    }

    /* Test 2: Int16 ops (32 elements = 2 AVX2 256-bit vectors) */
    int16_t a16[32], b16[32], c16[32];
    for (int i = 0; i < 32; i++) {
        a16[i] = (int16_t)(i * 5 + 2);
        b16[i] = (int16_t)(i * 2 + 3);
    }
    test_int16_add(a16, b16, c16);
    for (int i = 0; i < 32; i++) {
        if (c16[i] != (int16_t)(a16[i] + b16[i])) {
            printf("FAIL: int16 add at %d\n", i);
            return 4;
        }
    }

    test_int16_sub(a16, b16, c16);
    for (int i = 0; i < 32; i++) {
        if (c16[i] != (int16_t)(a16[i] - b16[i])) {
            printf("FAIL: int16 sub at %d\n", i);
            return 5;
        }
    }

    test_int16_mul(a16, b16, c16);
    for (int i = 0; i < 32; i++) {
        if (c16[i] != (int16_t)(a16[i] * b16[i])) {
            printf("FAIL: int16 mul at %d\n", i);
            return 6;
        }
    }

    /* Test 3: Int8 ops (64 elements = 2 AVX2 256-bit vectors) */
    int8_t a8[64], b8[64], c8[64];
    for (int i = 0; i < 64; i++) {
        a8[i] = (int8_t)(i * 3 + 1);
        b8[i] = (int8_t)(i + 2);
    }
    test_int8_add(a8, b8, c8);
    for (int i = 0; i < 64; i++) {
        if (c8[i] != (int8_t)(a8[i] + b8[i])) {
            printf("FAIL: int8 add at %d\n", i);
            return 7;
        }
    }

    test_int8_sub(a8, b8, c8);
    for (int i = 0; i < 64; i++) {
        if (c8[i] != (int8_t)(a8[i] - b8[i])) {
            printf("FAIL: int8 sub at %d\n", i);
            return 8;
        }
    }

    printf("PASS: AVX2 256-bit vectorization\n");
    return 0;
}
