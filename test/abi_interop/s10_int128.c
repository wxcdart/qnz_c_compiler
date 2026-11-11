/* ABI interop: s10 __int128 args & returns (shared main, compiled by BOTH compilers)
 * expect: 0 210001
 * expect: 192
 */
#include <stdio.h>
__int128 zc_i128_mul(__int128, __int128);
long long zc_i128_hi(__int128); unsigned long long zc_i128_lo(__int128);
__int128 gc_i128_add(__int128, __int128); __int128 gc_i128_shift(__int128, int);
int main(void) {
    __int128 p = zc_i128_mul(300, 700);          /* 210000 */
    __int128 s = gc_i128_add(p, 1);              /* 210001 */
    printf("%lld %llu\n", zc_i128_hi(s), zc_i128_lo(s));
    __int128 big = gc_i128_shift(1, 70);         /* 2^70 */
    big = zc_i128_mul(big, 3);
    printf("%lld\n", zc_i128_hi(big));           /* hi of 3*2^70 = 3 << 6 = 192 */
    return 0;
}

