/* ABI interop s03: gcc-side small-struct provider. */
#include <math.h>
typedef struct { int a; long long b; } S8;
typedef struct { int a; int b; char c; } S9;
typedef struct { int a, b, c, d; } S16;
typedef struct { double a, b; } S16f;

S8 gc_s8(S8 s, int k) { s.a -= k; s.b += 3 * k; return s; }
S9 gc_s9(S9 s) { s.a -= 5; s.b += 7; s.c = (char)(s.c - 1); return s; }
S16 gc_s16(S16 s, int k) { s.a -= k; s.b += k * 2; s.c -= 4; s.d += 5; return s; }
S16f gc_s16f(S16f s) { s.a = sqrt(s.a); s.b -= 0.25; return s; }
