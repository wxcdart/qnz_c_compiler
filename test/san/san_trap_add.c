/* zero_san v1: signed int + overflow must trap (SIGILL, rc 132) under
 * -fsanitize=undefined. 1.5e9 + 1.5e9 = 3e9 > INT_MAX. */
int main(void) {
    int a = 1500000000, b = 1500000000;
    return a + b;
}
