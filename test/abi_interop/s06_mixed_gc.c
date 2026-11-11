/* ABI interop s06: gcc-side mixed-args provider. */
typedef struct { long long a, b, c; } S24m;

long long gc_mixed1(int a, S24m m, int b, double d) {
    return (long long)a * 2 + m.a * 3 + m.b - m.c + (long long)b / 2 + (long long)d;
}
long long gc_mixed2(S24m m1, S24m m2, long long z) {
    return m1.c - m2.a + m2.b * 2 - m2.c + z * 3;
}
long long gc_mixed3(S24m x, S24m y, S24m z, int k) {
    return x.b + y.c + z.a - (long long)k * 7;
}
