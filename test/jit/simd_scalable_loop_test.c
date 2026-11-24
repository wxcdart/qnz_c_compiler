/* Test scalable loop auto-vectorization (RVV 1.0 dynamic vsetvli & SVE whilelt)
 * Verifies correctness across variable and tail trip counts (e.g. n=1, 7, 16, 35).
 */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>

void vec_add_int(int *c, const int *a, const int *b, int n) {
    for (int i = 0; i < n; i++) {
        c[i] = a[i] + b[i];
    }
}

void vec_sub_int(int *c, const int *a, const int *b, int n) {
    for (int i = 0; i < n; i++) {
        c[i] = a[i] - b[i];
    }
}

void vec_mul_int(int *c, const int *a, const int *b, int n) {
    for (int i = 0; i < n; i++) {
        c[i] = a[i] * b[i];
    }
}

void vec_and_int(int *c, const int *a, const int *b, int n) {
    for (int i = 0; i < n; i++) {
        c[i] = a[i] & b[i];
    }
}

void vec_add_float(float *c, const float *a, const float *b, int n) {
    for (int i = 0; i < n; i++) {
        c[i] = a[i] + b[i];
    }
}

void vec_sub_float(float *c, const float *a, const float *b, int n) {
    for (int i = 0; i < n; i++) {
        c[i] = a[i] - b[i];
    }
}

void vec_mul_float(float *c, const float *a, const float *b, int n) {
    for (int i = 0; i < n; i++) {
        c[i] = a[i] * b[i];
    }
}

void vec_add_double(double *c, const double *a, const double *b, int n) {
    for (int i = 0; i < n; i++) {
        c[i] = a[i] + b[i];
    }
}

void vec_sub_double(double *c, const double *a, const double *b, int n) {
    for (int i = 0; i < n; i++) {
        c[i] = a[i] - b[i];
    }
}

int main(void) {
    static const int test_sizes[] = {1, 3, 4, 7, 16, 25, 35, 64};
    int num_sizes = sizeof(test_sizes) / sizeof(test_sizes[0]);

    for (int s = 0; s < num_sizes; s++) {
        int n = test_sizes[s];
        int a_i[64], b_i[64], c_i[64];
        float a_f[64], b_f[64], c_f[64];
        double a_d[64], b_d[64], c_d[64];

        for (int i = 0; i < n; i++) {
            a_i[i] = i * 3 + 1;
            b_i[i] = i * 2 + 5;
            c_i[i] = 0;

            a_f[i] = (float)(i * 1.5f + 0.25f);
            b_f[i] = (float)(i * 0.5f + 1.75f);
            c_f[i] = 0.0f;

            a_d[i] = i * 2.25 + 0.125;
            b_d[i] = i * 1.75 + 3.5;
            c_d[i] = 0.0;
        }

        /* 1. Integer Addition */
        vec_add_int(c_i, a_i, b_i, n);
        for (int i = 0; i < n; i++) {
            int expected = a_i[i] + b_i[i];
            if (c_i[i] != expected) {
                fprintf(stderr, "FAIL add_int: size %d idx %d got %d expected %d\n", n, i, c_i[i], expected);
                return 1;
            }
        }

        /* 2. Integer Subtraction */
        vec_sub_int(c_i, a_i, b_i, n);
        for (int i = 0; i < n; i++) {
            int expected = a_i[i] - b_i[i];
            if (c_i[i] != expected) {
                fprintf(stderr, "FAIL sub_int: size %d idx %d got %d expected %d\n", n, i, c_i[i], expected);
                return 1;
            }
        }

        /* 3. Integer Multiplication */
        vec_mul_int(c_i, a_i, b_i, n);
        for (int i = 0; i < n; i++) {
            int expected = a_i[i] * b_i[i];
            if (c_i[i] != expected) {
                fprintf(stderr, "FAIL mul_int: size %d idx %d got %d expected %d\n", n, i, c_i[i], expected);
                return 1;
            }
        }

        /* 4. Integer Bitwise AND */
        vec_and_int(c_i, a_i, b_i, n);
        for (int i = 0; i < n; i++) {
            int expected = a_i[i] & b_i[i];
            if (c_i[i] != expected) {
                fprintf(stderr, "FAIL and_int: size %d idx %d got %d expected %d\n", n, i, c_i[i], expected);
                return 1;
            }
        }

        /* 5. Float Addition */
        vec_add_float(c_f, a_f, b_f, n);
        for (int i = 0; i < n; i++) {
            float expected = a_f[i] + b_f[i];
            if (fabsf(c_f[i] - expected) > 1e-4f) {
                fprintf(stderr, "FAIL add_float: size %d idx %d got %f expected %f\n", n, i, (double)c_f[i], (double)expected);
                return 1;
            }
        }

        /* 6. Float Subtraction */
        vec_sub_float(c_f, a_f, b_f, n);
        for (int i = 0; i < n; i++) {
            float expected = a_f[i] - b_f[i];
            if (fabsf(c_f[i] - expected) > 1e-4f) {
                fprintf(stderr, "FAIL sub_float: size %d idx %d got %f expected %f\n", n, i, (double)c_f[i], (double)expected);
                return 1;
            }
        }

        /* 7. Float Multiplication */
        vec_mul_float(c_f, a_f, b_f, n);
        for (int i = 0; i < n; i++) {
            float expected = a_f[i] * b_f[i];
            if (fabsf(c_f[i] - expected) > 1e-4f) {
                fprintf(stderr, "FAIL mul_float: size %d idx %d got %f expected %f\n", n, i, (double)c_f[i], (double)expected);
                return 1;
            }
        }

        /* 8. Double Addition */
        vec_add_double(c_d, a_d, b_d, n);
        for (int i = 0; i < n; i++) {
            double expected = a_d[i] + b_d[i];
            if (fabs(c_d[i] - expected) > 1e-6) {
                fprintf(stderr, "FAIL add_double: size %d idx %d got %f expected %f\n", n, i, c_d[i], expected);
                return 1;
            }
        }

        /* 9. Double Subtraction */
        vec_sub_double(c_d, a_d, b_d, n);
        for (int i = 0; i < n; i++) {
            double expected = a_d[i] - b_d[i];
            if (fabs(c_d[i] - expected) > 1e-6) {
                fprintf(stderr, "FAIL sub_double: size %d idx %d got %f expected %f\n", n, i, c_d[i], expected);
                return 1;
            }
        }
    }

    printf("PASS: simd_scalable_loop_test (all sizes and ops passed)\n");
    return 0;
}
