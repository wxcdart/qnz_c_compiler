// expect-unroll: no
/* Loop unrolling (Sprint 150): nonlocal goto (Sprint 99) coexistence.
 * A nested function nonlocal-gotoes a label OUTSIDE the loop (after it).
 * The loop carries the nested-function call in its body, so it must
 * stay a loop (the span gate never unrolls loops containing calls);
 * the goto must still land at the label after optimization.
 *
 * EXCLUDED from the host-gcc test binary glob (Makefile TEST_SRCS, like
 * test/jit/builtin_apply.c): gcc rejects cross-nested-function gotos, so
 * only zerocc can build this. Exercised by tools/loop_unroll_verifier.py
 * (falls back to the -O0 baseline) and the test-opt sweep (gcc reference
 * unavailable). */
#include <stdio.h>

int nl2(void) {
    int s = 5;
    int i = 0;
top:
    for (; i < 3; i++) {
        if (i == 2) {
            int jumpout(void) {
                goto done;  /* land AFTER the loop - terminates */
            }
            jumpout();
        }
        s += i * 10;
    }
done:
    return s;  /* i=0: s=5; i=1: s=15; i=2: jumpout -> done: 15 */
}

int main(void) {
    printf("%d\n", nl2());
    return 0;
}
