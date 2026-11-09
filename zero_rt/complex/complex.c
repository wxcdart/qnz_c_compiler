/* zero_rt/complex/complex.c - ZeroCC runtime helper for complex division.
 *
 * MIT License.
 *
 * Background:
 *   ZeroCC's IR lowering delegates `_Complex double` division (and
 *   `_Complex long double` where sizeof(long double) == 8) to `__divdc3`.
 *   In the static-musl build, the executable has no dynamic linker to dlopen
 *   libgcc_s.so.1 at runtime. This host module provides `__divdc3` directly
 *   inside the ZeroCC executable so that JIT dlsym(RTLD_DEFAULT, "__divdc3")
 *   resolves it from the process's own symbol table.
 *
 * Mathematical Algorithm:
 *   - Scaled complex division based on Smith's algorithm (CACM 1962, Alg 116).
 *   - Overflow avoidance: inputs are scaled by 0.5 when denominator magnitude
 *     approaches DBL_MAX.
 *   - Subnormal precision protection: intermediate values are scaled by
 *     1.0 / DBL_EPSILON when denominator magnitude is in subnormal range,
 *     preventing precision loss from coarse subnormal spacing.
 *   - Alternate algebraic ordering is selected when the denominator ratio is
 *     subnormal to avoid premature underflow.
 *   - Special-value handling conforms to ISO C99 Annex G.5.1 for infinity
 *     and NaN recovery.
 */

#include <float.h>

static inline double _zrt_fabs(double x) {
    return __builtin_fabs(x);
}

static inline double _zrt_copysign(double mag, double sgn) {
    return __builtin_copysign(mag, sgn);
}

double _Complex __divdc3(double a, double b, double c, double d) {
    const double max_limit = DBL_MAX * 0.5;
    const double min_normal = DBL_MIN;
    const double eps = DBL_EPSILON;
    const double scale_up = 1.0 / DBL_EPSILON;
    const double safe_max = max_limit * eps;

    double abs_c = _zrt_fabs(c);
    double abs_d = _zrt_fabs(d);
    double real_part, imag_part;

    if (abs_c < abs_d) {
        if (abs_d >= max_limit) {
            a *= 0.5;
            b *= 0.5;
            c *= 0.5;
            d *= 0.5;
        }
        if (abs_d < eps) {
            a *= scale_up;
            b *= scale_up;
            c *= scale_up;
            d *= scale_up;
        } else {
            double abs_a = _zrt_fabs(a);
            double abs_b = _zrt_fabs(b);
            if (((abs_a < min_normal) && (abs_b < safe_max) && (abs_d < safe_max)) ||
                ((abs_b < min_normal) && (abs_a < safe_max) && (abs_d < safe_max))) {
                a *= scale_up;
                b *= scale_up;
                c *= scale_up;
                d *= scale_up;
            }
        }
        double ratio = c / d;
        double denom = c * ratio + d;
        if (_zrt_fabs(ratio) > min_normal) {
            real_part = (a * ratio + b) / denom;
            imag_part = (b * ratio - a) / denom;
        } else {
            real_part = (c * (a / d) + b) / denom;
            imag_part = (c * (b / d) - a) / denom;
        }
    } else {
        if (abs_c >= max_limit) {
            a *= 0.5;
            b *= 0.5;
            c *= 0.5;
            d *= 0.5;
        }
        if (abs_c < eps) {
            a *= scale_up;
            b *= scale_up;
            c *= scale_up;
            d *= scale_up;
        } else {
            double abs_a = _zrt_fabs(a);
            double abs_b = _zrt_fabs(b);
            if (((abs_a < min_normal) && (abs_b < safe_max) && (abs_c < safe_max)) ||
                ((abs_b < min_normal) && (abs_a < safe_max) && (abs_c < safe_max))) {
                a *= scale_up;
                b *= scale_up;
                c *= scale_up;
                d *= scale_up;
            }
        }
        double ratio = d / c;
        double denom = d * ratio + c;
        if (_zrt_fabs(ratio) > min_normal) {
            real_part = (b * ratio + a) / denom;
            imag_part = (b - a * ratio) / denom;
        } else {
            real_part = (a + d * (b / c)) / denom;
            imag_part = (b - d * (a / c)) / denom;
        }
    }

    /* ISO C99 Annex G.5.1 NaN / Inf exceptional recovery */
    if (__builtin_isnan(real_part) && __builtin_isnan(imag_part)) {
        if (c == 0.0 && d == 0.0 && (!__builtin_isnan(a) || !__builtin_isnan(b))) {
            real_part = _zrt_copysign(__builtin_huge_val(), c) * a;
            imag_part = _zrt_copysign(__builtin_huge_val(), c) * b;
        } else if ((__builtin_isinf(a) || __builtin_isinf(b)) &&
                   __builtin_isfinite(c) && __builtin_isfinite(d)) {
            double inf_a = _zrt_copysign(__builtin_isinf(a) ? 1.0 : 0.0, a);
            double inf_b = _zrt_copysign(__builtin_isinf(b) ? 1.0 : 0.0, b);
            real_part = __builtin_huge_val() * (inf_a * c + inf_b * d);
            imag_part = __builtin_huge_val() * (inf_b * c - inf_a * d);
        } else if ((__builtin_isinf(c) || __builtin_isinf(d)) &&
                   __builtin_isfinite(a) && __builtin_isfinite(b)) {
            double inf_c = _zrt_copysign(__builtin_isinf(c) ? 1.0 : 0.0, c);
            double inf_d = _zrt_copysign(__builtin_isinf(d) ? 1.0 : 0.0, d);
            real_part = 0.0 * (a * inf_c + b * inf_d);
            imag_part = 0.0 * (b * inf_c - a * inf_d);
        }
    }

    double _Complex quotient;
    __real__ quotient = real_part;
    __imag__ quotient = imag_part;
    return quotient;
}
