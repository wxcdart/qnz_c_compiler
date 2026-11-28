/* zero_rt/core/soft_float.c
 *
 * IEEE-754 Single (binary32) and Double (binary64) Precision Soft-Float Runtime
 * for ZeroCC freestanding targets without hardware FPU (e.g. RV32I / RV32IMAC).
 *
 * Implements libgcc/compiler-rt ABI symbols:
 *   Single precision:
 *     __addsf3, __subsf3, __mulsf3, __divsf3, __negsf2
 *     __eqsf2, __nesf2, __ltsf2, __lesf2, __gtsf2, __gesf2, __unordsf2
 *     __extendsfdf2, __fixsfsi, __fixunssfsi, __floatsisf, __floatunsisf
 *     __fixsfdi, __fixunssfdi, __floatdisf, __floatundisf
 *   Double precision:
 *     __adddf3, __subdf3, __muldf3, __divdf3, __negdf2
 *     __eqdf2, __nedf2, __ltdf2, __ledf2, __gtdf2, __gedf2, __unorddf2
 *     __truncdfsf2, __fixdfsi, __fixunsdfsi, __floatsidf, __floatunsidf
 *     __fixdfdi, __fixunsdfdi, __floatdidf, __floatundidf
 */

#include <stdint.h>

typedef union { float f; uint32_t u; } zc_sf_u;
typedef union { double d; uint64_t u; } zc_df_u;

static inline uint32_t f2u(float f) { zc_sf_u x; x.f = f; return x.u; }
static inline float u2f(uint32_t u) { zc_sf_u x; x.u = u; return x.f; }
static inline uint64_t d2u(double d) { zc_df_u x; x.d = d; return x.u; }
static inline double u2d(uint64_t u) { zc_df_u x; x.u = u; return x.d; }

#define SF_EXP_BIAS  127
#define SF_EXP_MASK  0xFFU
#define SF_FRAC_MASK 0x7FFFFFU
#define SF_SIGN_MASK 0x80000000U
#define SF_QNAN_BIT  0x400000U

#define DF_EXP_BIAS  1023
#define DF_EXP_MASK  0x7FFULL
#define DF_FRAC_MASK 0x000FFFFFFFFFFFFFULL
#define DF_SIGN_MASK 0x8000000000000000ULL
#define DF_QNAN_BIT  0x0008000000000000ULL

static inline int sf_is_nan(uint32_t u) {
    return ((u & 0x7F800000U) == 0x7F800000U) && ((u & SF_FRAC_MASK) != 0);
}
static inline int sf_is_inf(uint32_t u) {
    return ((u & 0x7FFFFFFFU) == 0x7F800000U);
}
static inline int df_is_nan(uint64_t u) {
    return ((u & 0x7FF0000000000000ULL) == 0x7FF0000000000000ULL) && ((u & DF_FRAC_MASK) != 0);
}
static inline int df_is_inf(uint64_t u) {
    return ((u & 0x7FFFFFFFFFFFFFFFULL) == 0x7FF0000000000000ULL);
}

/* =========================================================================
 * Single-Precision (float) Comparisons & Unary
 * ========================================================================= */

float __negsf2(float a) {
    return u2f(f2u(a) ^ SF_SIGN_MASK);
}

int __unordsf2(float a, float b) {
    return sf_is_nan(f2u(a)) || sf_is_nan(f2u(b));
}

int __eqsf2(float a, float b) {
    uint32_t ua = f2u(a), ub = f2u(b);
    if (sf_is_nan(ua) || sf_is_nan(ub)) return 1;
    if (((ua | ub) & 0x7FFFFFFFU) == 0) return 0;
    return ua == ub ? 0 : 1;
}

int __nesf2(float a, float b) {
    return __eqsf2(a, b);
}

int __ltsf2(float a, float b) {
    uint32_t ua = f2u(a), ub = f2u(b);
    if (sf_is_nan(ua) || sf_is_nan(ub)) return 0;
    if (((ua | ub) & 0x7FFFFFFFU) == 0) return 0;
    uint32_t sa = ua >> 31, sb = ub >> 31;
    if (sa != sb) return sa ? -1 : 0;
    if (sa) return ua > ub ? -1 : 0;
    return ua < ub ? -1 : 0;
}

