/* Int return used in long context; negative values included. */
static int neg(void) { return -42; }
int main(void) {
    long l = (long)neg() * 100L;
    if (l != -4200L) return 1;
    return 0;
}
