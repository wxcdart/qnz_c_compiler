/* ABI interop s07: gcc-side sret+big-arg provider. */
#include <math.h>
typedef struct { int a; long long b; double c; } S24;
typedef struct { long long a, b; double c; } S24m;

S24 gc_sret_id(S24 s) { s.a -= 2; s.b *= 2; s.c = sqrt(s.c); return s; }
S24m gc_sret_mixed(int pre, S24m m, double post) {
    m.a -= pre; m.b += (long long)post; m.c -= (double)pre * 0.25;
    return m;
}
