/* test/jit/avx512_vectorize.c - Loop auto-vectorization targeting AVX-512 */
#include <stdio.h>
#include <math.h>

void vec_add_float(float *c, const float *a, const float *b) {
    for (int i = 0; i < 35; i++) {
        c[i] = a[i] + b[i];
    }
}

void vec_mul_double(double *c, const double *a, const double *b) {
    for (int i = 0; i < 20; i++) {
        c[i] = a[i] * b[i];
    }
}

int main(void) {
    float a[35], b[35], c[35];
    for (int i = 0; i < 35; i++) {
        a[i] = (float)i * 1.5f;
        b[i] = (float)(i + 2) * 2.0f;
    }

    vec_add_float(c, a, b);

    for (int i = 0; i < 35; i++) {
        float expected = a[i] + b[i];
        if (fabs(c[i] - expected) > 1e-5) {
            printf("FAIL: vec_add_float[%d] got %f expected %f\n", i, c[i], expected);
            return 1;
        }
    }

    double da[20], db[20], dc[20];
    for (int i = 0; i < 20; i++) {
        da[i] = (double)i * 2.5;
        db[i] = (double)(i + 1) * 3.0;
    }

    vec_mul_double(dc, da, db);

    for (int i = 0; i < 20; i++) {
        double expected = da[i] * db[i];
        if (fabs(dc[i] - expected) > 1e-9) {
            printf("FAIL: vec_mul_double[%d] got %f expected %f\n", i, dc[i], expected);
            return 2;
        }
    }

    printf("AVX-512 loop vectorization OK\n");
    return 0;
}
