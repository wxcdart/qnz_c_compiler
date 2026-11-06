/* zero_san v1: signed int * overflow must trap (SIGILL, rc 132).
 * 50000 * 100000 = 5e9 > INT_MAX. */
int main(void) {
    int a = 50000, b = 100000;
    return a * b;
}
