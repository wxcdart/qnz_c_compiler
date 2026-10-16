int main(void) {
    float a[4];
    float b[4];
    float c[4];
    a[0] = 1.0f; a[1] = 2.0f; a[2] = 3.0f; a[3] = 4.0f;
    b[0] = 10.0f; b[1] = 20.0f; b[2] = 30.0f; b[3] = 40.0f;
    for (int i = 0; i < 4; i++) {
        c[i] = a[i] + b[i];
    }
    if (c[0] != 11.0f) return 1;
    if (c[1] != 22.0f) return 2;
    if (c[2] != 33.0f) return 3;
    if (c[3] != 44.0f) return 4;
    return 0;
}
