// expect-unroll: yes
/* Loop unrolling (Sprint 150): while-loop shapes.
 * w1/w3 have the increment as the final statement (backedge block) and
 * unroll; w2's increment sits mid-body (before a conditional) and must
 * stay a loop - either way the result must match. */
#include <stdio.h>

int w1(void) {
    int i = 0, s = 0;
    while (i < 5) { s += i * 2 + 1; i++; }
    return s;  /* 1+3+5+7+9 = 25 */
}

int w2(void) {
    int i = 0, s = 0;
    while (i < 6) {
        i++;
        if (i == 3) continue;
        s += i;
    }
    return s;  /* 1+2+4+5+6 = 18 */
}

int w3(void) {
    int i = 0, s = 100;
    while (i < 4) {
        s += i * i;
        i += 1;
    }
    return s;  /* 100 + 0+1+4+9 = 114 */
}

int main(void) {
    printf("%d %d %d\n", w1(), w2(), w3());
    return 0;
}
