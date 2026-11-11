/* zero_san v1 (Sprint 161): compound *= signed long overflow must trap
 * (SIGILL, rc 132) via the 64-bit OF-flag check (width 8).
 * 4e9 * 4e9 = 1.6e19 > LLONG_MAX. */
int main(void) {
    long x = 4000000000L;
    x *= 4000000000L;    /* overflows long: must trap */
    (void)x;
    return 0;
}
