/* Sprint 177 (fuzz seed 20261137): the 64-bit -> 128-bit widening of a
 * __int128 store / return must follow the SOURCE expression's signedness
 * (C 6.3.1.3), not the target's. An unsigned 64-bit source with the top
 * bit set into a SIGNED __int128 target must zero-extend: keying off the
 * target's signedness sign-extended it (high half all ones).
 *
 * Also guards: signed 64-bit source sign-extends; explicit casts keep
 * their own signedness; unsigned __int128 targets zero-extend.
 */
#include <stdio.h>

static unsigned long long hi128(__int128 v) {
    return (unsigned long long)(v >> 64);
}
static unsigned long long lo128(__int128 v) {
    return (unsigned long long)(v & ((unsigned __int128)-1));
}

__int128 ret_unsigned(void) {
    unsigned long u = -17; /* 2^64-17 */
    return u;
}
__int128 ret_signed(void) {
    long s = -17;
    return s;
}

int main(void) {
    unsigned long g2 = -17; /* 0xFFFFFFFFFFFFFFEF */
    __int128 a = g2;                    /* zext: hi must be 0 */
    __int128 b = (g2 << 4);             /* (2^64-17)<<4 mod 2^64, zext */
    __int128 c = (g2 + 1);              /* 2^64-16, zext */
    long s2 = -17;
    __int128 d = s2;                    /* sext: hi all ones */
    unsigned __int128 e = g2;           /* zext (unsigned target) */
    __int128 f = ret_unsigned();        /* zext across the return */
    __int128 g = ret_signed();          /* sext across the return */
    __int128 h = (__int128)g2;          /* explicit cast: zext */

    if (hi128(a) != 0 || lo128(a) != 18446744073709551599ULL) { printf("FAIL a\n"); return 1; }
    if (hi128(b) != 0 || lo128(b) != 18446744073709551344ULL) { printf("FAIL b\n"); return 1; }
    if (hi128(c) != 0 || lo128(c) != 18446744073709551600ULL) { printf("FAIL c\n"); return 1; }
    if (hi128(d) != 18446744073709551615ULL || lo128(d) != 18446744073709551599ULL) { printf("FAIL d\n"); return 1; }
    if (hi128(((__int128)e)) != 0 || lo128(((__int128)e)) != 18446744073709551599ULL) { printf("FAIL e\n"); return 1; }
    if (hi128(f) != 0 || lo128(f) != 18446744073709551599ULL) { printf("FAIL f\n"); return 1; }
    if (hi128(g) != 18446744073709551615ULL || lo128(g) != 18446744073709551599ULL) { printf("FAIL g\n"); return 1; }
    if (hi128(h) != 0 || lo128(h) != 18446744073709551599ULL) { printf("FAIL h\n"); return 1; }
    printf("ok\n");
    return 0;
}
