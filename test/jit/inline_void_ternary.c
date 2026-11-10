/* Regression: inlined void-call expression value (c-torture
 * compile/pr39394).
 *
 * An inlined call's result temp (`inline_res_temp`) must be DEFINED on
 * every path reaching the inline exit label: the caller may read the
 * call expression's "value" even for a void callee - the ternary
 * operator MOVs each arm's value into its result temp
 * (`x ? f1() : f1()`).  Before the fix, an inline void body that
 * exits via a bare `return;` (jumping over the fallthrough
 * materialization) or falls off the end left the temp undefined, and
 * the liveness renumber pass's untracked-temp backstop rejected the
 * function: "internal error: untracked temp N in function 'f2'".
 *
 * The non-inlined call path materializes VREG_0 after the CALL in
 * all cases; the inlined path now mirrors it (fallthrough + inlined
 * void returns).
 */
#include <stdio.h>

static void sink(void) {
    /* Empty inline-able body: the inlined expansion emits nothing,
     * so only the materialization defines the result temp. */
}

static void maybe_return(int n) {
    /* `return;` mid-body: the exit goes through the exit label
     * without the fallthrough materialization. */
    if (n) return;
    volatile int v = n;
    (void)v;
}

static int g_count;

static void counted(void) {
    g_count++;
}

int main(void) {
    int x;

    /* The original shape: void inline calls in both ternary arms
     * (O2 inlines them; the ternary MOVs each arm's value). */
    for (x = 0; x < 4; x++) {
        x ? sink() : sink();
        x ? maybe_return(x) : maybe_return(0);
    }

    /* Semantic sanity: both arms execute exactly as C requires -
     * the true arm when x is nonzero, the false arm otherwise. */
    g_count = 0;
    {
        int t = 1 ? 1 : 0;      /* int ternary still works */
        int u = 0 ? 1 : 2;
        (1) ? counted() : sink();
        (0) ? counted() : counted();
        if (t + u + g_count != 5) {   /* 1 + 2 + 2 */
            printf("FAIL: ternary/inline-void semantics broken\n");
            return 1;
        }
    }

    /* Chained: void ternary used as an expression statement, both arms
     * exercised; only the selected arm runs (2 -> 3 -> 4). */
    int y = 1;
    y ? counted() : sink();
    y = 0;
    y ? sink() : counted();
    if (g_count != 4) {
        printf("FAIL: expected 4 counted calls, got %d\n", g_count);
        return 1;
    }

    printf("PASS\n");
    return 0;
}
