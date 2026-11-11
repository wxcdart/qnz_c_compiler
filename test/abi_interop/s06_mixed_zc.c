/* ABI interop s06: big struct args mixed with scalar args (register
 * streams skip the memory arg), multiple big struct args (packing). */
typedef struct { long long a, b; int c; } S20;   /* 24B w/ padding? 8+8+4=20 -> 24? no: align8 -> 24. hmm keep */
typedef struct { long long a, b, c; } S24m;      /* 24B */

/* memory arg between register args: a=RDI, m=stack, b=RSI, d=XMM0 */
long long zc_mixed1(int a, S24m m, int b, double d) {
    return (long long)a + m.a + m.b + m.c + (long long)b + (long long)d;
}
/* two memory args + trailing int: m1@RSP+0, m2@RSP+24, z=RDI */
long long zc_mixed2(S24m m1, S24m m2, long long z) {
    return m1.a + m1.b + m1.c + m2.a + m2.b + m2.c + z;
}
/* three memory args */
long long zc_mixed3(S24m x, S24m y, S24m z, int k) {
    return x.a + y.b + z.c + (long long)k;
}
