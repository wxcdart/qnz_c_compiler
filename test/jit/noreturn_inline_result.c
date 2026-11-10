/* Sprint 153 regression: an inlined non-void callee whose body can fall
 * off the end without executing a return (a noreturn body, or a missing
 * return - both UB in C but accepted by gcc) must leave the call result
 * temp DEFINED.
 *
 * The inliner allocates one result temp and expects the inlined body's
 * return to write it; a body that never returns never fires the redirect,
 * so the caller's use of the result was an untracked temp and the
 * opt_temp_renumber backstop rejected the function at -O2 (c-torture
 * compile/pr37913: `int i = bar();` for a noreturn `bar`; also
 * compile/20080613-1, execute/20001124-1). The inlined body's
 * fallthrough now materializes the result temp from VREG_0 - exactly
 * what the non-inlined call path emits after the CALL.
 *
 * The functions below are never CALLED at runtime (the result value is
 * UB); the regression is compile-time lowering, which zerocc performs
 * for the whole TU.
 */
#include <stdio.h>

void user_noreturn_fn(void) __attribute__((noreturn));
void user_noreturn_fn(void) {
    for (;;) {
        /* infinite loop - never returns */
    }
}

static int __attribute__((noreturn)) bar(void) {
    user_noreturn_fn();
}

static int baz(void) {
    int i = bar(); /* UB value; must still compile (bar inlines at -O2) */
    return i;
}

static int qux(int x) {
    if (x) return 1;
    /* missing return on the fall path - UB, accepted */
}

static int use_qux(void) {
    return qux(0) + qux(1);
}

int main(void) {
    printf("ok\n");
    return 0;
}
