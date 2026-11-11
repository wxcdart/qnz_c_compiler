/* ABI interop s08: function pointers as args & returns, incl.
 * pointers to functions with big struct params/returns. */
typedef struct { long long a, b; double c; } S24;
S24 zc_za(S24); S24 zc_zb(S24);

int zc_apply(int (*f)(int, int), int x, int y) { return f(x, y) + 1; }
int zc_zadd(int a, int b) { return a * 3 + b; }
S24 (*zc_pick(int k))(S24) { return k ? zc_zb : zc_za; }
S24 zc_za(S24 s) { s.a += 1; s.c += 1.0; return s; }
S24 zc_zb(S24 s) { s.b += 100; s.c += 2.0; return s; }
