/* ABI interop scenario s01: shared main (compiled by BOTH compilers).
 * expect: 20
 * expect: -232003
 * expect: 42
 * expect: 25
 */
#include <stdio.h>

int zc_ints(int, long, unsigned long long, const char *);
long long zc_ll_return(int, int);
int gc_ints(int, long, unsigned long long, const char *);
long long gc_ll_return(int, int);

int main(void) {
    int r1 = zc_ints(1, 2, 194, "hello") + gc_ints(3, 4, 178, "world");
    printf("%d\n", r1);
    long long r2 = zc_ll_return(-33, 7) + gc_ll_return(5, -2);
    printf("%lld\n", r2);
    int r3 = gc_ints(zc_ints(2, 3, 10, "ab"), -1, 5, "x")
            + zc_ints(1, 1, 4, "y"), q = 13;
    (void)q;
    printf("%d\n", r3 + q);
    printf("%d\n", (int)(zc_ll_return(1, 2) / 100) + (int)(gc_ll_return(2, 3) / 100));
    return 0;
}