int __lesf2(float a, float b) {
    uint32_t ua = f2u(a), ub = f2u(b);
    if (sf_is_nan(ua) || sf_is_nan(ub)) return 1;
    if (((ua | ub) & 0x7FFFFFFFU) == 0) return 0;
    uint32_t sa = ua >> 31, sb = ub >> 31;
    if (sa != sb) return sa ? 0 : 1;
    if (sa) return ua >= ub ? 0 : 1;
    return ua <= ub ? 0 : 1;
}

int __gtsf2(float a, float b) {
    uint32_t ua = f2u(a), ub = f2u(b);
    if (sf_is_nan(ua) || sf_is_nan(ub)) return 0;
    if (((ua | ub) & 0x7FFFFFFFU) == 0) return 0;
    uint32_t sa = ua >> 31, sb = ub >> 31;
    if (sa != sb) return (!sa) ? 1 : 0;
    if (sa) return ua < ub ? 1 : 0;
    return ua > ub ? 1 : 0;
}

int __gesf2(float a, float b) {
    uint32_t ua = f2u(a), ub = f2u(b);
    if (sf_is_nan(ua) || sf_is_nan(ub)) return -1;
    if (((ua | ub) & 0x7FFFFFFFU) == 0) return 0;
    uint32_t sa = ua >> 31, sb = ub >> 31;
    if (sa != sb) return (!sa) ? 0 : -1;
    if (sa) return ua <= ub ? 0 : -1;
    return ua >= ub ? 0 : -1;
}

/* =========================================================================
 * Double-Precision (double) Comparisons & Unary
 * ========================================================================= */

double __negdf2(double a) {
    return u2d(d2u(a) ^ DF_SIGN_MASK);
}

int __unorddf2(double a, double b) {
    return df_is_nan(d2u(a)) || df_is_nan(d2u(b));
}

int __eqdf2(double a, double b) {
    uint64_t ua = d2u(a), ub = d2u(b);
    if (df_is_nan(ua) || df_is_nan(ub)) return 1;
    if (((ua | ub) & 0x7FFFFFFFFFFFFFFFULL) == 0) return 0;
    return ua == ub ? 0 : 1;
}

int __nedf2(double a, double b) {
    return __eqdf2(a, b);
}

int __ltdf2(double a, double b) {
    uint64_t ua = d2u(a), ub = d2u(b);
    if (df_is_nan(ua) || df_is_nan(ub)) return 0;
    if (((ua | ub) & 0x7FFFFFFFFFFFFFFFULL) == 0) return 0;
    uint64_t sa = ua >> 63, sb = ub >> 63;
    if (sa != sb) return sa ? -1 : 0;
    if (sa) return ua > ub ? -1 : 0;
    return ua < ub ? -1 : 0;
}

int __ledf2(double a, double b) {
    uint64_t ua = d2u(a), ub = d2u(b);
    if (df_is_nan(ua) || df_is_nan(ub)) return 1;
    if (((ua | ub) & 0x7FFFFFFFFFFFFFFFULL) == 0) return 0;
    uint64_t sa = ua >> 63, sb = ub >> 63;
    if (sa != sb) return sa ? 0 : 1;
    if (sa) return ua >= ub ? 0 : 1;
    return ua <= ub ? 0 : 1;
}

int __gtdf2(double a, double b) {
    uint64_t ua = d2u(a), ub = d2u(b);
    if (df_is_nan(ua) || df_is_nan(ub)) return 0;
    if (((ua | ub) & 0x7FFFFFFFFFFFFFFFULL) == 0) return 0;
    uint64_t sa = ua >> 63, sb = ub >> 63;
    if (sa != sb) return (!sa) ? 1 : 0;
    if (sa) return ua < ub ? 1 : 0;
    return ua > ub ? 1 : 0;
}

int __gedf2(double a, double b) {
    uint64_t ua = d2u(a), ub = d2u(b);
    if (df_is_nan(ua) || df_is_nan(ub)) return -1;
    if (((ua | ub) & 0x7FFFFFFFFFFFFFFFULL) == 0) return 0;
    uint64_t sa = ua >> 63, sb = ub >> 63;
    if (sa != sb) return (!sa) ? 0 : -1;
    if (sa) return ua <= ub ? 0 : -1;
    return ua >= ub ? 0 : -1;
}

/* =========================================================================
 * Single-Precision Arithmetic (IEEE-754 Round to Nearest Even)
 * ========================================================================= */

