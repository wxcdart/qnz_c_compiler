// expect-unroll: no
/* Loop unrolling (Sprint 150): loops that must NOT be unrolled.
 * Every function here has a loop that fails at least one gate; the file
 * as a whole must report zero unrolls and still compute correctly:
 *  - variable bound
 *  - trip count > LU_MAX_TRIP (8)
 *  - call in the body
 *  - conditional increment (not exactly +1 per iteration)
 *  - do-while (post-test backedge)
 *  - counter initialized from a parameter (non-constant init)
 *  - VLA in the body
 */
#include <stdio.h>

int n1(int n) {
    int s = 0;
    for (int i = 0; i < n; i++) s += i;
    return s;
}

int n2(void) {
    int s = 0;
    for (int i = 0; i < 10; i++) s += i;
    return s;  /* 45 */
}

int n3(void) {
    int s = 0;
    for (int i = 0; i < 4; i++) s += n1(i + 1);
    return s;  /* n1(1)+n1(2)+n1(3)+n1(4) = 0+1+3+6 = 10 */
}

int n4(void) {
    int i = 0, s = 0;
    for (; i < 5; ) {
        s += i;
        if (i % 2 == 0) i++;
        else i += 2;
    }
    return s;  /* i: 0->1, 1->3, 3->5: s = 0+1+3 = 4 */
}

int n5(void) {
    int i = 0, s = 0;
    do { s += i * 2; i++; } while (i < 5);
    return s;  /* 0+2+4+6+8 = 20 */
}

int n6(int n) {
    int s = 0;
    while (n < 5) { s += n; n++; }
    return s;  /* n=2: 2+3+4 = 9 */
}

int n7(void) {
    int s = 0;
    int n = 4;
    for (int k = 0; k < 3; k++) {
        int v[n];
        for (int t = 0; t < n; t++) v[t] = t + k;
        s += v[n - 1];
    }
    return s;  /* 3+4+5 = 12 */
}

int main(void) {
    printf("%d %d %d %d %d %d %d\n",
           n1(7), n2(), n3(), n4(), n5(), n6(2), n7());
    return 0;
}
