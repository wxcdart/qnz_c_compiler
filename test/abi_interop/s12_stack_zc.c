/* ABI interop s12: stack discipline - big zerocc frames under gcc leaf
 * callers (red-zone/128-byte alignment), cross-boundary recursion. */
int gc_recurse_g(int);
int zc_bigframe(int x) {
    long long f[16];           /* 128-byte frame */
    long long t = x;
    for (int i = 0; i < 16; i++) { f[i] = t * (i + 1); t += f[i] % 7; }
    return (int)(t + f[3] % 13 + f[15] % 17);
}
int zc_recurse_z(int n) {
    if (n <= 0) return 1;
    return n + gc_recurse_g(n - 1);
}
