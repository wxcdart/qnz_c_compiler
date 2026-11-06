/* zero_san v1: signed long + overflow must trap (SIGILL, rc 132) via the
 * 64-bit OF-flag check (width 8). 5e18 + 5e18 = 1e19 > LLONG_MAX (~9.22e18). */
int main(void) {
    long a = 5000000000000000000L, b = 5000000000000000000L;
    return (int)(a + b);
}
