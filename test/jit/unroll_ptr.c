// expect-unroll: yes
/* Loop unrolling (Sprint 150): pointer loops.
 * p1 has a variable bound (stays a loop); p2 advances a pointer in the
 * step clause alongside the counter (both must advance per copy). */
#include <stdio.h>

int p1(int *a, int n) {
    int s = 0;
    for (int i = 0; i < n; i++) s += a[i];
    return s;
}

int p2(void) {
    int arr[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    int *p = arr;
    int s = 0;
    for (int i = 0; i < 8; i++, p++) s += *p;
    return s;  /* 36 */
}

int main(void) {
    int arr[4] = {10, 20, 30, 40};
    printf("%d %d\n", p1(arr, 4), p2());
    return 0;
}
