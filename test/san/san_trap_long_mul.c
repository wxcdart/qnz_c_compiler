/* zero_san v1: signed long * overflow must trap (SIGILL, rc 132).
 * 4e9 * 4e9 = 1.6e19 > LLONG_MAX. */
int main(void) {
    long a = 4000000000L, b = 4000000000L;
    return (int)(a * b);
}
