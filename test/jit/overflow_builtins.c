/* Sprint 159: __builtin_{mul,add,sub}_overflow{,_p} semantics.
 *
 * The lowering (src/zero_cc/ir/ast_lowering_builtins.c) implements the
 * GCC model calibrated against gcc-15 on the c-torture corpus
 * (pr71554, pr30314, pr91450-1, pr105777, pr91635):
 *
 *   - each operand keeps its OWN signedness (unsigned zero-extends,
 *     signed sign-extends to 64 bits);
 *   - the math domain is unsigned only when BOTH operands are unsigned
 *     (pr71554: 2u32 * -1073741824 with an int r must compute the
 *     signed product == INT_MIN, which fits);
 *   - the exact mathematical a OP b is checked against the RANGE OF r:
 *     an unsigned r rejects P < 0 (pr91450-1: -4 * 2 == -8 overflows
 *     the unsigned long long r), a signed r rejects |P| past its
 *     symmetric bounds;
 *   - the 64-bit wrap must not fool the check: 42 * 439208192231179800
 *     == 2^64-16 wraps negative in 64 bits but fits an unsigned long
 *     long r (pr30314 qux).
 *
 * Compiled with host gcc (make test) and zerocc -O0..-O3 (make
 * test-opt); expected exit code 0. */
#include <limits.h>
#include <stdint.h>
#include <stdio.h>

static int fails;

static void chk (int got, int want, const char *what)
{
    if (got != want) {
        fprintf (stderr, "overflow_builtins: %s: got %d want %d\n",
                 what, got, want);
        fails++;
    }
}

int main (void)
{
    unsigned u32r;
    unsigned long long u64r;
    long long i64r;
    int i32r;

    /* --- signed domain, result checked against r's range --- */

    /* pr71554: 2u32 * -1073741824 == INT_MIN fits the int r. */
    chk (__builtin_mul_overflow (2U, -1073741824, &i32r) == 0
         && i32r == INT_MIN, 1, "pr71554 fit");

    /* the same product does NOT fit when r is unsigned. */
    chk (__builtin_mul_overflow (2U, -1073741824, &u32r) == 1, 1,
        "neg product u32 r");

    /* pr91450-1: -4 * 2 == -8 overflows an unsigned-long-long r. */
    chk (__builtin_mul_overflow (-4, 2, &u64r) == 1, 1, "pr91450-1");
    chk (__builtin_mul_overflow_p (-4, 2, 0LL) == 0, 1, "-4*2 fits i64 r");

    /* pr30314 qux: 42 * 439208192231179800 == 2^64-16 fits u64 even
     * though the 64-bit wrap is negative. */
    chk (__builtin_mul_overflow_p (42L, 439208192231179800ULL, 0ULL) == 0, 1,
        "2^64-16 fits u64 r");
    chk (__builtin_mul_overflow_p (42L, 439208192231179801ULL, 0ULL) == 1, 1,
        "2^64-14 does not");
    /* the same magnitude with a negative factor: -(2^64-16) < 0. */
    chk (__builtin_mul_overflow_p (-42L, 439208192231179800ULL, 0ULL) == 1, 1,
        "-(2^64-16) u64 r");
    /* an int64 r caps at 2^63-1: 2^64-16 overflows it. */
    chk (__builtin_mul_overflow_p (42L, 439208192231179800ULL, 0LL) == 1, 1,
        "2^64-16 i64 r");

    /* zero operands: no overflow, stored 0. */
    chk (__builtin_mul_overflow (0, 100, &i32r) == 0 && i32r == 0, 1,
        "mul by 0");
    chk (__builtin_mul_overflow_p (100, 0, 0ULL) == 0, 1, "mul by 0 (u64 r)");

    /* --- unsigned domain (both operands unsigned) --- */

    chk (__builtin_mul_overflow (122713352U, 35U, &u32r) == 1, 1,
        "u32 mul ovf");
    chk (__builtin_mul_overflow_p (122713351U, 35U, 0U) == 0, 1,
        "u32 mul fit");
    chk (__builtin_mul_overflow_p (0xFFFFFFFFFFFFFFFFULL, 35ULL, 0ULL) == 1,
        1, "u64 mul ovf");
    chk (__builtin_mul_overflow_p (0xFFFFFFFFFFFFFFFEULL, 1ULL, 0ULL) == 0, 1,
        "u64 mul fit");
    chk (__builtin_mul_overflow_p (0x8000000000000000ULL, 2ULL, 0ULL) == 1, 1,
        "2^64 u64 r");
    chk (__builtin_mul_overflow_p (0x8000000000000000ULL, 1ULL, 0ULL) == 0, 1,
        "2^63 fits u64 r");

    /* --- add / sub --- */

    /* mixed pair keeps the signed domain: -1 + 2 == 1 fits u32. */
    chk (__builtin_add_overflow_p (-1, 2U, 0U) == 0, 1, "-1+2 u32 r");
    chk (__builtin_add_overflow_p (0x8000000000000000ULL, 1ULL, 0ULL) == 0, 1,
        "2^63+1 u64 r");
    chk (__builtin_add_overflow_p (0xFFFFFFFFFFFFFFFEULL, 3ULL, 0ULL) == 1, 1,
        "2^64+1 u64 r");
    chk (__builtin_add_overflow_p (0x7FFFFFFFFFFFFFFFLL, 2LL, 0LL) == 1, 1,
        "2^64-2 i64 r");
    chk (__builtin_add_overflow_p (0x7FFFFFFFFFFFFFFFLL, 2LL, 0ULL) == 0, 1,
        "2^64-2 u64 r");
    /* 64-bit wrap with P < 0: INT64_MIN - 1 overflows the u64 r. */
    chk (__builtin_add_overflow_p (INT64_MIN, -1, 0ULL) == 1, 1,
        "wrap neg add u64 r");

    chk (__builtin_sub_overflow_p (1U, 2U, 0U) == 1, 1, "1-2 u32 r");
    chk (__builtin_sub_overflow_p (-1LL, 2, 0ULL) == 1, 1, "-3 u64 r");
    chk (__builtin_sub_overflow_p (0x8000000000000000ULL, 1ULL, 0ULL) == 0, 1,
        "2^63-1 u64 r");
    chk (__builtin_sub_overflow_p (0, 2, 0) == 0, 1, "-2 fits i32 r");
    /* P > 0 past 2^63 but < 2^64: fits the u64 r, not the i64 r. */
    chk (__builtin_sub_overflow_p (0x7FFFFFFFFFFFFFFFLL, -1LL, 0ULL) == 0, 1,
        "2^63 u64 r");
    chk (__builtin_sub_overflow_p (0x7FFFFFFFFFFFFFFFLL, -1LL, 0LL) == 1, 1,
        "2^63 i64 r");

    /* stored result is the wrapped value, width-truncated. */
    chk (__builtin_add_overflow (0x7FFFFFFFFFFFFFFFLL, 1, &i64r) == 1
         && i64r == INT64_MIN, 1, "wrapped i64 store");

    return fails ? 1 : 0;
}
