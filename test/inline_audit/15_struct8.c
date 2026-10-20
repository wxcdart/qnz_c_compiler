/* <=8 byte struct return through inlining (allowed by candidate filter). */
typedef struct { int a; int b; } P;
static P mk(int x) { P p; p.a = x; p.b = x * 2; return p; }
int main(void) {
    P p = mk(21);
    if (p.a != 21 || p.b != 42) return 1;
    return 0;
}
