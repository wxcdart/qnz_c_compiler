/* Callee takes the address of a value parameter and writes through it. */
static int dbl(int x) { int *p = &x; *p = *p * 3; return *p + 1; }
int main(void) {
    int v = 4;
    int r = dbl(v);
    if (v != 4 || r != 13) return 1;  /* v untouched (pass by value), r = 4*3+1 */
    return 0;
}
