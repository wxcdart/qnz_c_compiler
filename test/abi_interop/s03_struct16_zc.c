/* ABI interop s03: 8/9/12/16B by-value structs (register-pair class). */
typedef struct { int a; long long b; } S8;      /* 16B? 4+4pad+8 = 16 */
typedef struct { int a; int b; char c; } S9;    /* 9B */
typedef struct { long long a; int b; char c; } S12; /* 8+4+1 -> 16? 8+4+1+3pad=16... want 12: use below */
typedef struct { double d; float f; } S12b;     /* 8+4+4pad = 16... */
typedef struct { int a, b, c, d; } S16;         /* 16B */
typedef struct { double a, b; } S16f;           /* 16B SSE */

S8 zc_s8_in_out(S8 s, int k) { s.a += k; s.b -= 2 * k; return s; }
S9 zc_s9(S9 s) { s.a += 1; s.b -= 2; s.c = (char)(s.c + 3); return s; }
S16 zc_s16(S16 s, int k) { s.a += k; s.b -= k; s.c += 1; s.d -= 3; return s; }
S16f zc_s16f(S16f s) { s.a += 0.5; s.b = s.b * 2.0; return s; }
