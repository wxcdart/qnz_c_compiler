/* CFG unit test: dominator / post-dominator structure (Sprint 149).
 *
 * The behavioural contract is trivial (main returns 0); the point is the
 * CFG SHAPE.  tools/cfg_verifier.py cross-checks the C-side idom /
 * post_idom arrays reported by -dump-cfg against an independent Python
 * computation, so this file exercises the interesting cases:
 *
 *   - fn_nested:      nested loops (two backedges, one header dominating
 *                     the other), break out of the outer loop
 *   - fn_side_entry:  goto out of a loop into the post-loop merge block
 *                     (side entry below the header), early exit path
 *   - fn_multi_exit:  several distinct return blocks (post-dominator
 *                     forks; the pre-fork block post-dominates only
 *                     itself)
 *   - fn_switch_gotos: switch with fallthrough (case 2 -> case 3) and a
 *                     trailing conditional on the merged value
 */
#include <stdio.h>

int fn_nested(int n) {
    int s = 0;
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
            if (j == i) s += i;
            else if (j > i) s -= 1;
            else continue;
        }
        if (s < 0) break;
    }
    return s;
}

int fn_side_entry(int x) {
    int r = 0;
    for (int i = 0; i < 10; i++) {
        if (x == i) {
            r = i * 2;
            goto done;
        }
        r += i;
        if (r > 5) continue;
    }
done:
    if (r < 0) return -r;
    return r;
}

int fn_multi_exit(int a, int b) {
    if (a < 0) return -1;
    if (b < 0) return -2;
    int t = a + b;
    if (t == 0) return 0;
    return t > 0 ? 1 : -3;
}

int fn_switch_gotos(int op) {
    int v = op;
    switch (op) {
        case 1:
            v += 10;
            break;
        case 2:
            v += 20;
            /* fall through */
        case 3:
            v += 30;
            break;
        default:
            v = 0;
    }
    if (v == 60) v = 7;
    return v;
}

int main(void) {
    int r = 0;
    r += fn_nested(4);          /* -3 */
    r += fn_side_entry(3);      /*  6 */
    r += fn_multi_exit(-1, 2);  /* -1 */
    r += fn_multi_exit(2, -1);  /* -2 */
    r += fn_multi_exit(0, 5);   /*  1 */
    r += fn_multi_exit(-3, -4); /* -1 */
    r += fn_switch_gotos(2);    /* 52 */
    r += fn_switch_gotos(4);    /*  0 */
    if (r != 52) return 1;
    printf("dominators ok %d\n", r);
    return 0;
}
