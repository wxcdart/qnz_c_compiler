/* ABI interop scenario s01: gcc-side provider (integer/pointer basics). */

int gc_ints(int x, long y, unsigned long long z, const char *s) {
    int n = 0;
    for (; s[n]; n++);
    return (int)(x + y) + (int)(z % 89) + n;
}

long long gc_ll_return(int a, int b) {
    return (long long)a * (long long)b * 100LL + a * 7 - b;
}
