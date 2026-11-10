// expect-unroll: yes
/* Loop unrolling (Sprint 150): coexistence with user labels and nested
 * functions.
 *  - l1: a user label INSIDE the loop -> the loop must NOT unroll
 *    (a nested function could nonlocal-goto the label; the label's
 *    per-copy identity would break the goto).
 *  - nl1: a nested function + a small loop: the loop may unroll, the
 *    nested function must still work.
 * (The nonlocal-goto coexistence case lives in unroll_nonlocal_goto.c -
 * gcc rejects cross-nested-function gotos, so that file cannot be part
 * of the host-gcc test suite.) */
#include <stdio.h>

int l1(void) {
    int s = 0;
    for (int i = 0; i < 5; i++) {
        if (i == 2) goto top;
        s += i;
    top:
        ;
    }
    return s;
}

int nl1(void) {
    int s = 0;
    for (int i = 0; i < 4; i++) s += i;
    int inner(void) {
        s += 100;
        return 0;
    }
    inner();
    return s;
}

int main(void) {
    printf("%d %d\n", l1(), nl1());
    return 0;
}
