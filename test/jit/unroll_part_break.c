// expect-partial: yes
/* Loop unrolling Phase 2 (Sprint 156): break inside a partially unrolled
 * loop - a break must exit to the FINAL exit label, skipping the
 * remainder loop (if the break re-entered the remainder the search would
 * keep scanning past the hit and return the wrong index). */
#include <stdio.h>

int find(const int *a, unsigned n, int key) {
    for (unsigned i = 0; i < n; i++)
        if (a[i] == key) return (int)i;
    return -1;
}

int main(void) {
    int a[8] = {5,3,8,1,9,2,7,4};
    int t = 0;
    t += find(a, 8, 1) * 100;      /* 300 (early hit) */
    t += find(a, 8, 7) * 10;       /* 60  (late hit) */
    t += find(a, 8, 4) * 2;        /* 14  (last element) */
    t += find(a, 8, 99);           /* -1  (no hit - full scan, no break) */
    t += find(a, 0, 1);            /* -1  (empty) */
    t += find(a, 1, 5);            /* 0   (first element) */
    printf("%d\n", t);
    return 0;
}
