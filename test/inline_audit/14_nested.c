/* nested inlining: outer() inlines middle(); middle's body calls leaf(). */
static int leaf(int x) { return x + 1; }
static int middle(int x) { return leaf(x) * 2; }
static int outer(int x) { return middle(x) + 10; }
int main(void) {
    if (outer(1) != 14) return 1;   /* (1+1)*2 + 10 */
    if (middle(5) != 12) return 1;
    if (leaf(9) != 10) return 1;
    return 0;
}
