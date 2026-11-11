/* ABI interop s08: gcc-side funcptr provider. */
typedef struct { long long a, b; double c; } S24;

int gc_apply(int (*f)(int, int), int x, int y) { return f(x, y) * 2; }
int gc_gmul(int a, int b) { return a * b; }
S24 gc_transform(S24 s) { s.a *= 2; s.b -= 50; s.c -= 1.0; return s; }
