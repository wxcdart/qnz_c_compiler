// expect-partial: yes
/* Loop unrolling Phase 2 (Sprint 156): runtime-bound SIGNED loops with
 * constant (including odd and non-zero) inits - exercises the signed
 * prologue's remainder correction (N % f - init'%f folding with the
 * runtime sign branch) and negative trip counts. */
#include <stdio.h>

long pos_sum(int n) {                      /* 0..n-1, n signed runtime */
    long s = 0;
    for (int i = 0; i < n; i++) s += i;
    return s;
}

long tail_sum(int n) {                     /* 3..n-1 (odd init) */
    long s = 0;
    for (int i = 3; i < n; i++) s += i;
    return s;
}

long le_sum(int n) {                       /* <= form, 0..n */
    long s = 0;
    for (int i = 0; i <= n; i++) s += i;
    return s;
}

int main(void) {
    long t = 0;
    t += pos_sum(50);                      /* 1225  */
    t += pos_sum(-7);                      /* 0     (negative trip) */
    t += pos_sum(0);                       /* 0     (empty) */
    t += tail_sum(20);                     /* 3+..+19 = 187 (odd init, remainder 1) */
    t += tail_sum(3);                      /* 0     (empty: 3 < 3 false) */
    t += tail_sum(1);                      /* 0     (negative trip) */
    t += le_sum(10);                       /* 55    (<= form) */
    t += le_sum(-1);                       /* 0     (<= form, empty) */
    printf("%ld\n", t);
    return 0;
}
