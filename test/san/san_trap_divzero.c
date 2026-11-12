/* zero_san follow-on (Sprint 166): signed int division by a runtime zero
 * must trap (SIGILL, rc 132) at every -O level. The divisor comes from a
 * volatile load so no pass can constant-fold the zero away or prove it
 * non-zero. */
int main(void) {
    volatile int v = 0;
    int d = (int)v;
    return 42 / d;
}