static inline uint32_t sf_round_pack(uint32_t sign, int exp, uint64_t m) {
    /* m is aligned such that bit 26 is the implicit 1 (bits 25..3 are frac, bits 2..0 are guard/round/sticky) */
    if (m == 0) return sign << 31;

    /* Normalize if high bits set */
    while (m >= (1ULL << 27)) {
        uint64_t low = m & 1;
        m >>= 1;
        m |= low;
        exp++;
    }
    while (m < (1ULL << 26) && exp > 0) {
        m <<= 1;
        exp--;
    }

    if (exp >= 255) {
        /* Overflow to infinity */
        return (sign << 31) | 0x7F800000U;
    }

    /* Subnormal handling */
    if (exp <= 0) {
        int shift = 1 - exp;
        if (shift > 31) return sign << 31;
        uint64_t sticky = 0;
        if ((m & ((1ULL << shift) - 1)) != 0) sticky = 1;
        m = (m >> shift) | sticky;
        exp = 0;
    }

    /* Round to nearest even */
    uint64_t round_bits = m & 0x7;
    m >>= 3;
    if (round_bits > 4 || (round_bits == 4 && (m & 1))) {
        m++;
        if (exp == 0 && (m & (1ULL << 23))) {
            exp = 1;
        } else if (m >= (1ULL << 24)) {
            m >>= 1;
            exp++;
            if (exp >= 255) return (sign << 31) | 0x7F800000U;
        }
    }

    return (sign << 31) | ((uint32_t)exp << 23) | (uint32_t)(m & SF_FRAC_MASK);
}

float __addsf3(float a, float b) {
    uint32_t ua = f2u(a), ub = f2u(b);
    if (sf_is_nan(ua)) return u2f(ua | SF_QNAN_BIT);
    if (sf_is_nan(ub)) return u2f(ub | SF_QNAN_BIT);

    int inf_a = sf_is_inf(ua), inf_b = sf_is_inf(ub);
    if (inf_a && inf_b) {
        if ((ua ^ ub) & SF_SIGN_MASK) return u2f(0x7FC00000U); /* inf - inf = NaN */
        return a;
    }
    if (inf_a) return a;
    if (inf_b) return b;

    if ((ua & 0x7FFFFFFFU) == 0) return b;
    if ((ub & 0x7FFFFFFFU) == 0) return a;

    uint32_t sa = ua >> 31, sb = ub >> 31;
    int ea = (ua >> 23) & 0xFF, eb = (ub >> 23) & 0xFF;
    uint64_t ma = ua & SF_FRAC_MASK, mb = ub & SF_FRAC_MASK;

    if (ea == 0) ea = 1; else ma |= (1ULL << 23);
    if (eb == 0) eb = 1; else mb |= (1ULL << 23);

    /* Align to 3 fraction guard/round/sticky bits */
    ma <<= 3;
    mb <<= 3;

    if (ea < eb || (ea == eb && ma < mb)) {
        uint32_t ts = sa; sa = sb; sb = ts;
        int te = ea; ea = eb; eb = te;
        uint64_t tm = ma; ma = mb; mb = tm;
    }

    int diff = ea - eb;
    if (diff > 31) {
        mb = 1; /* sticky */
    } else if (diff > 0) {
        uint64_t sticky = (mb & ((1ULL << diff) - 1)) ? 1 : 0;
        mb = (mb >> diff) | sticky;
    }

    uint64_t m;
    uint32_t sign = sa;
    if (sa == sb) {
        m = ma + mb;
    } else {
        if (ma == mb) return u2f(0); /* +0.0 */
        m = ma - mb;
    }

    return u2f(sf_round_pack(sign, ea, m));
}

float __subsf3(float a, float b) {
    return __addsf3(a, __negsf2(b));
}

