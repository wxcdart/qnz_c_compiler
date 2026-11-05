/* zero_rt/core/int128.c
 *
 * 128-bit integer division & modulo for ZeroCC's freestanding runtime.
 *
 * These are ZeroCC-owned (NOT libgcc's __divti3/__udivti3): the division is
 * implemented here in portable C using 64-bit halves only (no __int128, no
 * compiler-rt dependency), so it is fully freestanding and works on every
 * target ISA (x86-64, aarch64, riscv64, riscv32) with no external runtime.
 *
 * The public wrappers take the operands as four 64-bit halves and return the
 * 128-bit result via the System V (low,high) = (rax,rdx) / (x0,x1) / (a0,a1)
 * 128-bit integer-return convention. Callers (ZeroCC codegen) read the result
 * as a two-register value.
 *
 * C99 semantics: a/b truncates toward zero, a%b has the sign of a.
 */
#include <stdint.h>

#ifndef ZERO_RT_USE_INT128_TYPE
#define ZERO_RT_USE_INT128_TYPE 1
#endif

#if ZERO_RT_USE_INT128_TYPE
typedef unsigned __int128 zc_u128;
typedef __int128 zc_s128;
#endif

/* Unsigned 128 / 128 via restoring binary long division.
 * U = (u1<<64)|u0, V = (v1<<64)|v0, V != 0.
 * Quotient (q1<<64)|q0, remainder (r1<<64)|r0. 64-bit halves only. */
static void zc_udivmod128(uint64_t u0, uint64_t u1, uint64_t v0, uint64_t v1,
                          uint64_t *q0, uint64_t *q1, uint64_t *r0, uint64_t *r1) {
    uint64_t rl = 0, rh = 0, q0v = 0, q1v = 0;
    for (int i = 127; i >= 0; i--) {
        uint64_t ubit = (i >= 64) ? ((u1 >> (i - 64)) & 1ULL) : ((u0 >> i) & 1ULL);
        uint64_t carry = (rh >> 63) & 1ULL;
        rh = (rh << 1) | ((rl >> 63) & 1ULL);
        rl = (rl << 1) | ubit;
        int ge;
        if (carry) ge = 1;
        else if (rh > v1) ge = 1;
        else if (rh < v1) ge = 0;
        else ge = (rl >= v0);
        if (ge) {
            int b0 = (rl < v0);
            rl = rl - v0;
            int b1 = b0 ? (rh <= v1) : (rh < v1);
            rh = rh - v1 - (uint64_t)b0;
            (void)carry; (void)b1; /* top word clears (result < V < 2^128) */
            if (i >= 64) q1v |= (1ULL << (i - 64)); else q0v |= (1ULL << i);
        }
    }
    *q0 = q0v; *q1 = q1v; *r0 = rl; *r1 = rh;
}

static void zc_neg128(uint64_t *lo, uint64_t *hi) {
    *lo = ~*lo; *hi = ~*hi; *lo += 1; if (*lo == 0) *hi += 1;
}

/* Signed 128 / 128 (two's-complement halves in, halves out). */
static void zc_sdivmod128(uint64_t a0, uint64_t a1, uint64_t b0, uint64_t b1,
                          uint64_t *q0, uint64_t *q1, uint64_t *r0, uint64_t *r1) {
    int aneg = (int)(a1 >> 63), bneg = (int)(b1 >> 63);
    uint64_t ma0 = a0, ma1 = a1, mb0 = b0, mb1 = b1;
    if (aneg) zc_neg128(&ma0, &ma1);
    if (bneg) zc_neg128(&mb0, &mb1);
    uint64_t qa0, qa1, ra0, ra1;
    zc_udivmod128(ma0, ma1, mb0, mb1, &qa0, &qa1, &ra0, &ra1);
    /* INT128_MIN / -1 overflows (UB in C99); match GCC: q = INT128_MIN, r = 0. */
    if (a1 == 0x8000000000000000ULL && a0 == 0 &&
        b1 == 0xFFFFFFFFFFFFFFFFULL && b0 == 0xFFFFFFFFFFFFFFFFULL) {
        *q0 = a0; *q1 = a1; *r0 = 0; *r1 = 0;
        return;
    }
    if (aneg ^ bneg) zc_neg128(&qa0, &qa1); /* quotient sign = sign(a)*sign(b) */
    if (aneg) zc_neg128(&ra0, &ra1);        /* remainder sign = sign(a)       */
    *q0 = qa0; *q1 = qa1; *r0 = ra0; *r1 = ra1;
}

#if ZERO_RT_USE_INT128_TYPE

unsigned __int128 zc_udiv128(uint64_t a0, uint64_t a1, uint64_t b0, uint64_t b1) {
    uint64_t q0, q1, r0, r1;
    zc_udivmod128(a0, a1, b0, b1, &q0, &q1, &r0, &r1);
    return (zc_u128)((zc_u128)q1 << 64) | q0;
}

unsigned __int128 zc_umod128(uint64_t a0, uint64_t a1, uint64_t b0, uint64_t b1) {
    uint64_t q0, q1, r0, r1;
    zc_udivmod128(a0, a1, b0, b1, &q0, &q1, &r0, &r1);
    return (zc_u128)((zc_u128)r1 << 64) | r0;
}

__int128 zc_sdiv128(uint64_t a0, uint64_t a1, uint64_t b0, uint64_t b1) {
    uint64_t q0, q1, r0, r1;
    zc_sdivmod128(a0, a1, b0, b1, &q0, &q1, &r0, &r1);
    return (zc_s128)((zc_u128)q1 << 64) | (zc_u128)q0;
}

__int128 zc_smod128(uint64_t a0, uint64_t a1, uint64_t b0, uint64_t b1) {
    uint64_t q0, q1, r0, r1;
    zc_sdivmod128(a0, a1, b0, b1, &q0, &q1, &r0, &r1);
    return (zc_s128)((zc_u128)r1 << 64) | (zc_u128)r0;
}

#endif /* ZERO_RT_USE_INT128_TYPE */
