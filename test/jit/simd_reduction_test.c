/* Test SIMD Vector Horizontal Reductions (Sprint 210)
 * Tests auto-vectorization of sum += a[i] horizontal reductions for
 * integer, single-precision float, and double-precision float.
 */

int printf(const char *fmt, ...);

int sum_i_exact(const int *a) {
    int sum = 0;
    for (int i = 0; i < 100; i++) {
        sum += a[i];
    }
    return sum;
}

int sum_i_peel(const int *a) {
    int sum = 0;
    for (int i = 0; i < 102; i++) {
        sum += a[i];
    }
    return sum;
}

int sum_i_offset(const int *a) {
    int sum = 10;
    for (int i = 0; i < 100; i++) {
        sum += a[i];
    }
    return sum;
}

float sum_f_exact(const float *a) {
    float sum = 0.0f;
    for (int i = 0; i < 100; i++) {
        sum += a[i];
    }
    return sum;
}

float sum_f_peel(const float *a) {
    float sum = 0.0f;
    for (int i = 0; i < 102; i++) {
        sum += a[i];
    }
    return sum;
}

float sum_f_offset(const float *a) {
    float sum = 5.5f;
    for (int i = 0; i < 100; i++) {
        sum += a[i];
    }
    return sum;
}

double sum_d_exact(const double *a) {
    double sum = 0.0;
    for (int i = 0; i < 100; i++) {
        sum += a[i];
    }
    return sum;
}

double sum_d_peel(const double *a) {
    double sum = 0.0;
    for (int i = 0; i < 101; i++) {
        sum += a[i];
    }
    return sum;
}

double sum_d_offset(const double *a) {
    double sum = 20.25;
    for (int i = 0; i < 100; i++) {
        sum += a[i];
    }
    return sum;
}

int main(void) {
    int ai[110];
    float af[110];
    double ad[110];

    for (int i = 0; i < 110; i++) {
        ai[i] = i + 1;
        af[i] = (float)(i + 1);
        ad[i] = (double)(i + 1);
    }

    int res_ie = sum_i_exact(ai);
    int res_ip = sum_i_peel(ai);
    int res_io = sum_i_offset(ai);

    float res_fe = sum_f_exact(af);
    float res_fp = sum_f_peel(af);
    float res_fo = sum_f_offset(af);

    double res_de = sum_d_exact(ad);
    double res_dp = sum_d_peel(ad);
    double res_do = sum_d_offset(ad);

    int exp_ie = 100 * 101 / 2;
    int exp_ip = 102 * 103 / 2;
    int exp_io = 10 + 100 * 101 / 2;

    float exp_fe = 100.0f * 101.0f / 2.0f;
    float exp_fp = 102.0f * 103.0f / 2.0f;
    float exp_fo = 5.5f + 100.0f * 101.0f / 2.0f;

    double exp_de = 100.0 * 101.0 / 2.0;
    double exp_dp = 101.0 * 102.0 / 2.0;
    double exp_do = 20.25 + 100.0 * 101.0 / 2.0;

    if (res_ie != exp_ie) { printf("FAIL: sum_i_exact got %d, exp %d\n", res_ie, exp_ie); return 1; }
    if (res_ip != exp_ip) { printf("FAIL: sum_i_peel got %d, exp %d\n", res_ip, exp_ip); return 2; }
    if (res_io != exp_io) { printf("FAIL: sum_i_offset got %d, exp %d\n", res_io, exp_io); return 3; }

    if (res_fe != exp_fe) { printf("FAIL: sum_f_exact got %f, exp %f\n", (double)res_fe, (double)exp_fe); return 4; }
    if (res_fp != exp_fp) { printf("FAIL: sum_f_peel got %f, exp %f\n", (double)res_fp, (double)exp_fp); return 5; }
    if (res_fo != exp_fo) { printf("FAIL: sum_f_offset got %f, exp %f\n", (double)res_fo, (double)exp_fo); return 6; }

    if (res_de != exp_de) { printf("FAIL: sum_d_exact got %f, exp %f\n", res_de, exp_de); return 7; }
    if (res_dp != exp_dp) { printf("FAIL: sum_d_peel got %f, exp %f\n", res_dp, exp_dp); return 8; }
    if (res_do != exp_do) { printf("FAIL: sum_d_offset got %f, exp %f\n", res_do, exp_do); return 9; }

    printf("ALL SIMD REDUCTION TESTS PASSED!\n");
    return 0;
}
