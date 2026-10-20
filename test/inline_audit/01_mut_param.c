/* Callee mutates a value parameter: must not affect the caller's variable. */
static int bump(int x) { x += 100; return x * 2; }
int main(void) {
    int v = 5;
    int r = bump(v);
    if (v != 5 || r != 210) return 1;
    return 0;
}
