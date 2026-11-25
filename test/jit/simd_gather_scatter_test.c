/* Test Suite: Indexed Gather & Scatter SIMD Auto-Vectorization
 * Sprint 218: Indirect array accesses (dest[i] = src[idx[i]] and dest[idx[i]] = src[i])
 */

#include <stdio.h>
#include <stdlib.h>

#define ASSERT_EQ(a, b) do { \
    if ((a) != (b)) { \
        printf("FAIL: %s:%d: %lld != %lld\n", __FILE__, __LINE__, (long long)(a), (long long)(b)); \
        exit(1); \
    } \
} while (0)

#define ASSERT_FLOAT_EQ(a, b) do { \
    float diff = (a) - (b); \
    if (diff < -0.001f || diff > 0.001f) { \
        printf("FAIL: %s:%d: %f != %f\n", __FILE__, __LINE__, (double)(a), (double)(b)); \
        exit(1); \
    } \
} while (0)

#define ASSERT_DOUBLE_EQ(a, b) do { \
    double diff = (a) - (b); \
    if (diff < -0.0001 || diff > 0.0001) { \
        printf("FAIL: %s:%d: %f != %f\n", __FILE__, __LINE__, (double)(a), (double)(b)); \
        exit(1); \
    } \
} while (0)

void test_int_gather(int n) {
    int src[64], idx[64], dest[64];
    for (int i = 0; i < 64; i++) {
        src[i] = (i * 7 + 3) ^ 0x5A;
        idx[i] = (i * 13 + 5) % 64;
        dest[i] = -999;
    }
    for (int i = 0; i < n; i++) {
        dest[i] = src[idx[i]];
    }
    for (int i = 0; i < n; i++) {
        ASSERT_EQ(dest[i], src[idx[i]]);
    }
    for (int i = n; i < 64; i++) {
        ASSERT_EQ(dest[i], -999);
    }
}

void test_int_scatter(int n) {
    int src[64], idx[64], dest[64], expected[64];
    for (int i = 0; i < 64; i++) {
        src[i] = i * 11 + 7;
        /* Bijective permutation to avoid collisions */
        idx[i] = (i * 7 + 1) % 64;
        dest[i] = 0;
        expected[i] = 0;
    }
    for (int i = 0; i < n; i++) {
        expected[idx[i]] = src[i];
    }
    for (int i = 0; i < n; i++) {
        dest[idx[i]] = src[i];
    }
    for (int i = 0; i < 64; i++) {
        ASSERT_EQ(dest[i], expected[i]);
    }
}

void test_float_gather(int n) {
    float src[64], dest[64];
    int idx[64];
    for (int i = 0; i < 64; i++) {
        src[i] = (float)(i * 2.5f + 1.25f);
        idx[i] = (63 - i);
        dest[i] = -1.0f;
    }
    for (int i = 0; i < n; i++) {
        dest[i] = src[idx[i]];
    }
    for (int i = 0; i < n; i++) {
        ASSERT_FLOAT_EQ(dest[i], src[idx[i]]);
    }
}

void test_float_scatter(int n) {
    float src[64], dest[64], expected[64];
    int idx[64];
    for (int i = 0; i < 64; i++) {
        src[i] = (float)(i * 3.5f + 0.5f);
        idx[i] = (i * 3 + 7) % 64;
        dest[i] = 0.0f;
        expected[i] = 0.0f;
    }
    for (int i = 0; i < n; i++) {
        expected[idx[i]] = src[i];
    }
    for (int i = 0; i < n; i++) {
        dest[idx[i]] = src[i];
    }
    for (int i = 0; i < 64; i++) {
        ASSERT_FLOAT_EQ(dest[i], expected[i]);
    }
}

void test_double_gather(int n) {
    double src[64], dest[64];
    int idx[64];
    for (int i = 0; i < 64; i++) {
        src[i] = (double)(i * 1.75 + 4.5);
        idx[i] = (i * 5 + 3) % 64;
        dest[i] = -1.0;
    }
    for (int i = 0; i < n; i++) {
        dest[i] = src[idx[i]];
    }
    for (int i = 0; i < n; i++) {
        ASSERT_DOUBLE_EQ(dest[i], src[idx[i]]);
    }
}

void test_double_scatter(int n) {
    double src[64], dest[64], expected[64];
    int idx[64];
    for (int i = 0; i < 64; i++) {
        src[i] = (double)(i * 4.25 + 2.0);
        idx[i] = (i * 11 + 5) % 64;
        dest[i] = 0.0;
        expected[i] = 0.0;
    }
    for (int i = 0; i < n; i++) {
        expected[idx[i]] = src[i];
    }
    for (int i = 0; i < n; i++) {
        dest[idx[i]] = src[i];
    }
    for (int i = 0; i < 64; i++) {
        ASSERT_DOUBLE_EQ(dest[i], expected[i]);
    }
}

void test_gather_arith(int n) {
    int src[64], idx[64], dest[64];
    for (int i = 0; i < 64; i++) {
        src[i] = i * 3 + 2;
        idx[i] = (i * 9 + 4) % 64;
        dest[i] = 0;
    }
    for (int i = 0; i < n; i++) {
        dest[i] = src[idx[i]] * 2 + 5;
    }
    for (int i = 0; i < n; i++) {
        ASSERT_EQ(dest[i], src[idx[i]] * 2 + 5);
    }
}

int main(void) {
    static const int counts[] = {4, 7, 8, 15, 16, 23, 32, 47, 48};
    int num_counts = sizeof(counts) / sizeof(counts[0]);

    for (int c = 0; c < num_counts; c++) {
        int n = counts[c];
        test_int_gather(n);
        test_int_scatter(n);
        test_float_gather(n);
        test_float_scatter(n);
        test_double_gather(n);
        test_double_scatter(n);
        test_gather_arith(n);
    }

    printf("PASS: SIMD gather & scatter vectorization tests passed.\n");
    return 0;
}
