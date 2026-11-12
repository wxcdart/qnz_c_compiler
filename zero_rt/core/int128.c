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

/* Variable-count 128-bit shifts (Sprint 113) - 64-bit halves only: this
 * file is also compiled by zerocc itself for the AOT zero_rt archive, and
 * zerocc's 128-bit shift lowering requires a compile-time-constant count.
 * The count is masked to 127 (C11 6.5.7p3: shift >= width is
 * implementation-defined; every real target masks). */
static void zc_shl128_var(uint64_t lo, uint64_t hi, uint64_t n,
                          uint64_t *o0, uint64_t *o1) {
    n &= 127;
    if (n == 0) { *o0 = lo; *o1 = hi; return; }
    if (n < 64) { *o0 = lo << n; *o1 = (hi << n) | (lo >> (64 - n)); }
    else        { *o0 = 0;        *o1 = lo << (n - 64); }
}
static void zc_shr128_var(uint64_t lo, uint64_t hi, uint64_t n,
                          uint64_t *o0, uint64_t *o1) {
    n &= 127;
    if (n == 0) { *o0 = lo; *o1 = hi; return; }
    if (n < 64) { *o0 = (lo >> n) | (hi << (64 - n)); *o1 = hi >> n; }
    else        { *o0 = hi >> (n - 64);               *o1 = 0; }
}
static void zc_sar128_var(uint64_t lo, uint64_t hi, uint64_t n,
                          uint64_t *o0, uint64_t *o1) {
    n &= 127;
    if (n == 0) { *o0 = lo; *o1 = hi; return; }
    int64_t h = (int64_t)hi;
    if (n < 64) { *o0 = (lo >> n) | (hi << (64 - n)); *o1 = (uint64_t)(h >> n); }
    else        { *o0 = (uint64_t)(h >> (n - 64));    *o1 = (uint64_t)(h >> 63); }
}

unsigned __int128 zc_shl128_v(uint64_t a0, uint64_t a1, uint64_t n) {
    uint64_t o0, o1;
    zc_shl128_var(a0, a1, n, &o0, &o1);
    return (zc_u128)((zc_u128)o1 << 64) | o0;
}
unsigned __int128 zc_shr128_v(uint64_t a0, uint64_t a1, uint64_t n) {
    uint64_t o0, o1;
    zc_shr128_var(a0, a1, n, &o0, &o1);
    return (zc_u128)((zc_u128)o1 << 64) | o0;
}
__int128 zc_sar128_v(uint64_t a0, uint64_t a1, uint64_t n) {
    uint64_t o0, o1;
    zc_sar128_var(a0, a1, n, &o0, &o1);
    return (zc_s128)((zc_u128)o1 << 64) | (zc_u128)o0;
}

#endif /* ZERO_RT_USE_INT128_TYPE */

/* --------------------------------------------------------------------------
 * 64-bit software integer division and modulo helpers.
 * Required on 32-bit architectures (e.g. RISC-V 32) that lack 64-bit hardware
 * divide instructions. Provided under standard libgcc symbol names.
 * -------------------------------------------------------------------------- */
static uint64_t zc_udivmod64(uint64_t u, uint64_t v, uint64_t *rem) {
    if (v == 0) {
        if (rem) *rem = 0;
        return 0;
    }
    uint64_t q = 0, r = 0;
    for (int i = 63; i >= 0; i--) {
        r = (r << 1) | ((u >> i) & 1ULL);
        if (r >= v) {
            r -= v;
            q |= (1ULL << i);
        }
    }
    if (rem) *rem = r;
    return q;
}

/* 64-bit div/mod wrappers under the standard libgcc symbol names.
 *
 * On RISC-V 32 these are provided by the rv32 libgcc SHIM instead
 * (third_party/vendor/musl-cross/riscv32/lib/libgcc.a, built by
 * tools/setup_musl_cross.sh): musl's vfprintf references __udivdi3 with a
 * live relocation and __divdi3 with a dormant one, so if this member were
 * pulled in (for the zc_*128 helpers, or the dormant reference itself)
 * its __udivdi3/__umoddi3 definitions would multiple-define against the
 * shim's. The shim is always on the rv32 system link line, so it is the
 * single provider there; ZeroCC's own codegen never calls these names
 * (rv32 i64 division is not lowered - see sanitizer-roadmap.md §4d).
 * On 64-bit targets the definitions stay (harmless: 64-bit div/mod is
 * native and nothing references the libgcc names). */
#if !(defined(__riscv_xlen) && __riscv_xlen == 32)
uint64_t __udivdi3(uint64_t u, uint64_t v) {
    return zc_udivmod64(u, v, 0);
}

uint64_t __umoddi3(uint64_t u, uint64_t v) {
    uint64_t r = 0;
    zc_udivmod64(u, v, &r);
    return r;
}

int64_t __divdi3(int64_t a, int64_t b) {
    int aneg = a < 0, bneg = b < 0;
    uint64_t ua = aneg ? (uint64_t)-a : (uint64_t)a;
    uint64_t ub = bneg ? (uint64_t)-b : (uint64_t)b;
    uint64_t q = zc_udivmod64(ua, ub, 0);
    return (aneg ^ bneg) ? -(int64_t)q : (int64_t)q;
}

int64_t __moddi3(int64_t a, int64_t b) {
    int aneg = a < 0;
    uint64_t ua = aneg ? (uint64_t)-a : (uint64_t)a;
    uint64_t ub = (b < 0) ? (uint64_t)-b : (uint64_t)b;
    uint64_t r = 0;
    zc_udivmod64(ua, ub, &r);
    return aneg ? -(int64_t)r : (int64_t)r;
}
#endif /* !(riscv xlen 32) */

