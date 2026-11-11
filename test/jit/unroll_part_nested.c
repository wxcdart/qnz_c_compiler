// expect-partial: no
/* Loop unrolling Phase 2 (Sprint 156): REJECTION case (re-entry soundness) -
 * a nested loop whose counter is NOT re-initialized on each outer trip.
 * The inner loop's counter j carries its previous run's final value into
 * the next entry (advanced by the inner backedge, re-entered via the outer
 * backedge), so the "constant init at entry" assumption the partial prologue
 * relies on is false.  This is the c-torture execute/20041213-2 shape (PR
 * tree-optimization/18694): partial unrolling must refuse any loop whose
 * span is enclosed by another loop's span. */
#include <stdio.h>

void foo(int i) {
    int next_n = 1;
    int j = 0;
    for (; i != 0; i--) {
        int n;
        for (n = next_n; j < n; j++)
            next_n++;
        if (j != n) { printf("ABORT %d %d\n", j, n); return; }
    }
}

int main(void) {
    foo(2);
    foo(5);
    printf("ok\n");
    return 0;
}
