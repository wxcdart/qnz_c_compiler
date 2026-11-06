/* Sprint 119 (slice 2) zero_san: legal in-range narrow arithmetic must
 * NOT trap under -fsanitize=undefined.
 *
 * Exercises the two slice-2 mechanisms on the trap path:
 *   - the OVF check width is read tag-first from the checked temp's tag
 *     (I2: signed int of exactly the check width) instead of the
 *     instruction's check-width field;
 *   - the checked ALU op is unmarked (its C99 mark slot was consumed by
 *     the trap), so its C99 wrap-extend now comes from the tag
 *     (gap-fill) and is DEFERRED past the check - the check must see
 *     the raw 64-bit result and the ALU-set flags. On values that
 *     survive the check the deferred extend is provably a no-op.
 *
 * The sweep (tools/san_sweep.py) runs this at -O0..-O3, JIT + AOT:
 * exit 0 with -fsanitize=undefined and identically without. The paired
 * san_trap_* files prove the checks still fire (a lost deferral would
 * extend before the check, clobber the flags and silently drop traps).
 */
int main(void) {
    int a = 1000000000, b = 200000000;
    int s = a + b;            /* in-range int sum (check width 4) */
    int d = a - b;
    int m = 40000 * 30000;    /* in-range int product */
    int t = 120;
    int c = (int)((short)t + (short)8); /* short promotion, in range */

    long long L = 4000000000LL, R = 2000000000LL;
    long long ls = L + R;     /* in-range 64-bit sum (check width 8) */
    long long lm = 3000000000LL * 3LL;
    long long ld = L - R;

    if (s != 1200000000 || d != 800000000 || m != 1200000000) return 1;
    if (c != 128) return 2;
    if (ls != 6000000000LL || lm != 9000000000LL || ld != 2000000000LL)
        return 3;
    return 0;
}
