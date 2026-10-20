/* inlined call mixed with a normal (non-inlineable) call in one expression;
 * large callee stays a real call. */
static int small(int x) { return x + 1; }
static int big(int x) {
    int s = 0, i;
    for (i = 0; i < x; i++) s += i * i;
    return s;
}
int main(void) {
    int r = small(1) + big(4) + small(2);  /* 2 + (0+1+4+9) + 3 = 19 */
    if (r != 19) return 1;
    return 0;
}
