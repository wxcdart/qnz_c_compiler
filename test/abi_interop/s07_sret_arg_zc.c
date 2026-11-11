/* ABI interop s07: sret return AND big struct arg in the same call
 * (RDI=sret pointer, struct value on the caller stack) + big arg
 * between scalars with sret. */
typedef struct { int a; long long b; double c; } S24;
typedef struct { long long a, b; double c; } S24m;

S24 zc_sret_id(S24 s) { s.a += 1; s.b += 10; s.c += 0.25; return s; }
S24m zc_sret_mixed(int pre, S24m m, double post) {
    m.a += pre; m.b -= post; m.c += (double)pre * 0.5;
    return m;
}
