#include <stdio.h>
#include <stdlib.h>
#include <math.h>

static void test_int_max(int *c, const int *a, const int *b) {
    for (int i = 0; i < 32; i++) {
        c[i] = (a[i] > b[i]) ? a[i] : b[i];
    }
}

static void test_int_clamp(int *c, const int *a) {
    for (int i = 0; i < 32; i++) {
        c[i] = (a[i] < 0) ? 0 : a[i];
    }
}

static void test_float_cond(float *c, const float *a, const float *b) {
    for (int i = 0; i < 32; i++) {
        c[i] = (a[i] >= b[i]) ? a[i] : b[i];
    }
}

static void test_float_math_cond(float *c, const float *a) {
    for (int i = 0; i < 32; i++) {
        c[i] = (a[i] > 0.0f) ? (a[i] * 2.0f) : (a[i] - 1.0f);
    }
}

static void test_double_cond(double *c, const double *a, const double *b) {
    for (int i = 0; i < 32; i++) {
        c[i] = (a[i] != b[i]) ? a[i] : 0.0;
    }
}

/* Remainder loop: 35 elements (8 vector iterations + 3 peeled remainder iterations) */
static void test_remainder_peel(int *c, const int *a, const int *b) {
    for (int i = 0; i < 35; i++) {
        c[i] = (a[i] < b[i]) ? a[i] : b[i];
    }
}

int main(void) {
    int a_i[40], b_i[40], c_i[40];
    float a_f[40], b_f[40], c_f[40];
    double a_d[40], b_d[40], c_d[40];

    for (int i = 0; i < 40; i++) {
        a_i[i] = (i % 2 == 0) ? (i * 10) : (50 - i * 5);
        b_i[i] = 25;
        a_f[i] = (float)a_i[i] - 20.0f;
        b_f[i] = 15.5f;
        a_d[i] = (double)a_i[i];
        b_d[i] = (i % 3 == 0) ? (double)a_i[i] : 0.0;
    }

    /* 1. Int max */
    test_int_max(c_i, a_i, b_i);
    for (int i = 0; i < 32; i++) {
        int expected = (a_i[i] > b_i[i]) ? a_i[i] : b_i[i];
        if (c_i[i] != expected) {
            printf("FAIL: int_max[%d] = %d (expected %d)\n", i, c_i[i], expected);
            return 1;
        }
    }

    /* 2. Int clamp */
    test_int_clamp(c_i, a_i);
    for (int i = 0; i < 32; i++) {
        int expected = (a_i[i] < 0) ? 0 : a_i[i];
        if (c_i[i] != expected) {
            printf("FAIL: int_clamp[%d] = %d (expected %d)\n", i, c_i[i], expected);
            return 1;
        }
    }

    /* 3. Float cond */
    test_float_cond(c_f, a_f, b_f);
    for (int i = 0; i < 32; i++) {
        float expected = (a_f[i] >= b_f[i]) ? a_f[i] : b_f[i];
        if (c_f[i] != expected) {
            printf("FAIL: float_cond[%d] = %f (expected %f)\n", i, c_f[i], expected);
            return 1;
        }
    }

    /* 4. Float math cond */
    test_float_math_cond(c_f, a_f);
    for (int i = 0; i < 32; i++) {
        float expected = (a_f[i] > 0.0f) ? (a_f[i] * 2.0f) : (a_f[i] - 1.0f);
        if (fabsf(c_f[i] - expected) > 1e-5f) {
            printf("FAIL: float_math_cond[%d] = %f (expected %f)\n", i, c_f[i], expected);
            return 1;
        }
    }

    /* 5. Double cond */
    test_double_cond(c_d, a_d, b_d);
    for (int i = 0; i < 32; i++) {
        double expected = (a_d[i] != b_d[i]) ? a_d[i] : 0.0;
        if (c_d[i] != expected) {
            printf("FAIL: double_cond[%d] = %f (expected %f)\n", i, c_d[i], expected);
            return 1;
        }
    }

    /* 6. Remainder peel */
    test_remainder_peel(c_i, a_i, b_i);
    for (int i = 0; i < 35; i++) {
        int expected = (a_i[i] < b_i[i]) ? a_i[i] : b_i[i];
        if (c_i[i] != expected) {
            printf("FAIL: remainder_peel[%d] = %d (expected %d)\n", i, c_i[i], expected);
            return 1;
        }
    }

    printf("ALL SIMD COND VECTORIZE TESTS PASSED\n");
    return 0;
}