float __mulsf3(float a, float b) {
    uint32_t ua = f2u(a), ub = f2u(b);
    if (sf_is_nan(ua)) return u2f(ua | SF_QNAN_BIT);
    if (sf_is_nan(ub)) return u2f(ub | SF_QNAN_BIT);

    uint32_t sign = (ua ^ ub) >> 31;
    int inf_a = sf_is_inf(ua), inf_b = sf_is_inf(ub);
    int zero_a = (ua & 0x7FFFFFFFU) == 0, zero_b = (ub & 0x7FFFFFFFU) == 0;

    if ((inf_a && zero_b) || (inf_b && zero_a)) return u2f(0x7FC00000U); /* 0 * inf = NaN */
    if (inf_a || inf_b) return u2f((sign << 31) | 0x7F800000U);
    if (zero_a || zero_b) return u2f(sign << 31);

    int ea = (ua >> 23) & 0xFF, eb = (ub >> 23) & 0xFF;
    uint64_t ma = ua & SF_FRAC_MASK, mb = ub & SF_FRAC_MASK;

    if (ea == 0) {
        ea = 1;
        while ((ma & (1ULL << 23)) == 0) { ma <<= 1; ea--; }
    } else {
        ma |= (1ULL << 23);
    }

    if (eb == 0) {
        eb = 1;
        while ((mb & (1ULL << 23)) == 0) { mb <<= 1; eb--; }
    } else {
        mb |= (1ULL << 23);
    }

    int exp = ea + eb - SF_EXP_BIAS;
    uint64_t prod = ma * mb; /* 46 or 47 bits */

    /* prod has implicit bit at bit 46 (or 47); align bit 46 to bit 26 with 3 guard bits */
    uint64_t sticky = (prod & ((1ULL << 20) - 1)) ? 1 : 0;
    uint64_t m = (prod >> 20) | sticky;

    return u2f(sf_round_pack(sign, exp, m));
}

float __divsf3(float a, float b) {
    uint32_t ua = f2u(a), ub = f2u(b);
    if (sf_is_nan(ua)) return u2f(ua | SF_QNAN_BIT);
    if (sf_is_nan(ub)) return u2f(ub | SF_QNAN_BIT);

    uint32_t sign = (ua ^ ub) >> 31;
    int inf_a = sf_is_inf(ua), inf_b = sf_is_inf(ub);
    int zero_a = (ua & 0x7FFFFFFFU) == 0, zero_b = (ub & 0x7FFFFFFFU) == 0;

    if (inf_a && inf_b) return u2f(0x7FC00000U); /* inf / inf = NaN */
    if (zero_a && zero_b) return u2f(0x7FC00000U); /* 0 / 0 = NaN */
    if (inf_a || zero_b) return u2f((sign << 31) | 0x7F800000U);
    if (zero_a || inf_b) return u2f(sign << 31);

    int ea = (ua >> 23) & 0xFF, eb = (ub >> 23) & 0xFF;
    uint64_t ma = ua & SF_FRAC_MASK, mb = ub & SF_FRAC_MASK;

    if (ea == 0) {
        ea = 1;
        while ((ma & (1ULL << 23)) == 0) { ma <<= 1; ea--; }
    } else {
        ma |= (1ULL << 23);
    }

    if (eb == 0) {
        eb = 1;
        while ((mb & (1ULL << 23)) == 0) { mb <<= 1; eb--; }
    } else {
        mb |= (1ULL << 23);
    }

    int exp = ea - eb + SF_EXP_BIAS;
    /* Binary restoring division for 27 bits of quotient (bit 26 down to bit 0) */
    uint64_t q = 0;
    uint64_t rem = ma;
    for (int i = 26; i >= 0; i--) {
        if (rem >= mb) {
            rem -= mb;
            q |= (1ULL << i);
        }
        rem <<= 1;
    }
    if (rem != 0) q |= 1;

    return u2f(sf_round_pack(sign, exp, q));
}

/* =========================================================================
 * Double-Precision Arithmetic (IEEE-754 Round to Nearest Even)
 * ========================================================================= */

static inline uint64_t df_round_pack(uint64_t sign, int exp, uint64_t m, uint64_t sticky) {
    /* m has implicit 1 at bit 55 (bits 54..3 are frac, bits 2..0 are guard/round/sticky) */
    if (m == 0 && sticky == 0) return sign << 63;

    m |= (sticky ? 1 : 0);

    while (m >= (1ULL << 56)) {
        uint64_t low = m & 1;
        m >>= 1;
        m |= low;
        exp++;
    }
    while (m < (1ULL << 55) && exp > 0) {
        m <<= 1;
        exp--;
    }

    if (exp >= 2047) {
        return (sign << 63) | 0x7FF0000000000000ULL;
    }

    if (exp <= 0) {
        int shift = 1 - exp;
        if (shift > 63) return sign << 63;
        uint64_t stk = (m & ((1ULL << shift) - 1)) ? 1 : 0;
        m = (m >> shift) | stk;
        exp = 0;
    }

    uint64_t round_bits = m & 0x7;
    m >>= 3;
    if (round_bits > 4 || (round_bits == 4 && (m & 1))) {
        m++;
        if (exp == 0 && (m & (1ULL << 52))) {
            exp = 1;
        } else if (m >= (1ULL << 53)) {
            m >>= 1;
            exp++;
            if (exp >= 2047) return (sign << 63) | 0x7FF0000000000000ULL;
        }
    }

    return (sign << 63) | ((uint64_t)exp << 52) | (m & DF_FRAC_MASK);
}

