/* Sprint 109: __int128 / __uint128_t local value model.
 *
 * Exercises the (lo,hi) GPR128 lowering for locals: initialization,
 * load/store, arithmetic, bitwise, shifts (all count ranges), signedness,
 * and the half-compare ordering of the relational operators.  Also covers
 * file-scope (global) 128-bit objects: type size, initialization, load,
 * store, and compound assignment.  Runs under the JIT and the AOT sweep
 * (test-opt).  Excluded from the host-gcc `make test` baseline because it is
 * zerocc-specific 128-bit codegen. */
__uint128_t gval = 1000;
__int128_t  gsign = -5;
__int128_t  gneg = -1024;      /* negative initializer: hi must be 0xFFFF..FF */
__int128_t  gbig = 0;

static __int128 mkbig(void) {   /* return-ABI pressure: -(2^70) + 4096 */
    return (-((__int128)1 << 70)) + 4096;
}
int main(void) {
    __uint128_t a = 1000, b = 500;
    __uint128_t c = a + b;          /* 1500 */
    __uint128_t d = a - b;          /* 500  */
    __uint128_t e = a * b;          /* 500000 */
    __uint128_t f = a & b;
    __uint128_t g = a | b;
    __uint128_t h = a ^ b;
    __uint128_t s1 = a << 20;
    __uint128_t s2 = s1 >> 20;      /* back to a */
    __uint128_t big = (((__uint128_t)1) << 70) + 7;
    __int128    sg = -5;
    int ok = 1;

    if ((int)(c == 1500) != 1) ok = 0;
    if ((int)(d == 500)  != 1) ok = 0;
    if ((int)(e == 500000) != 1) ok = 0;
    if ((int)(s2 == a) != 1) ok = 0;
    if ((int)((big >> 70) == 1) != 1) ok = 0;   /* 64<N<128 shift */
    if ((int)(((__uint128_t)1) << 64) != 0) ok = 0;  /* N==64 shift */
    if ((int)(big & 0xFF) != 7) ok = 0;
    if ((int)(f == (a & b)) != 1) ok = 0;
    if ((int)(g == (a | b)) != 1) ok = 0;
    if ((int)(h == (a ^ b)) != 1) ok = 0;
    if ((int)(sg == -5) != 1) ok = 0;
    if ((int)(a > b) != 1) ok = 0;
    if ((int)(b < a) != 1) ok = 0;
    if ((int)(a >= c) != 0) ok = 0;   /* a=1000 < c=1500 */
    if ((int)(c != a) != 1) ok = 0;

    /* high-half carry / borrow / negation (Sprint 109: aarch64 SUBS->SBC,
     * riscv SLTU carry/borrow, x86-64 neg CF-clobber, unary-minus lowering).
     * Checked via >>64 + compare to keep the 128-bit value register-resident
     * (the 128-bit pass-by-value argument ABI is not yet implemented). */
    __uint128_t add_c = ((__uint128_t)0xFFFFFFFFFFFFFFFFULL) + 1;  /* (0, 1) */
    if ((int)((add_c >> 64) != 1) != 0) ok = 0;
    if ((int)(add_c != 0) != 1) ok = 0;
    __uint128_t sub_b = ((__uint128_t)100 + ((__uint128_t)1 << 64)) - 3;  /* (97, 1) */
    if ((int)((sub_b >> 64) != 1) != 0) ok = 0;
    if ((int)(sub_b != 97) != 1) ok = 0;
    __uint128_t borrow = ((__uint128_t)0) - 1;  /* 2^128-1 = (ff, ff) */
    if ((int)((borrow >> 64) != 0xFFFFFFFFFFFFFFFFULL) != 0) ok = 0;
    if ((int)(borrow != 0xFFFFFFFFFFFFFFFFULL) != 1) ok = 0;
    __int128 nneg = -5;  /* (fb, ff) */
    if ((int)(((__uint128_t)nneg >> 64) != 0xFFFFFFFFFFFFFFFFULL) != 0) ok = 0;
    if ((int)(((__uint128_t)nneg) != 0xFFFFFFFFFFFFFFFFULL) != 1) ok = 0;

    /* compound assignment */
    a += 10;
    if ((int)(a == 1010) != 1) ok = 0;
    a <<= 1;
    if ((int)(a == 2020) != 1) ok = 0;

    /* file-scope globals: init, load, compound, store */
    gval += 500;                     /* 1500 */
    if ((int)gval != 1500) ok = 0;
    gsign = gsign + 10;              /* 5 */
    if ((int)gsign != 5) ok = 0;
    if ((int)(gval * 2) != 3000) ok = 0;
    if ((int)(gval > 1000) != 1) ok = 0;
    if (sizeof(gval) != 16) ok = 0;

    /* ============ Sprint 109 phase 2: high-half correctness (hi != 0) ============
     * These caught: (a) signed >> emitted a logical shift (hi not sign-filled),
     * (b) AArch64 NEG128 register-field encoding, (c) global int128 negative
     * initializers stored only the low 8 bytes.  Expected values verified
     * against gmc / 128-bit two's-complement math. */
    __int128 v = -1024;                       /* lo=mfmffc00, hi=mfmf...mf */
    if ((int)(v >> 10) != -1) ok = 0;         /* was: hi left 0 by lsr path */
    if ((int)(v >> 64) != -1) ok = 0;
    if ((int)(v >> 70) != -1) ok = 0;
    v >>= 10;                                 /* compound shift, signed */
    if (v != -1) ok = 0;

    __int128 w = mkbig();                     /* lo=1000, hi=mfmfmf...fc0 */
    if ((unsigned long long)w != 0x0000000000001000ULL) ok = 0;
    if (((__uint128_t)w >> 64) != 0xFFFFFFFFFFFFFFC0ULL) ok = 0;
    if ((unsigned long long)(w >> 10) != 0xF000000000000004ULL) ok = 0;
    if (((__uint128_t)(w >> 10) >> 64) != 0xFFFFFFFFFFFFFFFFULL) ok = 0;
    if ((unsigned long long)(w >> 64) != 0xFFFFFFFFFFFFFFC0ULL) ok = 0;
    if (((__uint128_t)(w >> 64) >> 64) != 0xFFFFFFFFFFFFFFFFULL) ok = 0;
    if ((int)(w >> 70) != -1) ok = 0;

    __int128 big2 = ((__int128)2) + ((__int128)1 << 64);   /* (2, 1) */
    __int128 nb2 = -big2;                                  /* (fe, fe) */
    if ((unsigned long long)nb2 != 0xFFFFFFFFFFFFFFFEULL) ok = 0;
    if (((__uint128_t)nb2 >> 64) != 0xFFFFFFFFFFFFFFFEULL) ok = 0;

    /* mul with hi != 0 in one or both operands */
    __uint128_t ma = ((__uint128_t)0x1111111111111111ULL) + (((__uint128_t)0x2222222222222222ULL) << 64);
    __uint128_t mb = 0x3333333333333333ULL;
    __uint128_t mc = ((__uint128_t)0x4444444444444444ULL) + (((__uint128_t)0x5555555555555555ULL) << 64);
    if ((unsigned long long)(ma * mb) != 0x2FC962FC962FC963ULL) ok = 0;
    if (((ma * mb) >> 64) != 0x62FC962FC962FC96ULL) ok = 0;
    if ((unsigned long long)(ma * mc) != 0x3FB72EA61D950C84ULL) ok = 0;
    if (((ma * mc) >> 64) != 0xD3A06D3A06D3A06DULL) ok = 0;
    __uint128_t md = ((__uint128_t)0x6789ABCDEF012345ULL) + (((__uint128_t)0xABCDEF0123456789ULL) << 64);
    if ((unsigned long long)(md * 2) != 0xCF13579BDE02468AULL) ok = 0;
    if (((md * 2) >> 64) != 0x579BDE02468ACF12ULL) ok = 0;
    __int128 sn = -((__int128)1 << 64) - 7;   /* (f9, fe) */
    if ((unsigned long long)((__uint128_t)(sn * 10)) != 0xFFFFFFFFFFFFFFBAULL) ok = 0;
    if ((((__uint128_t)(sn * 10)) >> 64) != 0xFFFFFFFFFFFFFFF5ULL) ok = 0;

    /* add / sub with hi != 0 both sides */
    __uint128_t me = ((__uint128_t)0x123456789ABCDEF0ULL) + (((__uint128_t)0xFEDCBA0987654321ULL) << 64);
    __uint128_t mf = ((__uint128_t)0x0FEDCBA987654321ULL) + (((__uint128_t)0x0123456789ABCDEFULL) << 64);
    if ((unsigned long long)(me + mf) != 0x2222222222222211ULL) ok = 0;
    if (((me + mf) >> 64) != 0xFFFFFF7111111110ULL) ok = 0;
    if ((unsigned long long)(me - mf) != 0x02468ACF13579BCFULL) ok = 0;
    if (((me - mf) >> 64) != 0xFDB974A1FDB97532ULL) ok = 0;

    /* bitwise with hi != 0 */
    if ((unsigned long long)(ma & mc) != 0) ok = 0;
    if (((ma & mc) >> 64) != 0) ok = 0;
    if ((unsigned long long)(ma | mc) != 0x5555555555555555ULL) ok = 0;
    if (((ma | mc) >> 64) != 0x7777777777777777ULL) ok = 0;
    if ((unsigned long long)(ma ^ mc) != 0x5555555555555555ULL) ok = 0;
    if (((ma ^ mc) >> 64) != 0x7777777777777777ULL) ok = 0;
    if ((unsigned long long)((__uint128_t)~ma) != 0xEEEEEEEEEEEEEEEEULL) ok = 0;
    if ((((__uint128_t)~ma) >> 64) != 0xDDDDDDDDDDDDDDDDULL) ok = 0;

    /* unsigned shifts with hi != 0 */
    if ((unsigned long long)(me >> 10) != 0xC8448D159E26AF37ULL) ok = 0;
    if (((me >> 10) >> 64) != 0x003FB72E8261D950ULL) ok = 0;
    if ((unsigned long long)(me >> 64) != 0xFEDCBA0987654321ULL) ok = 0;
    if (((me >> 64) >> 64) != 0) ok = 0;
    if ((unsigned long long)(me >> 70) != 0x03FB72E8261D950CULL) ok = 0;
    if (((me >> 70) >> 64) != 0) ok = 0;
    if ((unsigned long long)(me << 10) != 0xD159E26AF37BC000ULL) ok = 0;
    if (((me << 10) >> 64) != 0x72E8261D950C8448ULL) ok = 0;
    if ((unsigned long long)(me << 70) != 0) ok = 0;
    if (((me << 70) >> 64) != 0x8D159E26AF37BC00ULL) ok = 0;

    /* global negative initializer: high half must be sign-extended */
    if ((unsigned long long)gneg != 0xFFFFFFFFFFFFFC00ULL) ok = 0;
    if (((__uint128_t)gneg >> 64) != 0xFFFFFFFFFFFFFFFFULL) ok = 0;
    /* global store with hi != 0, then reload */
    gbig = nb2;
    if ((unsigned long long)gbig != 0xFFFFFFFFFFFFFFFEULL) ok = 0;
    if (((__uint128_t)gbig >> 64) != 0xFFFFFFFFFFFFFFFEULL) ok = 0;

    return ok ? 0 : 1;
}
