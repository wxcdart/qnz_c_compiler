/* ABI interop s13: gcc-side big-struct provider. */
typedef struct { long long v[32]; } S256;
typedef struct { long long v[128]; } S1K;
typedef struct { long long v[512]; } S4K;

long long gc_sum256(S256 s, long long k) {
    for (int i = 0; i < 32; i++) s.v[i] -= k;
    long long t = 0;
    for (int i = 0; i < 32; i++) t += s.v[i];
    return t;
}
long long gc_half1k(S1K s, long long k) {
    for (int i = 0; i < 128; i++) s.v[i] = s.v[i] / 2 - k;
    long long t = s.v[0] - s.v[127] + s.v[64];
    return t;
}
S4K gc_add4k(S4K s, long long k) {
    for (int i = 0; i < 512; i++) s.v[i] += k;
    return s;
}