double __adddf3(double a, double b) {
    uint64_t ua = d2u(a), ub = d2u(b);
    if (df_is_nan(ua)) return u2d(ua | DF_QNAN_BIT);
    if (df_is_nan(ub)) return u2d(ub | DF_QNAN_BIT);

    int inf_a = df_is_inf(ua), inf_b = df_is_inf(ub);
    if (inf_a && inf_b) {
        if ((ua ^ ub) & DF_SIGN_MASK) return u2d(0x7FF8000000000000ULL);
        return a;
    }
    if (inf_a) return a;
    if (inf_b) return b;

    if ((ua & 0x7FFFFFFFFFFFFFFFULL) == 0) return b;
    if ((ub & 0x7FFFFFFFFFFFFFFFULL) == 0) return a;

    uint64_t sa = ua >> 63, sb = ub >> 63;
    int ea = (ua >> 52) & 0x7FF, eb = (ub >> 52) & 0x7FF;
    uint64_t ma = ua & DF_FRAC_MASK, mb = ub & DF_FRAC_MASK;

    if (ea == 0) ea = 1; else ma |= (1ULL << 52);
    if (eb == 0) eb = 1; else mb |= (1ULL << 52);

    ma <<= 3;
    mb <<= 3;

    if (ea < eb || (ea == eb && ma < mb)) {
        uint64_t ts = sa; sa = sb; sb = ts;
        int te = ea; ea = eb; eb = te;
        uint64_t tm = ma; ma = mb; mb = tm;
    }

    int diff = ea - eb;
    if (diff > 63) {
        mb = 1;
    } else if (diff > 0) {
        uint64_t sticky = (mb & ((1ULL << diff) - 1)) ? 1 : 0;
        mb = (mb >> diff) | sticky;
    }

    uint64_t m;
    if (sa == sb) {
        m = ma + mb;
    } else {
        if (ma == mb) return u2d(0);
        m = ma - mb;
    }

    return u2d(df_round_pack(sa, ea, m, 0));
}

double __subdf3(double a, double b) {
    return __adddf3(a, __negdf2(b));
}

/* 64x64 -> 128-bit unsigned multiplication using 32-bit halves */
static void mul64x64(uint64_t a, uint64_t b, uint64_t *hi, uint64_t *lo) {
    uint64_t a0 = (uint32_t)a, a1 = a >> 32;
    uint64_t b0 = (uint32_t)b, b1 = b >> 32;

    uint64_t p0 = a0 * b0;
    uint64_t p1 = a0 * b1;
    uint64_t p2 = a1 * b0;
    uint64_t p3 = a1 * b1;

    uint64_t mid = p1 + (p0 >> 32) + (uint32_t)p2;
    *lo = (p0 & 0xFFFFFFFFULL) | (mid << 32);
    *hi = p3 + (mid >> 32) + (p2 >> 32);
}

double __muldf3(double a, double b) {
    uint64_t ua = d2u(a), ub = d2u(b);
    if (df_is_nan(ua)) return u2d(ua | DF_QNAN_BIT);
    if (df_is_nan(ub)) return u2d(ub | DF_QNAN_BIT);

    uint64_t sign = (ua ^ ub) >> 63;
    int inf_a = df_is_inf(ua), inf_b = df_is_inf(ub);
    int zero_a = (ua & 0x7FFFFFFFFFFFFFFFULL) == 0, zero_b = (ub & 0x7FFFFFFFFFFFFFFFULL) == 0;

    if ((inf_a && zero_b) || (inf_b && zero_a)) return u2d(0x7FF8000000000000ULL);
    if (inf_a || inf_b) return u2d((sign << 63) | 0x7FF0000000000000ULL);
    if (zero_a || zero_b) return u2d(sign << 63);

    int ea = (ua >> 52) & 0x7FF, eb = (ub >> 52) & 0x7FF;
    uint64_t ma = ua & DF_FRAC_MASK, mb = ub & DF_FRAC_MASK;

    if (ea == 0) {
        ea = 1;
        while ((ma & (1ULL << 52)) == 0) { ma <<= 1; ea--; }
    } else {
        ma |= (1ULL << 52);
    }

    if (eb == 0) {
        eb = 1;
        while ((mb & (1ULL << 52)) == 0) { mb <<= 1; eb--; }
    } else {
        mb |= (1ULL << 52);
    }

    int exp = ea + eb - DF_EXP_BIAS;
    uint64_t phi, plo;
    mul64x64(ma, mb, &phi, &plo); /* 106-bit product */

    /* Product bits: bit (52+52) = bit 104 is set (or 105).
     * In (phi, plo): phi has bits 64..105, which is bits 0..41 of phi.
     * We want mantissa with implicit 1 at bit 55 and 3 guard bits.
     * Total shift from 104 to 55 is 49 bits right.
     * So result m comes from (phi << 15) | (plo >> 49).
     */
    uint64_t m = (phi << 15) | (plo >> 49);
    uint64_t sticky = (plo & ((1ULL << 49) - 1)) ? 1 : 0;

    return u2d(df_round_pack(sign, exp, m, sticky));
}

