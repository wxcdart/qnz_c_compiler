/* CFG unit test: user labels and goto (multi-edge block). */
int main(void) {
    int x = 0, y = 0;
    if (x > 10) goto big;
    if (x > 5) goto mid;
    y = 0;
    goto out;
mid:
    y = 1;
    goto out;
big:
    y = 2;
out:
    y += 3;
    return y == 3 ? 0 : 1;
}
