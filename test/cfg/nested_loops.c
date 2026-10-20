/* CFG unit test: nested loops (two backedges). */
int main(void) {
    int s = 0, i, j;
    for (i = 0; i < 3; i++) {
        for (j = 0; j < 4; j++) {
            s += i + j;
        }
    }
    return s == 36 ? 0 : 1;
}
