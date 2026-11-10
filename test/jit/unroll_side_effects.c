// expect-unroll: yes
/* Loop unrolling (Sprint 150): side effects in the body - global writes,
 * pointer stores, early return. Each effect must run exactly once per
 * iteration (never N times, never zero times). */
#include <stdio.h>

static int g = 0;

int s1(void) {
    for (int i = 0; i < 4; i++) g += i * 3;
    return g;  /* 0+3+6+9 = 18 */
}

int s2(int *p) {
    for (int i = 0; i < 5; i++) p[i] = i * i;
    return p[1] + p[4];  /* 1 + 16 = 17 */
}

int s3(void) {
    int s = 0;
    for (int i = 0; i < 8; i++) {
        if (i == 4) return s + 100;  /* early return skips later copies */
        s += i;
    }
    return s;  /* 0+1+2+3 = 6, then return 106 */
}

int main(void) {
    int buf[5] = {0, 0, 0, 0, 0};
    printf("%d %d %d\n", s1(), s2(buf), s3());
    return 0;
}
