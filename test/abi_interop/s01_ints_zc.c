/* ABI interop scenario s01: integer/pointer arg & return basics.
 * Compiled by ZEROCC; the provider counterpart is s01_ints_gc.c (gcc). */

int zc_ints(int x, long y, unsigned long long z, const char *s) {
    int n = 0;
    for (; s[n]; n++);
    return (int)(x + y) + (int)(z % 97) + n;
}

long long zc_ll_return(int a, int b) {
    return (long long)a * (long long)b * 1000LL + a - b;
}
