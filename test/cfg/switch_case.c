/* CFG unit test: switch (many edges into one block). */
int map(int k) {
    int r;
    switch (k) {
        case 1: r = 10; break;
        case 2: r = 20; break;
        case 3: r = 30; break;
        default: r = 0; break;
    }
    return r;
}
int main(void) {
    int s = map(1) + map(2) + map(3) + map(9);
    return s == 60 ? 0 : 1;
}
