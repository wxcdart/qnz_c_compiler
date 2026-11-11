/* ABI interop s14: zerocc-side SSEup 16B struct functions. */
typedef struct { double a; long long b; } M16;
typedef struct { long long a; double b; } M16b;
M16 zc_m16(M16 m, double d) { m.a += d; m.b += 1; return m; }
M16b zc_m16b(M16b n) { n.a -= 1; n.b *= 2.0; return n; }
