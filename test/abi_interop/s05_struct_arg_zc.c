/* ABI interop s05: >16B struct ARGS by value (System V memory args). */
typedef struct { int a; long long b; double c; } S24;   /* 24B */
typedef struct { int a[2]; long long b[2]; double c[2]; } S32; /* 32B */
typedef struct { long long v[6]; int tag; } S48;        /* 48B */
typedef struct { double m[5]; long long id; } S64;      /* 64B? 40+8 -> 48... make 64 */

S24 zc_s24(S24 s, int k) { s.a += k; s.b -= k; s.c += (double)k * 0.5; return s; }
S32 zc_s32(S32 s, long k) {
    for (int i = 0; i < 2; i++) { s.a[i] += (int)k; s.b[i] -= k; s.c[i] += 0.5; }
    return s;
}
S48 zc_s48(S48 s, long k) {
    for (int i = 0; i < 6; i++) s.v[i] += k * (i + 1);
    s.tag += (int)k;
    return s;
}
int zc_sum24(const S24 *s) { return s->a + (int)(s->b % 13) + (int)(s->c * 10.0); }
