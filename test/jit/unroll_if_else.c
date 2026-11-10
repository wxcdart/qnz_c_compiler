// expect-unroll: yes
/* Loop unrolling (Sprint 150): mid-body control flow (if/else).
 * Every in-loop label gets a per-copy clone, so data-dependent branches
 * stay inside one iteration. */
#include <stdio.h>

int e1(void) {
    int s = 0;
    for (int i = 0; i < 6; i++) {
        if (i % 2 == 0) s += i * 2;
        else s -= i;
    }
    return s;
}

int e2(void) {
    int s = 0;
    for (int i = 0; i < 8; i++) {
        if (i > 2) {
            if (i < 6) s += 100;
            else s -= 1;
        } else {
            s += i;
        }
    }
    return s;
}

int e3(void) {
    int a[4] = {1, 2, 3, 4}, s = 0;
    for (int i = 0; i < 4; i++) {
        if (a[i] > 2) a[i] = a[i] * 10;
        s += a[i];
    }
    return s;
}

int main(void) {
    printf("%d %d %d\n", e1(), e2(), e3());
    return 0;
}
