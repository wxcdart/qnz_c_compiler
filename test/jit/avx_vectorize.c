#include <stdio.h>

void vec_float_add(float *a, float *b, float *c) {
    for (int i = 0; i < 35; i++) {
        c[i] = a[i] + b[i];
    }
}

void vec_double_add(double *a, double *b, double *c) {
    for (int i = 0; i < 19; i++) {
        c[i] = a[i] + b[i];
    }
}

int main(void) {
    float fa[35], fb[35], fc[35];
    for (int i = 0; i < 35; i++) {
        fa[i] = (float)i * 1.5f;
        fb[i] = (float)i * 2.5f;
        fc[i] = 0.0f;
    }

    vec_float_add(fa, fb, fc);

    for (int i = 0; i < 35; i++) {
        if (fc[i] != fa[i] + fb[i]) {
            printf("FAIL float vec [%d]: %f != %f\n", i, fc[i], fa[i] + fb[i]);
            return 1;
        }
    }

    double da[19], db[19], dc[19];
    for (int i = 0; i < 19; i++) {
        da[i] = (double)i * 3.14;
        db[i] = (double)i * 2.71;
        dc[i] = 0.0;
    }

    vec_double_add(da, db, dc);

    for (int i = 0; i < 19; i++) {
        if (dc[i] != da[i] + db[i]) {
            printf("FAIL double vec [%d]: %f != %f\n", i, dc[i], da[i] + db[i]);
            return 2;
        }
    }

    printf("PASS: AVX vectorization\n");
    return 0;
}
