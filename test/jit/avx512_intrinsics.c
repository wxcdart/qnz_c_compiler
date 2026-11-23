/* test/jit/avx512_intrinsics.c - AVX-512 foundation intrinsics test */
#include <stdio.h>
#include <immintrin.h>

static float diff_f(float a, float b) {
    float d = a - b;
    return d < 0 ? -d : d;
}

static double diff_d(double a, double b) {
    double d = a - b;
    return d < 0 ? -d : d;
}

#ifndef __ZEROCC__
__attribute__((target("avx512f,avx512dq")))
#endif
static int run_avx512_tests(void) {
    /* 1. 512-bit float arithmetic */
    float fa[16], fb[16], fc[16];
    for (int i = 0; i < 16; i++) {
        fa[i] = (float)(i + 1);
        fb[i] = (float)(i * 2 + 1);
    }

    __m512 va = _mm512_loadu_ps(fa);
    __m512 vb = _mm512_loadu_ps(fb);
    __m512 vadd = _mm512_add_ps(va, vb);
    __m512 vsub = _mm512_sub_ps(vb, va);
    __m512 vmul = _mm512_mul_ps(va, vb);
    __m512 vdiv = _mm512_div_ps(vb, va);

    _mm512_storeu_ps(fc, vadd);
    for (int i = 0; i < 16; i++) {
        float expected = fa[i] + fb[i];
        if (diff_f(fc[i], expected) > 1e-5f) {
            printf("FAIL: vaddps[%d] got %f expected %f\n", i, fc[i], expected);
            return 1;
        }
    }

    _mm512_storeu_ps(fc, vsub);
    for (int i = 0; i < 16; i++) {
        float expected = fb[i] - fa[i];
        if (diff_f(fc[i], expected) > 1e-5f) {
            printf("FAIL: vsubps[%d] got %f expected %f\n", i, fc[i], expected);
            return 2;
        }
    }

    _mm512_storeu_ps(fc, vmul);
    for (int i = 0; i < 16; i++) {
        float expected = fa[i] * fb[i];
        if (diff_f(fc[i], expected) > 1e-5f) {
            printf("FAIL: vmulps[%d] got %f expected %f\n", i, fc[i], expected);
            return 3;
        }
    }

    _mm512_storeu_ps(fc, vdiv);
    for (int i = 0; i < 16; i++) {
        float expected = fb[i] / fa[i];
        if (diff_f(fc[i], expected) > 1e-5f) {
            printf("FAIL: vdivps[%d] got %f expected %f\n", i, fc[i], expected);
            return 4;
        }
    }

    /* 2. 512-bit double arithmetic */
    double da[8], db[8], dc[8];
    for (int i = 0; i < 8; i++) {
        da[i] = (double)(i + 1) * 1.5;
        db[i] = (double)(i * 3 + 2) * 0.5;
    }

    __m512d vda = _mm512_loadu_pd(da);
    __m512d vdb = _mm512_loadu_pd(db);
    __m512d vdadd = _mm512_add_pd(vda, vdb);
    __m512d vdsub = _mm512_sub_pd(vda, vdb);
    __m512d vdmul = _mm512_mul_pd(vda, vdb);
    __m512d vddiv = _mm512_div_pd(vda, vdb);

    _mm512_storeu_pd(dc, vdadd);
    for (int i = 0; i < 8; i++) {
        double expected = da[i] + db[i];
        if (diff_d(dc[i], expected) > 1e-9) {
            printf("FAIL: vaddpd[%d] got %f expected %f\n", i, dc[i], expected);
            return 5;
        }
    }

    _mm512_storeu_pd(dc, vdsub);
    for (int i = 0; i < 8; i++) {
        double expected = da[i] - db[i];
        if (diff_d(dc[i], expected) > 1e-9) {
            printf("FAIL: vsubpd[%d] got %f expected %f\n", i, dc[i], expected);
            return 6;
        }
    }

    _mm512_storeu_pd(dc, vdmul);
    for (int i = 0; i < 8; i++) {
        double expected = da[i] * db[i];
        if (diff_d(dc[i], expected) > 1e-9) {
            printf("FAIL: vmulpd[%d] got %f expected %f\n", i, dc[i], expected);
            return 7;
        }
    }

    _mm512_storeu_pd(dc, vddiv);
    for (int i = 0; i < 8; i++) {
        double expected = da[i] / db[i];
        if (diff_d(dc[i], expected) > 1e-9) {
            printf("FAIL: vdivpd[%d] got %f expected %f\n", i, dc[i], expected);
            return 8;
        }
    }

    /* 3. Bitwise logic */
    __m512 v1 = _mm512_set1_ps(3.0f);
    __m512 vxor = _mm512_xor_ps(v1, v1);
    _mm512_storeu_ps(fc, vxor);
    for (int i = 0; i < 16; i++) {
        if (fc[i] != 0.0f) {
            printf("FAIL: vxorps[%d] got %f expected 0.0\n", i, fc[i]);
            return 9;
        }
    }

    /* 4. Set1 and setzero */
    __m512 vzero = _mm512_setzero_ps();
    _mm512_storeu_ps(fc, vzero);
    for (int i = 0; i < 16; i++) {
        if (fc[i] != 0.0f) {
            printf("FAIL: setzero_ps[%d] got %f expected 0.0\n", i, fc[i]);
            return 10;
        }
    }

    __m512d vdzero = _mm512_setzero_pd();
    _mm512_storeu_pd(dc, vdzero);
    for (int i = 0; i < 8; i++) {
        if (dc[i] != 0.0) {
            printf("FAIL: setzero_pd[%d] got %f expected 0.0\n", i, dc[i]);
            return 11;
        }
    }

    printf("AVX-512 intrinsics OK\n");
    return 0;
}

int main(void) {
#ifndef __ZEROCC__
    __builtin_cpu_init();
    if (!__builtin_cpu_supports("avx512f")) {
        printf("AVX-512 not supported by host CPU (skipped)\n");
        return 0;
    }
#endif
    return run_avx512_tests();
}
