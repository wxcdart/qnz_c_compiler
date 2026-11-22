#pragma GCC target("avx")
#include <stdio.h>
#include <immintrin.h>

int main(void) {
    /* 1. Float 8-lane operations */
    float a[8] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f};
    float b[8] = {10.0f, 20.0f, 30.0f, 40.0f, 50.0f, 60.0f, 70.0f, 80.0f};
    float c[8] = {0};

    __m256 va = _mm256_loadu_ps(a);
    __m256 vb = _mm256_loadu_ps(b);

    /* Test ADD */
    __m256 vadd = _mm256_add_ps(va, vb);
    _mm256_storeu_ps(c, vadd);
    for (int i = 0; i < 8; i++) {
        if (c[i] != a[i] + b[i]) {
            printf("FAIL add_ps [%d]: got %f, expected %f\n", i, c[i], a[i] + b[i]);
            return 1;
        }
    }

    /* Test SUB */
    __m256 vsub = _mm256_sub_ps(vb, va);
    _mm256_storeu_ps(c, vsub);
    for (int i = 0; i < 8; i++) {
        if (c[i] != b[i] - a[i]) {
            printf("FAIL sub_ps [%d]: got %f, expected %f\n", i, c[i], b[i] - a[i]);
            return 2;
        }
    }

    /* Test MUL */
    __m256 vmul = _mm256_mul_ps(va, vb);
    _mm256_storeu_ps(c, vmul);
    for (int i = 0; i < 8; i++) {
        if (c[i] != a[i] * b[i]) {
            printf("FAIL mul_ps [%d]: got %f, expected %f\n", i, c[i], a[i] * b[i]);
            return 3;
        }
    }

    /* Test DIV */
    __m256 vdiv = _mm256_div_ps(vb, va);
    _mm256_storeu_ps(c, vdiv);
    for (int i = 0; i < 8; i++) {
        if (c[i] != 10.0f) {
            printf("FAIL div_ps [%d]: got %f, expected 10.0\n", i, c[i]);
            return 4;
        }
    }

    /* Test SET1 */
    __m256 vset1 = _mm256_set1_ps(42.5f);
    _mm256_storeu_ps(c, vset1);
    for (int i = 0; i < 8; i++) {
        if (c[i] != 42.5f) {
            printf("FAIL set1_ps [%d]: got %f, expected 42.5\n", i, c[i]);
            return 5;
        }
    }

    /* 2. Double 4-lane operations */
    double da[4] = {1.5, 2.5, 3.5, 4.5};
    double db[4] = {10.0, 20.0, 30.0, 40.0};
    double dc[4] = {0};

    __m256d vda = _mm256_loadu_pd(da);
    __m256d vdb = _mm256_loadu_pd(db);
    __m256d vdadd = _mm256_add_pd(vda, vdb);
    _mm256_storeu_pd(dc, vdadd);
    for (int i = 0; i < 4; i++) {
        if (dc[i] != da[i] + db[i]) {
            printf("FAIL add_pd [%d]: got %f, expected %f\n", i, dc[i], da[i] + db[i]);
            return 6;
        }
    }

    __m256d vdsub = _mm256_sub_pd(vdb, vda);
    _mm256_storeu_pd(dc, vdsub);
    for (int i = 0; i < 4; i++) {
        if (dc[i] != db[i] - da[i]) {
            printf("FAIL sub_pd [%d]: got %f, expected %f\n", i, dc[i], db[i] - da[i]);
            return 7;
        }
    }

    __m256d vdmul = _mm256_mul_pd(vda, vdb);
    _mm256_storeu_pd(dc, vdmul);
    for (int i = 0; i < 4; i++) {
        if (dc[i] != da[i] * db[i]) {
            printf("FAIL mul_pd [%d]: got %f, expected %f\n", i, dc[i], da[i] * db[i]);
            return 8;
        }
    }

    /* Test zeroupper */
    _mm256_zeroupper();

    printf("PASS: AVX intrinsics\n");
    return 0;
}
