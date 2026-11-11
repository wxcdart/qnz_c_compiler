/* ABI interop s14: gcc-side SSEup 16B struct functions. */
typedef struct { double a; long long b; } M16;
typedef struct { long long a; double b; } M16b;
M16 gc_m16(M16 m, double d) { m.a += d; m.b += 2; return m; }
M16b gc_m16b(M16b n) { n.a -= 2; n.b += 0.5; return n; }
