/* long arg passed to int param (implementation-defined narrowing);
 * use values that fit so the expectation is well-defined. */
static int low32(long x) { return (int)(x & 0x7fffffffL); }
int main(void) {
    int r = low32(0x12345L);
    if (r != 0x12345) return 1;
    return 0;
}
