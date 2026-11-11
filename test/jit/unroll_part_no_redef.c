// expect-partial: no
/* Loop unrolling Phase 2 (Sprint 156): REJECTION case (temp-bound path) -
 * the bound is reloaded through a pointer inside the header, so its
 * operand temp is DEFINED in the header block (one load per trip).  It is
 * not a loop-invariant value, so partial unrolling must refuse.
 * (Complements unroll_part_no_store.c, which covers the slot-bound path.) */
#include <stdio.h>

long via_ptr(unsigned n, const int *a) {
    long s = 0;
    unsigned *pn = &n;
    for (unsigned i = 0; i < *pn; i++)
        s += a[i];
    return s;
}

int main(void) {
    int a[6] = {2, 4, 6, 8, 10, 12};
    printf("%ld\n", via_ptr(6, a));   /* 42 */
    return 0;
}
