/* same variable passed as two params; callee mutates the first copy.
 * The two args must bind to independent temps (pass by value). */
static int add2(int a, int b) { a += 10; return a + b; }
int main(void) {
    int v = 3;
    int r = add2(v, v);
    if (v != 3 || r != 16) return 1;  /* (3+10) + 3 */
    return 0;
}
