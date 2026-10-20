/* Narrow signed args (char/short) must be sign-extended to int params. */
static int sum3(char a, short b, int c) { return a + b + c; }
int main(void) {
    int r = sum3((char)-3, (short)-50, 100);
    if (r != 47) return 1;  /* -3 - 50 + 100 */
    return 0;
}
