/* Sprint 119 (slice 2): tag-driven codegen consumers.
 *
 * Exercises the three codegen decisions that now read the temp tag as
 * the single source of truth (mark-fallback):
 *   1. C99 wrap-extend: narrow-class ALU ops canonicalize their result
 *      to the dest temp's tagged C width (char/short/unsigned, 32-bit
 *      wrap of an int sum) - including the tag gap-fill path (ops whose
 *      width mark was consumed by the zero_san OVF_TRAP slot-steal,
 *      where the extend is deferred past the check) and the O3
 *      branchless-select rewrites (tagged scratch + tagged final res).
 *   2. DIV/MOD signedness: the quotient's tagged C type (INT vs UINT)
 *      selects idiv vs div, for 32- and 64-bit, signed and unsigned.
 *   3. OVF_TRAP check width (san mode only, exercised by
 *      test/san/san_ok_tagdriven.c in the test-san sweep).
 *
 * Differential against host gcc at -O0..-O3, JIT + AOT (make test-opt).
 */
#include <stdio.h>

static int fails = 0;
static void chk(int got, int want, const char *what) {
    if (got != want) {
        printf("FAIL %s: got %d want %d\n", what, got, want);
        fails++;
    }
}

int main(void) {
    /* char/short wrap + canonicalization (signed + unsigned) */
    char a = 120, b = 10;
    char c = (char)(a + b);                  /* 130 -> -126 */
    unsigned char u = 250u, v2 = 10u;
    unsigned char v = (unsigned char)(u + v2); /* 260 -> 4 */
    short s = -40000;
    short t = (short)(s + 40000);            /* 0 */
    short u2 = 30000;
    short u3 = (short)(u2 + 30000);          /* wraps negative */
    /* 32-bit wrap of an int sum (the OVF-deferred-extend shape in san
     * mode; the plain marked shape here) */
    int x = 2000000000, y = 2000000000;
    int w = x + y;                           /* -294967296 */
    int wn = -2000000000 - 2000000000;      /* +294967296 */

    /* signed / unsigned div & mod (tag-first signedness) */
    int q1 = -7 / 2, r1 = -7 % 2;            /* -3, -1 (trunc toward 0) */
    int q1b = 7 / -2, r1b = 7 % -2;          /* -3, 1 */
    unsigned q2 = 7u / 2u, r2 = 7u % 2u;     /* 3, 1 */
    unsigned int q2b = 4000000000u / 3u;     /* 1333333333 */
    long long q3 = -9000000000LL / 4LL;      /* -2250000000 */
    long long r3 = -9000000001LL % 4LL;      /* -1 */
    unsigned long long q4 = 9000000000ULL / 4ULL;
    unsigned long long r4 = 9000000001ULL % 4ULL;

    /* ternaries: the variable-armed ones are branchless-select-eligible
     * at -O3 (GPR arms, non-constant condition) - the select scratch
     * temps (MUL/CMP/COND_SET/SUB mask, XOR diff, AND sel) and the
     * final XOR res all ride on tags at codegen time */
    int p = 3 > 2 ? 41 : 42;
    int av = 100, bv = -100, n = -5;
    int m = n > 0 ? av : bv;             /* branchless at -O3 */
    int cv = 7, dv = 8;
    int sp = x < y ? cv : dv;            /* branchless at -O3 */
    char c5 = 5, cminus = -5;
    char cp = 1 > 2 ? c5 : cminus;       /* narrow-tagged final res */
    long l11 = 11, l22 = 22;
    long l = 2 < 1 ? l11 : l22;
    unsigned u5 = 5, u6 = 6;
    unsigned up = 0u > 1u ? u5 : u6;
    int tv = 3 > 2 ? av : bv;            /* const-cond variable arms */

    chk((int)c, -126, "char add wrap");
    chk((int)v, 4, "uchar add wrap");
    chk((int)t, 0, "short add back");
    chk((int)u3, (int)(60000 - 65536), "short wrap negative");
    chk(w, -294967296, "int 32-bit wrap");
    chk(wn, 294967296, "int 32-bit wrap positive");

    chk(q1, -3, "signed div trunc");
    chk(r1, -1, "signed mod sign");
    chk(q1b, -3, "signed div neg divisor");
    chk(r1b, 1, "signed mod neg divisor");
    chk((int)q2, 3, "unsigned div");
    chk((int)r2, 1, "unsigned mod");
    chk((int)q2b, 1333333333, "unsigned div big");
    chk((int)q3, -2250000000, "ll div");
    chk((int)r3, -1, "ll mod sign");
    chk((int)q4, 2250000000, "ull div");
    chk((int)r4, 1, "ull mod");

    chk(p, 41, "ternary then");
    chk(m, -100, "ternary else");
    chk((int)cp, -5, "ternary narrow");
    chk((int)l, 22, "ternary long");
    chk((int)up, 6, "ternary unsigned");
    chk(sp, 8, "ternary from vars");
    chk(tv, 100, "ternary const cond");

    if (!fails) printf("ok\n");
    return fails ? 1 : 0;
}
