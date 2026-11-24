/* Test suite for Strided & Non-Unit Stride SIMD Auto-Vectorization */
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <math.h>

struct Point {
    float x;
    float y;
};

struct Point3D {
    float x;
    float y;
    float z;
};

/* 1. Int strided load: b[i] = a[2*i] * 3 (exact trip count 16) */
void test_int_strided_load(const int *a, int *b, int n) {
    for (int i = 0; i < n; i++) {
        b[i] = a[2 * i] * 3;
    }
}

/* 2. Int strided load and store: b[2*i] = a[2*i] + 5 (peeled trip count 15) */
void test_int_strided_load_store(const int *a, int *b, int n) {
    for (int i = 0; i < n; i++) {
        b[2 * i] = a[2 * i] + 5;
    }
}

/* 3. Float strided load: b[i] = a[2*i] * 2.5f (exact trip count 16) */
void test_float_strided_load(const float *a, float *b, int n) {
    for (int i = 0; i < n; i++) {
        b[i] = a[2 * i] * 2.5f;
    }
}

/* 4. Float AoS access: pts[i].x += 1.0f; pts[i].y *= 2.0f; (trip count 15) */
void test_float_aos(struct Point *pts, int n) {
    for (int i = 0; i < n; i++) {
        pts[i].x = pts[i].x + 1.0f;
        pts[i].y = pts[i].y * 2.0f;
    }
}

/* 5. Double strided load and store: b[2*i] = a[2*i] + 1.5 (trip count 10) */
void test_double_strided(const double *a, double *b, int n) {
    for (int i = 0; i < n; i++) {
        b[2 * i] = a[2 * i] + 1.5;
    }
}

/* 6. Strided reduction: sum += a[2*i] */
int test_int_strided_reduction(const int *a, int n) {
    int sum = 0;
    for (int i = 0; i < n; i++) {
        sum += a[2 * i];
    }
    return sum;
}

/* 7. Two-input strided load: c[i] = a[2*i] + b[2*i] */
void test_two_input_strided(const int *a, const int *b, int *c, int n) {
    for (int i = 0; i < n; i++) {
        c[i] = a[2 * i] + b[2 * i];
    }
}

/* 8. Two-input strided load and strided store: c[2*i] = a[2*i] + b[2*i] */
void test_two_input_strided_store(const int *a, const int *b, int *c, int n) {
    for (int i = 0; i < n; i++) {
        c[2 * i] = a[2 * i] + b[2 * i];
    }
}

int main(void) {
    /* Test 1: Int strided load */
    {
        int a[32];
        int b[16];
        for (int i = 0; i < 32; i++) a[i] = i;
        for (int i = 0; i < 16; i++) b[i] = 0;

        test_int_strided_load(a, b, 16);
        for (int i = 0; i < 16; i++) {
            int expected = (2 * i) * 3;
            if (b[i] != expected) {
                printf("FAIL test_int_strided_load at %d: got %d, expected %d\n", i, b[i], expected);
                return 1;
            }
        }
    }

    /* Test 2: Int strided load and store (peeled trip count 15) */
    {
        int a[40];
        int b[40];
        for (int i = 0; i < 40; i++) { a[i] = i * 2; b[i] = 0; }

        test_int_strided_load_store(a, b, 15);
        for (int i = 0; i < 15; i++) {
            int expected = a[2 * i] + 5;
            if (b[2 * i] != expected) {
                printf("FAIL test_int_strided_load_store at %d: got %d, expected %d\n", i, b[2 * i], expected);
                return 2;
            }
        }
    }

    /* Test 3: Float strided load */
    {
        float a[32];
        float b[16];
        for (int i = 0; i < 32; i++) a[i] = (float)i;
        for (int i = 0; i < 16; i++) b[i] = 0.0f;

        test_float_strided_load(a, b, 16);
        for (int i = 0; i < 16; i++) {
            float expected = (float)(2 * i) * 2.5f;
            if (fabsf(b[i] - expected) > 1e-4f) {
                printf("FAIL test_float_strided_load at %d: got %f, expected %f\n", i, b[i], expected);
                return 3;
            }
        }
    }

    /* Test 4: Float AoS struct members (Point {x, y}) */
    {
        struct Point pts[15];
        for (int i = 0; i < 15; i++) {
            pts[i].x = (float)i;
            pts[i].y = (float)(i + 10);
        }

        test_float_aos(pts, 15);
        for (int i = 0; i < 15; i++) {
            float expected_x = (float)i + 1.0f;
            float expected_y = (float)(i + 10) * 2.0f;
            if (fabsf(pts[i].x - expected_x) > 1e-4f || fabsf(pts[i].y - expected_y) > 1e-4f) {
                printf("FAIL test_float_aos at %d: got (%f, %f), expected (%f, %f)\n",
                       i, pts[i].x, pts[i].y, expected_x, expected_y);
                return 4;
            }
        }
    }

    /* Test 5: Double strided load and store */
    {
        double a[30];
        double b[30];
        for (int i = 0; i < 30; i++) { a[i] = (double)i * 1.25; b[i] = 0.0; }

        test_double_strided(a, b, 10);
        for (int i = 0; i < 10; i++) {
            double expected = a[2 * i] + 1.5;
            if (fabs(b[2 * i] - expected) > 1e-5) {
                printf("FAIL test_double_strided at %d: got %f, expected %f\n", i, b[2 * i], expected);
                return 5;
            }
        }
    }

    /* Test 6: Strided reduction */
    {
        int a[32];
        int expected_sum = 0;
        for (int i = 0; i < 32; i++) a[i] = i + 1;
        for (int i = 0; i < 15; i++) expected_sum += a[2 * i];

        int sum = test_int_strided_reduction(a, 15);
        if (sum != expected_sum) {
            printf("FAIL test_int_strided_reduction: got %d, expected %d\n", sum, expected_sum);
            return 6;
        }
    }

    /* Test 7: Two-input strided load */
    {
        int a[32], b[32], c[16];
        for (int i = 0; i < 32; i++) { a[i] = i * 2; b[i] = i * 3; }
        for (int i = 0; i < 16; i++) c[i] = 0;

        test_two_input_strided(a, b, c, 16);
        for (int i = 0; i < 16; i++) {
            int expected = a[2 * i] + b[2 * i];
            if (c[i] != expected) {
                printf("FAIL test_two_input_strided at %d: got %d, expected %d\n", i, c[i], expected);
                return 7;
            }
        }
    }

    /* Test 8: Two-input strided load and store */
    {
        int a[40], b[40], c[40];
        for (int i = 0; i < 40; i++) { a[i] = i; b[i] = 100 - i; c[i] = 0; }

        test_two_input_strided_store(a, b, c, 15);
        for (int i = 0; i < 15; i++) {
            int expected = a[2 * i] + b[2 * i];
            if (c[2 * i] != expected) {
                printf("FAIL test_two_input_strided_store at %d: got %d, expected %d\n", i, c[2 * i], expected);
                return 8;
            }
        }
    }

    printf("PASS: Strided & Non-Unit Stride SIMD Auto-Vectorization tests succeeded\n");
    return 0;
}