double __divdf3(double a, double b) {
    uint64_t ua = d2u(a), ub = d2u(b);
    if (df_is_nan(ua)) return u2d(ua | DF_QNAN_BIT);
    if (df_is_nan(ub)) return u2d(ub | DF_QNAN_BIT);

    uint64_t sign = (ua ^ ub) >> 63;
    int inf_a = df_is_inf(ua), inf_b = df_is_inf(ub);
    int zero_a = (ua & 0x7FFFFFFFFFFFFFFFULL) == 0, zero_b = (ub & 0x7FFFFFFFFFFFFFFFULL) == 0;

    if (inf_a && inf_b) return u2d(0x7FF8000000000000ULL);
    if (zero_a && zero_b) return u2d(0x7FF8000000000000ULL);
    if (inf_a || zero_b) return u2d((sign << 63) | 0x7FF0000000000000ULL);
    if (zero_a || inf_b) return u2d(sign << 63);

    int ea = (ua >> 52) & 0x7FF, eb = (ub >> 52) & 0x7FF;
    uint64_t ma = ua & DF_FRAC_MASK, mb = ub & DF_FRAC_MASK;

    if (ea == 0) {
        ea = 1;
        while ((ma & (1ULL << 52)) == 0) { ma <<= 1; ea--; }
    } else {
        ma |= (1ULL << 52);
    }

    if (eb == 0) {
        eb = 1;
        while ((mb & (1ULL << 52)) == 0) { mb <<= 1; eb--; }
    } else {
        mb |= (1ULL << 52);
    }

    int exp = ea - eb + DF_EXP_BIAS;

    /* Binary restoring division for 56 bits of quotient (implicit bit 55 down to bit 0) */
    uint64_t q = 0;
    uint64_t rem = ma;
    for (int i = 55; i >= 0; i--) {
        if (rem >= mb) {
            rem -= mb;
            q |= (1ULL << i);
        }
        rem <<= 1;
    }
    uint64_t sticky = rem ? 1 : 0;

    return u2d(df_round_pack(sign, exp, q, sticky));
}

/* =========================================================================
 * Conversions: Float <-> Double
 * ========================================================================= */

double __extendsfdf2(float a) {
    uint32_t u = f2u(a);
    uint64_t sign = (uint64_t)(u >> 31);
    uint32_t exp = (u >> 23) & 0xFF;
    uint32_t frac = u & SF_FRAC_MASK;

    if (exp == 0xFF) {
        uint64_t dfrac = ((uint64_t)frac) << (52 - 23);
        if (frac != 0) dfrac |= DF_QNAN_BIT;
        return u2d((sign << 63) | 0x7FF0000000000000ULL | dfrac);
    }
    if (exp == 0) {
        if (frac == 0) return u2d(sign << 63);
        int shift = 0;
        while ((frac & 0x800000U) == 0) { frac <<= 1; shift++; }
        exp = 127 - shift + 1;
        frac &= SF_FRAC_MASK;
    }
    uint64_t dexp = (uint64_t)(exp - SF_EXP_BIAS + DF_EXP_BIAS);
    uint64_t dfrac = ((uint64_t)frac) << (52 - 23);
    return u2d((sign << 63) | (dexp << 52) | dfrac);
}

