/* labels and goto inside the inlined body. */
static int classify(int x) {
    int r;
    if (x > 10) goto big;
    if (x > 5) goto mid;
    r = 0;
    goto out;
mid:
    r = 1;
    goto out;
big:
    r = 2;
out:
    return r;
}
int main(void) {
    if (classify(3) != 0) return 1;
    if (classify(7) != 1) return 1;
    if (classify(20) != 2) return 1;
    return 0;
}
