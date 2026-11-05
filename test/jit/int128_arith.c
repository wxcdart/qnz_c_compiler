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

    return ok ? 0 : 1;
}
