/* Sprint 109: __int128 / __uint128_t RETURN value ABI.
 *
 * A function returning a 128-bit integer returns the (lo,hi) pair across
 * (rax, rdx) per the System V AMD64 ABI (the same pair as the 9-16 byte
 * struct return).  This exercises the callee write + caller read for:
 *   - a value built from constants (build_u),
 *   - a value built from int parameters (build_s),
 *   - signed and unsigned 128-bit types.
 * 128-bit *arguments* are covered separately (they need the arg-ABI half).
 * Runs under the JIT and the AOT sweep (test-opt); excluded from the
 * host-gcc `make test` baseline because it is zerocc-specific 128-bit codegen. */
__uint128_t build_u(void) {
    __uint128_t r = 0x1234;
    r |= ((__uint128_t)0x5678) << 64;
    return r;
}
__int128 build_s(int lo, int hi) {
    __int128 r = lo;
    r |= ((__int128)hi) << 64;
    return r;
}
__uint128_t hi_only(void) {
    __uint128_t r = 0;
    r |= ((__uint128_t)1) << 64;   /* 2^64 */
    return r;
}
int main(void) {
    __uint128_t u = build_u();
    __uint128_t ue = 0x1234;
    ue |= ((__uint128_t)0x5678) << 64;
    if (u != ue) return 1;
    if (sizeof(u) != 16) return 11;

    __int128 s = build_s(0x1234, 0x5678);
    __int128 se = 0x1234;
    se |= ((__int128)0x5678) << 64;
    if (s != se) return 2;

    __uint128_t h = hi_only();
    if (h != (((__uint128_t)1) << 64)) return 3;
    if ((int)h != 0) return 4;                 /* low half must be 0 */
    if (h != (((__uint128_t)1) << 64) + 0) return 5;
    return 0;
}
