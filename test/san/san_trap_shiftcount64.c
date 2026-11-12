/* zero_san follow-on (Sprint 166): a 64-bit shift count equal to 64 is UB
 * and must trap (SIGILL, rc 132) at every -O level - exercises the width-8
 * bound (cmp $64 / b.ge vs 64), not the 32-bit one. The count comes from a
 * volatile load so no pass can fold it. */
int main(void) {
    volatile long v = 64L;
    long c = (long)v;
    return (int)(1L << c);
}
