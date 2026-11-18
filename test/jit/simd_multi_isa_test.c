static float my_fabsf(float x) {
    return x < 0.0f ? -x : x;
}

static double my_fabs(double x) {
    return x < 0.0 ? -x : x;
}

/* Test 1: Vector float operations (4 x 32-bit float = 128-bit) */
static int test_simd_float(void) {
    float a[4] = { 1.5f, 2.5f, 3.5f, 4.5f };
    float b[4] = { 0.5f, 1.5f, 2.5f, 3.5f };
    float c[4];
    float d[4];
    float e[4];

    for (int i = 0; i < 4; i++) {
        c[i] = a[i] + b[i];
    }
    if (my_fabsf(c[0] - 2.0f) > 1e-5f || my_fabsf(c[1] - 4.0f) > 1e-5f ||
        my_fabsf(c[2] - 6.0f) > 1e-5f || my_fabsf(c[3] - 8.0f) > 1e-5f) {
        return 1;
    }

    for (int i = 0; i < 4; i++) {
        d[i] = a[i] - b[i];
    }
    if (my_fabsf(d[0] - 1.0f) > 1e-5f || my_fabsf(d[1] - 1.0f) > 1e-5f ||
        my_fabsf(d[2] - 1.0f) > 1e-5f || my_fabsf(d[3] - 1.0f) > 1e-5f) {
        return 2;
    }

    for (int i = 0; i < 4; i++) {
        e[i] = a[i] * b[i];
    }
    if (my_fabsf(e[0] - 0.75f) > 1e-5f || my_fabsf(e[1] - 3.75f) > 1e-5f ||
        my_fabsf(e[2] - 8.75f) > 1e-5f || my_fabsf(e[3] - 15.75f) > 1e-5f) {
        return 3;
    }

    return 0;
}

/* Test 2: Vector double operations (2 x 64-bit double = 128-bit) */
static int test_simd_double(void) {
    double a[4] = { 10.0, 20.0, 30.0, 40.0 };
    double b[4] = { 2.0, 4.0, 5.0, 8.0 };
    double c[4];
    double d[4];
    double e[4];

    for (int i = 0; i < 4; i++) {
        c[i] = a[i] + b[i];
    }
    if (my_fabs(c[0] - 12.0) > 1e-9 || my_fabs(c[1] - 24.0) > 1e-9 ||
        my_fabs(c[2] - 35.0) > 1e-9 || my_fabs(c[3] - 48.0) > 1e-9) {
        return 10;
    }

    for (int i = 0; i < 4; i++) {
        d[i] = a[i] - b[i];
    }
    if (my_fabs(d[0] - 8.0) > 1e-9 || my_fabs(d[1] - 16.0) > 1e-9 ||
        my_fabs(d[2] - 25.0) > 1e-9 || my_fabs(d[3] - 32.0) > 1e-9) {
        return 11;
    }

    for (int i = 0; i < 4; i++) {
        e[i] = a[i] * b[i];
    }
    if (my_fabs(e[0] - 20.0) > 1e-9 || my_fabs(e[1] - 80.0) > 1e-9 ||
        my_fabs(e[2] - 150.0) > 1e-9 || my_fabs(e[3] - 320.0) > 1e-9) {
        return 12;
    }

    return 0;
}

/* Test 3: Vector int operations (4 x 32-bit int = 128-bit) */
static int test_simd_int(void) {
    int a[4] = { 100, 200, 300, 400 };
    int b[4] = { 10, 20, 30, 40 };
    int c[4];
    int d[4];
    int e[4];

    for (int i = 0; i < 4; i++) {
        c[i] = a[i] + b[i];
    }
    if (c[0] != 110 || c[1] != 220 || c[2] != 330 || c[3] != 440) {
        return 20;
    }

    for (int i = 0; i < 4; i++) {
        d[i] = a[i] - b[i];
    }
    if (d[0] != 90 || d[1] != 180 || d[2] != 270 || d[3] != 360) {
        return 21;
    }

    for (int i = 0; i < 4; i++) {
        e[i] = a[i] * b[i];
    }
    if (e[0] != 1000 || e[1] != 4000 || e[2] != 9000 || e[3] != 16000) {
        return 22;
    }

    return 0;
}

/* Test 4: Heavy vector operations stressing vector register usage */
static int test_simd_spill(void) {
    int a1[4] = {1, 1, 1, 1}, a2[4] = {2, 2, 2, 2};
    int a3[4] = {3, 3, 3, 3}, a4[4] = {4, 4, 4, 4};
    int a5[4] = {5, 5, 5, 5}, a6[4] = {6, 6, 6, 6};
    int a7[4] = {7, 7, 7, 7}, a8[4] = {8, 8, 8, 8};
    int r1[4], r2[4], r3[4], r4[4], r5[4], r6[4], r7[4];

    for (int i = 0; i < 4; i++) r1[i] = a1[i] + a2[i];
    for (int i = 0; i < 4; i++) r2[i] = a3[i] + a4[i];
    for (int i = 0; i < 4; i++) r3[i] = a5[i] + a6[i];
    for (int i = 0; i < 4; i++) r4[i] = a7[i] + a8[i];
    for (int i = 0; i < 4; i++) r5[i] = r1[i] + r2[i];
    for (int i = 0; i < 4; i++) r6[i] = r3[i] + r4[i];
    for (int i = 0; i < 4; i++) r7[i] = r5[i] + r6[i];

    if (r7[0] != 36 || r7[1] != 36 || r7[2] != 36 || r7[3] != 36) return 30;
    return 0;
}

int main(void) {
    int rc;
    if ((rc = test_simd_float()) != 0) return rc;
    if ((rc = test_simd_double()) != 0) return rc;
    if ((rc = test_simd_int()) != 0) return rc;
    if ((rc = test_simd_spill()) != 0) return rc;
    return 0;
}
