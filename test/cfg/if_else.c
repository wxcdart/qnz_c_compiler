/* CFG unit test: if/else (branch + two fallthrough regions). */
int classify(int x) {
    if (x > 0) {
        return 1;
    } else if (x < 0) {
        return -1;
    } else {
        return 0;
    }
}
int main(void) {
    int r = classify(0) + classify(5) + classify(-5);
    return r == 0 ? 0 : 1;
}
