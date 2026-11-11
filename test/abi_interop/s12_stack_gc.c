/* ABI interop s12: gcc-side stack-discipline provider. */
int zc_bigframe(int);
int zc_recurse_z(int);
int gc_leaf_locals(int x) {
    /* leaf: 10 locals live in the red zone (no call before use) */
    int l0 = x, l1 = x + 1, l2 = x + 2, l3 = x + 3, l4 = x + 4;
    int l5 = x + 5, l6 = x + 6, l7 = x + 7, l8 = x + 8, l9 = x + 9;
    int r = zc_bigframe(l0 + l9);   /* call into zerocc big frame */
    return r + l1 + l4 + l7 + l9;
}
int gc_recurse_g(int n) {
    if (n <= 0) return 2;
    return n * 2 + zc_recurse_z(n - 1);
}
int gc_wrap_big(int a, int b) {
    int save = a + b;
    int r = zc_bigframe(save);
    return r - save;
}
