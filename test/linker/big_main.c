long fib(int n);
int main(void) {
    long s = 0;
    for (int i = 1; i <= 12; i++) s += fib(i);
    /* 1+1+2+3+5+8+13+21+34+55+89+144 = 376 */
    return s == 376 ? 0 : 1;
}