float __truncdfsf2(double a) {
    uint64_t u = d2u(a);
    uint32_t sign = (uint32_t)(u >> 63);
    int exp = (int)((u >> 52) & 0x7FF);
    uint64_t frac = u & DF_FRAC_MASK;

    if (exp == 0x7FF) {
        uint32_t sfrac = (uint32_t)(frac >> (52 - 23));
        if (frac != 0) sfrac |= SF_QNAN_BIT;
        return u2f((sign << 31) | 0x7F800000U | sfrac);
    }
    if (exp == 0 && frac == 0) return u2f(sign << 31);

    int sexp = exp - DF_EXP_BIAS + SF_EXP_BIAS;
    uint64_t m = (exp == 0) ? frac : (frac | (1ULL << 52));

    /* Align to 26 bits (bit 26 is implicit 1, bits 2..0 are guard/round/sticky) */
    uint64_t sticky = (m & ((1ULL << 26) - 1)) ? 1 : 0;
    uint64_t sm = (m >> 26) | sticky;

    return u2f(sf_round_pack(sign, sexp, sm));
}

/* =========================================================================
 * Conversions: Int <-> Float
 * ========================================================================= */

int __fixsfsi(float a) {
    uint32_t u = f2u(a);
    if (sf_is_nan(u) || (u & 0x7FFFFFFFU) == 0) return 0;
    int exp = ((u >> 23) & 0xFF) - SF_EXP_BIAS;
    if (exp < 0) return 0;
    uint32_t sign = u >> 31;
    if (exp >= 31) return sign ? (-2147483647 - 1) : 2147483647;

    uint32_t m = (u & SF_FRAC_MASK) | (1U << 23);
    uint32_t val;
    if (exp >= 23) val = m << (exp - 23);
    else val = m >> (23 - exp);

    return sign ? -(int)val : (int)val;
}

unsigned int __fixunssfsi(float a) {
    uint32_t u = f2u(a);
    if (sf_is_nan(u) || (u >> 31)) return 0;
    int exp = ((u >> 23) & 0xFF) - SF_EXP_BIAS;
    if (exp < 0) return 0;
    if (exp >= 32) return 0xFFFFFFFFU;

    uint32_t m = (u & SF_FRAC_MASK) | (1U << 23);
    if (exp >= 23) return m << (exp - 23);
    return m >> (23 - exp);
}

int64_t __fixsfdi(float a) {
    uint32_t u = f2u(a);
    if (sf_is_nan(u) || (u & 0x7FFFFFFFU) == 0) return 0;
    int exp = ((u >> 23) & 0xFF) - SF_EXP_BIAS;
    if (exp < 0) return 0;
    uint32_t sign = u >> 31;
    if (exp >= 63) return sign ? ((int64_t)1 << 63) : ~((int64_t)1 << 63);

    uint64_t m = (u & SF_FRAC_MASK) | (1ULL << 23);
    uint64_t val;
    if (exp >= 23) val = m << (exp - 23);
    else val = m >> (23 - exp);

    return sign ? -(int64_t)val : (int64_t)val;
}

uint64_t __fixunssfdi(float a) {
    uint32_t u = f2u(a);
    if (sf_is_nan(u) || (u >> 31)) return 0;
    int exp = ((u >> 23) & 0xFF) - SF_EXP_BIAS;
    if (exp < 0) return 0;
    if (exp >= 64) return ~0ULL;

    uint64_t m = (u & SF_FRAC_MASK) | (1ULL << 23);
    if (exp >= 23) return m << (exp - 23);
    return m >> (23 - exp);
}

float __floatsisf(int a) {
    if (a == 0) return u2f(0);
    uint32_t sign = a < 0 ? 1 : 0;
    uint32_t u = sign ? (uint32_t)-a : (uint32_t)a;

    int exp = SF_EXP_BIAS + 23;
    uint64_t m = (uint64_t)u << 3;
    return u2f(sf_round_pack(sign, exp, m));
}

float __floatunsisf(unsigned int a) {
    if (a == 0) return u2f(0);
    int exp = SF_EXP_BIAS + 23;
    uint64_t m = (uint64_t)a << 3;
    return u2f(sf_round_pack(0, exp, m));
}

float __floatdisf(int64_t a) {
    if (a == 0) return u2f(0);
    uint32_t sign = a < 0 ? 1 : 0;
    uint64_t u = sign ? (uint64_t)-a : (uint64_t)a;

    int exp = SF_EXP_BIAS + 23;
    /* Scale u so bit 26 is aligned */
    while (u >= (1ULL << 24)) {
        uint64_t low = u & 1;
        u >>= 1;
        u |= low;
        exp++;
    }
    uint64_t m = u << 3;
    return u2f(sf_round_pack(sign, exp, m));
}

