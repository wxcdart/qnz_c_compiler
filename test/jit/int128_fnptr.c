/* Sprint 110 follow-up: function pointers with __int128 signatures.
 *
 * Two separate bugs are guarded here:
 *
 * 1. Type checker: `__uint128_t (*fp)(__uint128_t) = id_u;` was rejected
 *    with "incompatible initializer type" because type_is_compatible had
 *    no TYPE_KIND_INT128 arm (two identical 128-bit types compared as
 *    incompatible), so pointer-to-function compatibility failed.
 *
 * 2. O2 small-function inliner: an inlined body that takes/returns a
 *    128-bit scalar lost the high half. The inlined param substitution
 *    did not set last_expr_is_128, so the body's `v >> 64` was lowered
 *    as HI128(ZEXT64_128(v_lo)) = 0 instead of HI128(v). The inliner's
 *    argument loop and return path also lacked the 128-bit width
 *    normalization the normal call path has.
 *
 * The small static helpers (lo64/hi64/negv/widen/comb) are inlining
 * candidates, so at -O2 they exercise the inlined paths; at -O0/-O1
 * they exercise the direct-call paths.
 *
 * Host-verified: gcc -static -O2
 */
#include <stdio.h>
typedef unsigned long long U64;
static U64 lo64(__uint128_t v) { return (U64)v; }
static U64 hi64(__uint128_t v) { return (U64)(v >> 64); }
static __int128_t negv(__int128_t v) { return -v; }
static __uint128_t widen(U64 x) { return (__uint128_t)x + (((__uint128_t)1) << 64); }
static __uint128_t comb(__uint128_t a, U64 b) { return a + b; }

static __uint128_t mk2(U64 lo, U64 hi) {
    return (__uint128_t)lo + (((__uint128_t)hi) << 64);
}

__uint128_t id_u(__uint128_t v) { return v; }
__int128_t id_s(__int128_t v) { return v; }
__uint128_t add_u(__uint128_t a, __uint128_t b) { return a + b; }
U64 first64(__uint128_t v) { return (U64)v; }
U64 wide64(unsigned long long v) { return v * 3 + 1; }

int main(void) {
    int ok = 1;
    __uint128_t (*fp_u)(__uint128_t) = id_u;
    __int128_t (*fp_s)(__int128_t) = id_s;
    __uint128_t (*fp_add)(__uint128_t, __uint128_t) = add_u;
    unsigned long long (*fp64)(__uint128_t) = first64;
    unsigned long long (*narrow)(unsigned long long) = wide64;

    __uint128_t v = mk2(0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL);

    /* 128-bit return + 128-bit param through a pointer */
    __uint128_t r = fp_u(v);
    if (lo64(r) != 0x0123456789ABCDEFULL) ok = 0;
    if (hi64(r) != 0xFEDCBA9876543210ULL) ok = 0;

    /* signed 128-bit through a pointer (two's complement ~x + 1 carry) */
    __int128_t sv = -(__int128_t)mk2(0x1111111111111111ULL, 0x2222222222222222ULL);
    __int128_t sr = fp_s(sv);
    if (lo64((__uint128_t)sr) != 0xEEEEEEEEEEEEEEEFULL) ok = 0;
    if (hi64((__uint128_t)sr) != 0xDDDDDDDDDDDDDDDDULL) ok = 0;
    /* inlined signed 128-bit return: negv(sr) must restore the positive */
    __int128_t back = negv(sr);
    if (lo64((__uint128_t)back) != 0x1111111111111111ULL) ok = 0;
    if (hi64((__uint128_t)back) != 0x2222222222222222ULL) ok = 0;

    /* two 128-bit params through a pointer */
    __uint128_t r2 = fp_add(v, v);
    if (lo64(r2) != (0x0123456789ABCDEFULL * 2)) ok = 0;
    if (hi64(r2) != (0xFEDCBA9876543210ULL * 2)) ok = 0;

    /* 128-bit param truncating to a 64-bit return */
    if (fp64(v) != 0x0123456789ABCDEFULL) ok = 0;

    /* 64-bit signature through a pointer */
    if (narrow(42) != 127ULL) ok = 0;

    /* inlined narrow-arg -> 128-bit-param and 128-bit-arg -> 64-bit-param */
    __uint128_t w = widen(0x42ULL);
    if (lo64(w) != 0x42ULL) ok = 0;
    if (hi64(w) != 1ULL) ok = 0;
    if (hi64(comb(w, 0x58ULL)) != 1ULL) ok = 0;
    if (lo64(comb(w, 0x58ULL)) != 0x9AULL) ok = 0;
    U64 t = hi64(w);           /* inlined hi64 of an inlined widen result */
    if (t != 1ULL) ok = 0;

    /* reassignment and later use through the same pointer object */
    __uint128_t (*fp2)(__uint128_t);
    fp2 = id_u;
    fp2 = add_u ? id_u : id_u;
    if (lo64(fp2(v)) != 0x0123456789ABCDEFULL) ok = 0;
    if (hi64(fp2(v)) != 0xFEDCBA9876543210ULL) ok = 0;

    printf("%s\n", ok ? "FP128-OK" : "FP128-FAIL");
    return ok ? 0 : 1;
}
