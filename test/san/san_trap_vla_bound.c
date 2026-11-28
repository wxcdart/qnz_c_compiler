/* zero_san UBSan (Sprint 266): non-positive VLA dimension evaluates to non-positive
 * value must trap (SIGILL, rc 132) at every -O level. */
int main(void) {
    volatile int n = -5;
    int vla[n];
    vla[0] = 42;
    (void)vla[0];
    return 0;
}
