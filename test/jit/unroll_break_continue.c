// expect-unroll: yes
/* Loop unrolling (Sprint 150): break/continue semantics.
 * Breaks must skip the remaining copies; continues must run the current
 * iteration's step (for-form) or land on the next iteration (while-form). */
#include <stdio.h>

int b1(void) {              /* break at 3 */
    int s = 0;
    for (int i = 0; i < 6; i++) {
        if (i == 3) break;
        s += i;
    }
    return s;
}

int b2(void) {              /* continue at 1, 2 */
    int s = 0;
    for (int i = 0; i < 6; i++) {
        if (i == 1 || i == 2) continue;
        s += i;
    }
    return s;
}

int b3(void) {              /* continue between two adds */
    int s = 0;
    for (int i = 0; i < 5; i++) {
        s += i;
        if (i == 2) continue;
        s += 10;
    }
    return s;
}

int b4(void) {              /* break in the last iteration */
    int s = 0;
    for (int i = 0; i < 4; i++) {
        s += i;
        if (i == 3) break;
    }
    return s;
}

int b5(void) {              /* break immediately */
    int s = 0;
    for (int i = 0; i < 7; i++) {
        if (i == 0) break;
        s += i;
    }
    return s;
}

int main(void) {
    printf("%d %d %d %d %d\n", b1(), b2(), b3(), b4(), b5());
    return 0;
}
