int main(void) {
    double a[4];
    double b[4];
    double c[4];
    a[0] = 1.0; a[1] = 2.0; a[2] = 3.0; a[3] = 4.0;
    b[0] = 10.0; b[1] = 20.0; b[2] = 30.0; b[3] = 40.0;
    for (int i = 0; i < 4; i++) {
        c[i] = a[i] + b[i];
    }
    if (c[0] != 11.0) return 1;
    if (c[1] != 22.0) return 2;
    if (c[2] != 33.0) return 3;
    if (c[3] != 44.0) return 4;
    return 0;
}
