/* zero_san follow-on (Sprint 166): a shift count equal to the operand bit
 * width (32 for int) is UB and must trap (SIGILL, rc 132) at every -O
 * level. The count comes from a volatile load so no pass can fold it. */
int main(void) {
    volatile int v = 32;
    int c = (int)v;
    return 1 << c;
}
