// expect-partial: yes
/* Loop unrolling Phase 2 (Sprint 156): 64-bit (long long) signed bound -
 * the signed prologue on 64-bit values (N % f, sign branch, N - r). */
#include <stdio.h>

long long sum64(long long n) {
    long long s = 0;
    for (long long i = 0; i < n; i++) s += i;
    return s;
}

int main(void) {
    long long t = sum64(100001LL);   /* 5000050000 */
    t += sum64(99999LL);             /* 4999950000 */
    t += sum64(-40LL);               /* 0 */
    printf("%lld\n", t);
    return 0;
}
