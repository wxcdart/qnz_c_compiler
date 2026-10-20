/* CFG unit test: inlined ternary in a loop (loop + branch inside). */
int main(void) {
    int s = 0, i;
    for (i = 0; i < 5; i++) {
        s += (i % 2 == 0) ? i * 10 : i;
    }
    return s == 0 + 1 + 20 + 3 + 40 ? 0 : 1;
}
