int main(void) {
    int a[4];
    int b[4];
    int c[4];
    int d[4];
    int e[4];

    a[0] = 10; a[1] = 20; a[2] = 30; a[3] = 40;
    b[0] = 1;  b[1] = 2;  b[2] = 3;  b[3] = 4;

    /* Vectorized int addition (paddd / add.4s / vadd.vv) */
    for (int i = 0; i < 4; i++) {
        c[i] = a[i] + b[i];
    }
    if (c[0] != 11) return 1;
    if (c[1] != 22) return 2;
    if (c[2] != 33) return 3;
    if (c[3] != 44) return 4;

    /* Vectorized int subtraction (psubd / sub.4s / vsub.vv) */
    for (int i = 0; i < 4; i++) {
        d[i] = a[i] - b[i];
    }
    if (d[0] != 9)  return 5;
    if (d[1] != 18) return 6;
    if (d[2] != 27) return 7;
    if (d[3] != 36) return 8;

    /* Vectorized int multiplication (pmulld / mul.4s / vmul.vv) */
    for (int i = 0; i < 4; i++) {
        e[i] = a[i] * b[i];
    }
    if (e[0] != 10)  return 9;
    if (e[1] != 40)  return 10;
    if (e[2] != 90)  return 11;
    if (e[3] != 160) return 12;

    return 0;
}
