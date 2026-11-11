/* ABI interop s05: gcc-side big-struct-arg provider. */
typedef struct { int a; long long b; double c; } S24;
typedef struct { int a[2]; long long b[2]; double c[2]; } S32;
typedef struct { long long v[6]; int tag; } S48;

S24 gc_s24(S24 s, int k) { s.a -= k; s.b += 2 * k; s.c -= (double)k * 0.25; return s; }
S32 gc_s32(S32 s, long k) {
    for (int i = 0; i < 2; i++) { s.a[i] -= (int)k; s.b[i] += k * 2; s.c[i] -= 0.25; }
    return s;
}
S48 gc_s48(S48 s, long k) {
    for (int i = 0; i < 6; i++) s.v[i] -= k;
    s.tag -= (int)k * 3;
    return s;
}
long long gc_sum48(const S48 *s) {
    long long t = 0;
    for (int i = 0; i < 6; i++) t += s->v[i];
    return t + s->tag;
}
