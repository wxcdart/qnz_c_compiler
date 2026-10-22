/* Five distinct faults across three functions — the recovery target
 * file: each fault must be diagnosed in a single compile. */
struct P { int x; int y; };
int add(int a, int b) { return a + b; }
int f1(void) {
    struct P p;
    p.y2 = 1;              /* fault 1: 'P' has no member 'y2' */
    return add(1);         /* fault 2: too few arguments to add */
}
int f2(void) {
    int x = 1;
    if (x) goto nowhere;   /* fault 3: label 'nowhere' not defined */
    int y;
    y->x = 2;              /* fault 4: -> on a non-pointer */
    return y;
}
int main(void) {
    int z = 2 return z;    /* fault 5: missing ';' */
}
