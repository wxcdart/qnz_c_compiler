struct Point {
    int x;
    int y;
};

int test_fixed_2d_int(void) {
    int a[2][3];
    a[0][0] = 1; a[0][1] = 2; a[0][2] = 3;
    a[1][0] = 4; a[1][1] = 5; a[1][2] = 6;
    return (a[1][2] == 6 && a[0][1] == 2 && a[1][0] == 4);
}

int test_fixed_3d_int(void) {
    int b[2][2][2];
    b[0][0][0] = 10;
    b[0][0][1] = 11;
    b[0][1][0] = 12;
    b[0][1][1] = 13;
    b[1][0][0] = 14;
    b[1][0][1] = 15;
    b[1][1][0] = 16;
    b[1][1][1] = 17;
    return (b[1][1][1] == 17 && b[0][1][0] == 12 && b[1][0][1] == 15);
}

int test_fixed_2d_float(void) {
    float f[2][3];
    f[0][0] = 1.5f;
    f[0][1] = 2.5f;
    f[1][2] = 4.25f;
    return (f[0][0] == 1.5f && f[0][1] == 2.5f && f[1][2] == 4.25f);
}

int test_fixed_2d_struct(void) {
    struct Point p[2][3];
    p[0][0].x = 10; p[0][0].y = 20;
    p[1][2].x = 30; p[1][2].y = 40;
    return (p[0][0].x == 10 && p[0][0].y == 20 && p[1][2].x == 30 && p[1][2].y == 40);
}

int test_vla_2d(int n) {
    int a[n][4];
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < 4; j++) {
            a[i][j] = i * 10 + j;
        }
    }
    int sum = 0;
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < 4; j++) {
            sum += a[i][j];
        }
    }
    return sum;
}

int test_vla_matrix_float(int n) {
    float A[3][3];
    float B[3][3];
    float C[n][3];

    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            A[i][j] = (float)(i + j);
            B[i][j] = (float)(i * j);
            C[i][j] = 0.0f;
        }
    }

    for (int i = 0; i < 3; i++) {
        for (int k = 0; k < 3; k++) {
            for (int j = 0; j < 3; j++) {
                C[i][j] += A[i][k] * B[k][j];
            }
        }
    }
    return (C[1][2] == 16.0f);
}

int main(void) {
    if (!test_fixed_2d_int()) return 1;
    if (!test_fixed_3d_int()) return 2;
    if (!test_fixed_2d_float()) return 3;
    if (!test_fixed_2d_struct()) return 4;
    if (test_vla_2d(3) != 138) return 5;
    if (!test_vla_matrix_float(3)) return 6;
    return 0;
}
