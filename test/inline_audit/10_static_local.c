/* static local must be shared across call sites (and across inlined copies). */
static int next_id(void) { static int cnt = 100; return cnt++; }
int main(void) {
    int a = next_id();
    int b = next_id();
    int c = next_id();
    if (a != 100 || b != 101 || c != 102) return 1;
    return 0;
}
