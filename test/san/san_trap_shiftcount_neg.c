/* zero_san follow-on (Sprint 166): a NEGATIVE shift count is UB and must
 * trap (SIGILL, rc 132) at every -O level - exercises the count < 0 half
 * of the check (the >= width half is san_trap_shiftcount.c). The count
 * comes from a volatile load so no pass can fold it. */
int main(void) {
    volatile int v = -1;
    int c = (int)v;
    return -8 >> c;
}
