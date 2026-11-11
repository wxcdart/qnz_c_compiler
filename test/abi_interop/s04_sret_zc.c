/* ABI interop s04: >16B struct RETURNS (sret) from zerocc functions. */
typedef struct { int a[2]; long long b[2]; double c[2]; } S32;
typedef struct { long long v[6]; int tag; } S48;

S32 zc_make32(long seed) {
    S32 r;
    for (int i = 0; i < 2; i++) {
        r.a[i] = seed + i;
        r.b[i] = seed * 100 + i;
        r.c[i] = (double)i / 2.0 + seed;
    }
    return r;
}
S48 zc_bump48(S48 s, long k) {
    for (int i = 0; i < 6; i++) s.v[i] += k;
    s.tag += (int)k;
    return s;
}
