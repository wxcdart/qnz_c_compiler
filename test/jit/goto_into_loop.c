/* Sprint 35 Phase 0 regression test: `goto` from OUTSIDE a loop into a label
 * inside the loop body, plus re-entry of the whole loop via `goto`.
 *
 * The inner for-loop carries `sum` (exactly one in-loop load/store, one
 * preheader init) - a loop-promotion candidate.  The mid-body `goto mid`
 * entry path skips the preheader init, so promotion must be disqualified
 * for the loop (entry-cleanness guard) or the slot must otherwise be
 * unauditable; either way every -O level must produce the same result.
 *
 * Trace: round 1 sum = 0+1+2 = 3; round 2 sum = 3+0+1+2 = 6; then
 * `goto mid` enters at i = 3 (loop-exit value): sum += 3 = 9. */
int main() {
    int i, sum = 0;
    int n = 3;
    int r = 0;
loop:
    for (i = 0; i < n; i++) {
        mid:
        sum += i;
    }
    r++;
    if (r == 1) goto loop;
    if (r == 2) goto mid;
    return sum == 9 ? 0 : 1;
}
