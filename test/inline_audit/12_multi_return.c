/* multiple return points in the inlined body. */
static int signof(int x) {
    if (x < 0) return -1;
    if (x == 0) return 0;
    return 1;
}
int main(void) {
    if (signof(-5) != -1) return 1;
    if (signof(0) != 0) return 1;
    if (signof(5) != 1) return 1;
    return 0;
}
