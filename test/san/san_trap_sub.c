/* zero_san v1: signed int - overflow must trap (SIGILL, rc 132).
 * INT_MIN - 1. (The INT_MIN literal is built from -2147483647 - 1, which
 * itself is in range and constant-folds cleanly.) */
int main(void) {
    int a = -2147483647 - 1, b = 1;
    return a - b;
}
