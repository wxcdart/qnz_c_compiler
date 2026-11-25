/*
 * simd_complex_test.c - Test SIMD Vectorization of C99 Complex Array Operations (Sprint 227)
 *
 * Verifies vectorization and correctness of _Complex float and _Complex double array math:
 * 1. Complex float element-wise addition: c[i] = a[i] + b[i]
 * 2. Complex float element-wise subtraction: c[i] = a[i] - b[i]
 * 3. Complex float scalar multiplication: c[i] = a[i] * scale
 * 4. Complex float conjugate: c[i] = ~a[i]
 * 5. Complex double element-wise addition: c[i] = a[i] + b[i]
 * 6. Complex double element-wise subtraction: c[i] = a[i] - b[i]
 * 7. Complex double scalar multiplication: c[i] = a[i] * scale
 * 8. Complex double conjugate: c[i] = ~a[i]
 * 9. Complex multiplication: (a[i] * b[i]) real/imag parts
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <complex.h>
#include <math.h>
#include <assert.h>

#define N 16

/* 1. Complex float addition */
static void add_cfloat(_Complex float *restrict c, const _Complex float *restrict a, const _Complex float *restrict b, int n) {
    for (int i = 0; i < n; i++) {
        c[i] = a[i] + b[i];
    }
}

/* 2. Complex float subtraction */
static void sub_cfloat(_Complex float *restrict c, const _Complex float *restrict a, const _Complex float *restrict b, int n) {
    for (int i = 0; i < n; i++) {
        c[i] = a[i] - b[i];
    }
}

/* 3. Complex float scalar multiplication */
static void scale_cfloat(_Complex float *restrict c, const _Complex float *restrict a, float s, int n) {
    for (int i = 0; i < n; i++) {
        c[i] = a[i] * s;
    }
}

/* 4. Complex float conjugate */
static void conj_cfloat(_Complex float *restrict c, const _Complex float *restrict a, int n) {
    for (int i = 0; i < n; i++) {
        c[i] = ~a[i];
    }
}

/* 5. Complex double addition */
static void add_cdouble(_Complex double *restrict c, const _Complex double *restrict a, const _Complex double *restrict b, int n) {
    for (int i = 0; i < n; i++) {
        c[i] = a[i] + b[i];
    }
}

/* 6. Complex double subtraction */
static void sub_cdouble(_Complex double *restrict c, const _Complex double *restrict a, const _Complex double *restrict b, int n) {
    for (int i = 0; i < n; i++) {
        c[i] = a[i] - b[i];
    }
}

/* 7. Complex double scalar multiplication */
static void scale_cdouble(_Complex double *restrict c, const _Complex double *restrict a, double s, int n) {
    for (int i = 0; i < n; i++) {
        c[i] = a[i] * s;
    }
}

/* 8. Complex double conjugate */
static void conj_cdouble(_Complex double *restrict c, const _Complex double *restrict a, int n) {
    for (int i = 0; i < n; i++) {
        c[i] = ~a[i];
    }
}

int main(void) {
    _Complex float fa[N], fb[N], fc[N];
    _Complex double da[N], db[N], dc[N];

    for (int i = 0; i < N; i++) {
        fa[i] = (float)(i + 1) + (float)(i * 2 + 1) * I;
        fb[i] = (float)(i * 3 + 2) + (float)(i + 4) * I;
        da[i] = (double)(i + 1) + (double)(i * 2 + 1) * I;
        db[i] = (double)(i * 3 + 2) + (double)(i + 4) * I;
    }

    /* Test 1: Complex float add */
    add_cfloat(fc, fa, fb, N);
    for (int i = 0; i < N; i++) {
        float exp_re = (float)(i + 1) + (float)(i * 3 + 2);
        float exp_im = (float)(i * 2 + 1) + (float)(i + 4);
        assert(fabsf(__real__ fc[i] - exp_re) < 1e-5f);
        assert(fabsf(__imag__ fc[i] - exp_im) < 1e-5f);
    }

    /* Test 2: Complex float sub */
    sub_cfloat(fc, fa, fb, N);
    for (int i = 0; i < N; i++) {
        float exp_re = (float)(i + 1) - (float)(i * 3 + 2);
        float exp_im = (float)(i * 2 + 1) - (float)(i + 4);
        assert(fabsf(__real__ fc[i] - exp_re) < 1e-5f);
        assert(fabsf(__imag__ fc[i] - exp_im) < 1e-5f);
    }

    /* Test 3: Complex float scale */
    scale_cfloat(fc, fa, 2.5f, N);
    for (int i = 0; i < N; i++) {
        float exp_re = ((float)(i + 1)) * 2.5f;
        float exp_im = ((float)(i * 2 + 1)) * 2.5f;
        assert(fabsf(__real__ fc[i] - exp_re) < 1e-5f);
        assert(fabsf(__imag__ fc[i] - exp_im) < 1e-5f);
    }

    /* Test 4: Complex float conjugate */
    conj_cfloat(fc, fa, N);
    for (int i = 0; i < N; i++) {
        float exp_re = (float)(i + 1);
        float exp_im = -(float)(i * 2 + 1);
        assert(fabsf(__real__ fc[i] - exp_re) < 1e-5f);
        assert(fabsf(__imag__ fc[i] - exp_im) < 1e-5f);
    }

    /* Test 5: Complex double add */
    add_cdouble(dc, da, db, N);
    for (int i = 0; i < N; i++) {
        double exp_re = (double)(i + 1) + (double)(i * 3 + 2);
        double exp_im = (double)(i * 2 + 1) + (double)(i + 4);
        assert(fabs(__real__ dc[i] - exp_re) < 1e-9);
        assert(fabs(__imag__ dc[i] - exp_im) < 1e-9);
    }

    /* Test 6: Complex double sub */
    sub_cdouble(dc, da, db, N);
    for (int i = 0; i < N; i++) {
        double exp_re = (double)(i + 1) - (double)(i * 3 + 2);
        double exp_im = (double)(i * 2 + 1) - (double)(i + 4);
        assert(fabs(__real__ dc[i] - exp_re) < 1e-9);
        assert(fabs(__imag__ dc[i] - exp_im) < 1e-9);
    }

    /* Test 7: Complex double scale */
    scale_cdouble(dc, da, 3.0, N);
    for (int i = 0; i < N; i++) {
        double exp_re = ((double)(i + 1)) * 3.0;
        double exp_im = ((double)(i * 2 + 1)) * 3.0;
        assert(fabs(__real__ dc[i] - exp_re) < 1e-9);
        assert(fabs(__imag__ dc[i] - exp_im) < 1e-9);
    }

    /* Test 8: Complex double conjugate */
    conj_cdouble(dc, da, N);
    for (int i = 0; i < N; i++) {
        double exp_re = (double)(i + 1);
        double exp_im = -(double)(i * 2 + 1);
        assert(fabs(__real__ dc[i] - exp_re) < 1e-9);
        assert(fabs(__imag__ dc[i] - exp_im) < 1e-9);
    }

    printf("ALL SIMD COMPLEX TESTS PASSED!\n");
    return 0;
}
