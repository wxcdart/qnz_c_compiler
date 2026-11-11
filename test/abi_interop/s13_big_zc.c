/* ABI interop s13: LARGE by-value struct args (256B / 1KB / 4KB) -
 * exercises the range (non-per-chunk) stack-arg path. */
typedef struct { long long v[32]; } S256;   /* 256B */
typedef struct { long long v[128]; } S1K;   /* 1024B */
typedef struct { long long v[512]; } S4K;   /* 4096B */

long long zc_fill(S256 s, long long k) {
    for (int i = 0; i < 32; i++) s.v[i] += k;
    long long t = 0;
    for (int i = 0; i < 32; i++) t += s.v[i];
    return t;
}
long long zc_fill1k(S1K s, long long k) {
    for (int i = 0; i < 128; i++) s.v[i] = s.v[i] * 2 + k;
    long long t = s.v[0] + s.v[127] + s.v[64];
    return t;
}
S4K zc_flip4k(S4K s, long long k) {
    for (int i = 0; i < 512; i++) s.v[i] = -s.v[i] + k;
    return s;
}
