/* Sprint 102: __builtin_apply / __builtin_apply_args (c-torture
 * execute/pr47237, the whole SKIP-TARGET:untyped_assembly bucket).
 *
 * __builtin_apply_args() snapshots the current function's parameters into
 * an 8-byte-per-arg stack block; __builtin_apply(fn, block, n) calls fn
 * through the block using fn's declared prototype.  Regression coverage:
 *   - multiple int args (whole-block and prefix apply)
 *   - an FP arg (the 8-byte slot must round-trip the bit pattern exactly,
 *     both 32-bit halves checked)
 *   - the (void *) result flows back into an int
 * The callees return int so the (void *) return works under both host gcc
 * and zerocc.  Regression for the inlining interaction: a function
 * calling __builtin_apply_args must NOT be inlined (the snapshot must
 * capture the callee's own parameters), so run_ints/run_fp are called,
 * not inlined.
 * Compiled with host gcc (make test) and zerocc -O0..-O3 (make test-opt);
 * expected exit code 0. */
#include <stdlib.h>

static int bits_hi = -1;

static int add3 (int a, int b, int c)
{
    return a + b + c;
}

static int pair (int a, int b)
{
    return a * 2 + b;
}

/* Splits the double's 64-bit pattern; returns the low 32 bits and
 * stores the high 32 in bits_hi. */
static int fbits (double d)
{
    union { double dd; int ii[2]; } u;
    u.dd = d;
    bits_hi = u.ii[1];
    return u.ii[0];
}

/* Three int params: apply the whole block to add3, the 2-arg prefix to
 * pair. */
static int run_ints (int a, int b, int c)
{
    int r1 = (int) __builtin_apply (add3, __builtin_apply_args (), 24);
    int r2 = (int) __builtin_apply (pair, __builtin_apply_args (), 16);
    return r1 + r2;
}

/* One double param: the 8-byte slot must carry the FP bits to the FP
 * register stream. */
static int run_fp (double d)
{
    return (int) __builtin_apply (fbits, __builtin_apply_args (), 8);
}

int main (void)
{
    /* (1+2+3) + (1*2+2) = 6 + 4 = 10 */
    int r = run_ints (1, 2, 3);
    if (r != 10)
        return 1;

    /* 2.5 == 0x4004000000000000 */
    int lo = run_fp (2.5);
    if (lo != 0x00000000 || bits_hi != 0x40040000)
        return 2;

    /* 2.0 + 2^-24 == 0x4000000008000000 (nonzero low half) */
    lo = run_fp (2.0 + 1.0 / 16777216.0);
    if (lo != 0x08000000 || bits_hi != 0x40000000)
        return 3;

    return 0;
}