float __floatundisf(uint64_t a) {
    if (a == 0) return u2f(0);
    int exp = SF_EXP_BIAS + 23;
    while (a >= (1ULL << 24)) {
        uint64_t low = a & 1;
        a >>= 1;
        a |= low;
        exp++;
    }
    uint64_t m = a << 3;
    return u2f(sf_round_pack(0, exp, m));
}

/* =========================================================================
 * Conversions: Int <-> Double
 * ========================================================================= */

int __fixdfsi(double a) {
    uint64_t u = d2u(a);
    if (df_is_nan(u) || (u & 0x7FFFFFFFFFFFFFFFULL) == 0) return 0;
    int exp = ((u >> 52) & 0x7FF) - DF_EXP_BIAS;
    if (exp < 0) return 0;
    uint64_t sign = u >> 63;
    if (exp >= 31) return sign ? (-2147483647 - 1) : 2147483647;

    uint64_t m = (u & DF_FRAC_MASK) | (1ULL << 52);
    uint64_t val;
    if (exp >= 52) val = m << (exp - 52);
    else val = m >> (52 - exp);

    return sign ? -(int)val : (int)val;
}

unsigned int __fixunsdfsi(double a) {
    uint64_t u = d2u(a);
    if (df_is_nan(u) || (u >> 63)) return 0;
    int exp = ((u >> 52) & 0x7FF) - DF_EXP_BIAS;
    if (exp < 0) return 0;
    if (exp >= 32) return 0xFFFFFFFFU;

    uint64_t m = (u & DF_FRAC_MASK) | (1ULL << 52);
    if (exp >= 52) return (unsigned int)(m << (exp - 52));
    return (unsigned int)(m >> (52 - exp));
}

int64_t __fixdfdi(double a) {
    uint64_t u = d2u(a);
    if (df_is_nan(u) || (u & 0x7FFFFFFFFFFFFFFFULL) == 0) return 0;
    int exp = ((u >> 52) & 0x7FF) - DF_EXP_BIAS;
    if (exp < 0) return 0;
    uint64_t sign = u >> 63;
    if (exp >= 63) return sign ? ((int64_t)1 << 63) : ~((int64_t)1 << 63);

    uint64_t m = (u & DF_FRAC_MASK) | (1ULL << 52);
    uint64_t val;
    if (exp >= 52) val = m << (exp - 52);
    else val = m >> (52 - exp);

    return sign ? -(int64_t)val : (int64_t)val;
}

uint64_t __fixunsdfdi(double a) {
    uint64_t u = d2u(a);
    if (df_is_nan(u) || (u >> 63)) return 0;
    int exp = ((u >> 52) & 0x7FF) - DF_EXP_BIAS;
    if (exp < 0) return 0;
    if (exp >= 64) return ~0ULL;

    uint64_t m = (u & DF_FRAC_MASK) | (1ULL << 52);
    if (exp >= 52) return m << (exp - 52);
    return m >> (52 - exp);
}

double __floatsidf(int a) {
    if (a == 0) return u2d(0);
    uint64_t sign = a < 0 ? 1 : 0;
    uint32_t u = sign ? (uint32_t)-a : (uint32_t)a;

    int exp = DF_EXP_BIAS + 52;
    uint64_t m = (uint64_t)u << 3;
    return u2d(df_round_pack(sign, exp, m, 0));
}

double __floatunsidf(unsigned int a) {
    if (a == 0) return u2d(0);
    int exp = DF_EXP_BIAS + 52;
    uint64_t m = (uint64_t)a << 3;
    return u2d(df_round_pack(0, exp, m, 0));
}

double __floatdidf(int64_t a) {
    if (a == 0) return u2d(0);
    uint64_t sign = a < 0 ? 1 : 0;
    uint64_t u = sign ? (uint64_t)-a : (uint64_t)a;

    int exp = DF_EXP_BIAS + 52;
    uint64_t sticky = 0;
    while (u >= (1ULL << 53)) {
        sticky |= (u & 1);
        u >>= 1;
        exp++;
    }
    uint64_t m = u << 3;
    return u2d(df_round_pack(sign, exp, m, sticky));
}

double __floatundidf(uint64_t a) {
    if (a == 0) return u2d(0);
    int exp = DF_EXP_BIAS + 52;
    uint64_t sticky = 0;
    while (a >= (1ULL << 53)) {
        sticky |= (a & 1);
        a >>= 1;
        exp++;
    }
    uint64_t m = a << 3;
    return u2d(df_round_pack(0, exp, m, sticky));
}
