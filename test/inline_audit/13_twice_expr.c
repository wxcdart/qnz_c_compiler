/* same callee inlined twice in one expression; args must evaluate correctly. */
static int sq(int x) { return x * x; }
int main(void) {
    int r = sq(3) + sq(4);      /* 9 + 16 */
    int s = sq(sq(2) - 1) - 1;  /* sq(3) - 1 = 8 */
    if (r != 25 || s != 8) return 1;
    return 0;
}
