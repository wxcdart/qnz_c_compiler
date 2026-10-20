/* CFG unit test: straight-line code (single block). */
int main(void) {
    int a = 1, b = 2, c = 3;
    int d = a + b;
    int e = d * c;
    return e == 9 ? 0 : 1;
}
