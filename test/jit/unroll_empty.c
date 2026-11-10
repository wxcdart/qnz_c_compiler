// expect-unroll: yes
/* Loop unrolling (Sprint 150): empty body (the counter machinery itself
 * is the content; the whole thing may also be dead-code-eliminated -
 * either way the following store must survive). */
#include <stdio.h>

int e1(void) {
    int i = 7;
    for (int k = 0; k < 4; k++) {
        i += k + 1;
    }
    return i;  /* 7+1+2+3+4 = 17 */
}

int e2(void) {
    int i = 0;
    for (int k = 0; k < 6; k++) {
    }
    i = 42;
    return i;
}

int main(void) {
    printf("%d %d\n", e1(), e2());
    return 0;
}
