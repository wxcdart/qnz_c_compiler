/* Test Suite: SIMD Epilogue Vectorization (Vectorized Remainder)
 * Sprint 217: Vectorize remainder epilogues for loops with non-multiple trip counts.
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

void test_rem_add_int(int n) {
    int a[64], b[64], c[64];
    for (int i = 0; i < n; i++) {
        a[i] = i * 3 + 1;
        b[i] = i * 2 + 5;
        c[i] = 0;
    }
    for (int i = 0; i < n; i++) {
        c[i] = a[i] + b[i];
    }
    for (int i = 0; i < n; i++) {
        ASSERT_EQ(c[i], (i * 3 + 1) + (i * 2 + 5));
    }
}

void test_rem_add_float(int n) {
    float a[64], b[64], c[64];
    for (int i = 0; i < n; i++) {
        a[i] = (float)i * 1.5f;
        b[i] = (float)i * 0.5f + 2.0f;
        c[i] = 0.0f;
    }
    for (int i = 0; i < n; i++) {
        c[i] = a[i] + b[i];
    }
    for (int i = 0; i < n; i++) {
        ASSERT_FLOAT_EQ(c[i], ((float)i * 1.5f) + ((float)i * 0.5f + 2.0f));
    }
}

void test_rem_mul_float(int n) {
    float a[64], b[64], c[64];
    for (int i = 0; i < n; i++) {
        a[i] = (float)(i + 1);
        b[i] = 2.0f;
        c[i] = 0.0f;
    }
    for (int i = 0; i < n; i++) {
        c[i] = a[i] * b[i];
    }
    for (int i = 0; i < n; i++) {
        ASSERT_FLOAT_EQ(c[i], (float)(i + 1) * 2.0f);
    }
}

int test_rem_sum_reduction(int n) {
    int a[64];
    int expected = 0;
    for (int i = 0; i < n; i++) {
        a[i] = i + 1;
        expected += (i + 1);
    }
    int sum = 0;
    for (int i = 0; i < n; i++) {
        sum += a[i];
    }
    ASSERT_EQ(sum, expected);
    return sum;
}

int main(void) {
    /* Test non-multiple trip counts */
    static const int counts[] = {7, 9, 14, 15, 33, 38};
    for (int k = 0; k < 6; k++) {
        int n = counts[k];
        test_rem_add_int(n);
        test_rem_add_float(n);
        test_rem_mul_float(n);
        test_rem_sum_reduction(n);
    }

    printf("simd_epilogue_test: PASS\n");
    return 0;
}
