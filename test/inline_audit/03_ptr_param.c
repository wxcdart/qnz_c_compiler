/* Callee mutates through a pointer argument (legitimate side effect). */
static int addto(int *p, int n) { *p += n; return *p; }
int main(void) {
    int v = 10;
    int r = addto(&v, 7);
    if (v != 17 || r != 17) return 1;
    return 0;
}
