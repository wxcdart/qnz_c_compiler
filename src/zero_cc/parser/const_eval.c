/* Compile-time constant evaluation (Sprint 114 Tier C: converted from
 * const_eval.h; the header keeps the ZCConstVal value model + prototypes). */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "zero_cc/types/types.h"
#include "zero_cc/types/registry.h"
#include "zero_cc/types/global_vars.h"


#include "zero_cc/parser/const_eval.h"  /* ZCConstVal model + prototypes */


ZCConstVal zc_cv_int(int64_t i) {
    ZCConstVal v;
    v.kind = ZC_CV_INT;
    v.i = i;
    v.r = (double)i;
    v.im = 0.0;
    v.hi = 0;
    v.w128 = false;
    v.signed_ = false;
    return v;
}

ZCConstVal zc_cv_float(double r) {
    ZCConstVal v;
    v.kind = ZC_CV_FLOAT;
    v.i = (int64_t)r;
    v.r = r;
    v.im = 0.0;
    v.hi = 0;
    v.w128 = false;
    v.signed_ = false;
    return v;
}

ZCConstVal zc_cv_complex(double r, double im) {
    ZCConstVal v;
    v.kind = ZC_CV_COMPLEX;
    v.i = (int64_t)r;
    v.r = r;
    v.im = im;
    v.hi = 0;
    v.w128 = false;
    v.signed_ = false;
    return v;
}

/* Promote a 64-bit int value to 128 bits (sign- or zero-extend). A no-op
 * for values that are already 128-bit. */
ZCConstVal zc_cv_promote128(ZCConstVal v) {
    if (v.kind == ZC_CV_INT && !v.w128) {
        v.hi = (v.signed_ && v.i < 0) ? ~0ULL : 0;
        v.w128 = true;
    }
    return v;
}

/* True (negative) when a signed 128-bit value's top bit is set. */
bool zc_cv128_negative(ZCConstVal v) {
    return v.w128 && v.signed_ && (v.hi >> 63) != 0;
}

/* 128-bit two's-complement negation (in place, w128 preserved). */
ZCConstVal zc_cv128_neg(ZCConstVal v) {
    if (v.kind == ZC_CV_INT && v.w128) {
        uint64_t lo = (uint64_t)v.i;
        uint64_t nlo = ~lo + 1;
        v.i = (int64_t)nlo;
        v.hi = ~v.hi + (nlo == 0);
    } else if (v.kind == ZC_CV_INT) {
        v.i = -v.i;
        v.signed_ = true;
    }
    return v;
}

bool zc_cv_is_true(ZCConstVal v) {
    if (v.kind == ZC_CV_COMPLEX) return v.r != 0.0 || v.im != 0.0;
    if (v.kind == ZC_CV_FLOAT) return v.r != 0.0;
    if (v.w128) return v.i != 0 || v.hi != 0;
    return v.i != 0;
}

ZCConstVal zc_cv_cast(ZCConstVal v, ZeroCCType ty) {
    if (ty.is_complex) {
        double r = (v.kind == ZC_CV_INT) ? (double)v.i : v.r;
        double im = (v.kind == ZC_CV_COMPLEX) ? v.im : 0.0;
        return zc_cv_complex(r, im);
    }
    if (ty.tag == TYPE_FLOAT || ty.tag == TYPE_DOUBLE) {
        double r = (v.kind == ZC_CV_INT) ? (double)v.i : v.r;
        return zc_cv_float(r);
    }
    /* GNU __int128 cast target: widen the 64-bit value into (hi, lo),
     * zero-extended for __uint128_t, sign-extended for __int128. Casting a
     * 128-bit value to a 64-bit type truncates to the low half. */
    if (ty.tag == TYPE_INT128 || ty.tag == TYPE_UINT128) {
        ZCConstVal w = (v.kind == ZC_CV_INT) ? v : zc_cv_int((int64_t)v.r);
        w.signed_ = (ty.tag == TYPE_INT128);
        if (!w.w128) {
            w.hi = (w.signed_ && w.i < 0) ? ~0ULL : 0;
            w.w128 = true;
        }
        return w;
    }
    if (v.w128 && v.kind == ZC_CV_INT) {
        v.hi = 0;  /* truncate to the low 64 bits */
        v.w128 = false;
    }
    int64_t val = (v.kind == ZC_CV_INT) ? v.i : (int64_t)v.r;
    uint64_t u = (uint64_t)val;
    switch (ty.tag) {
        case TYPE_BOOL:   u = u ? 1 : 0; break;
        case TYPE_INT8:   u &= 0xFFULL;       u = (uint64_t)(((int64_t)(u << 56)) >> 56); break;
        case TYPE_UINT8:  u &= 0xFFULL; break;
        case TYPE_INT16:  u &= 0xFFFFULL;     u = (uint64_t)(((int64_t)(u << 48)) >> 48); break;
        case TYPE_UINT16: u &= 0xFFFFULL; break;
        case TYPE_INT32:  u &= 0xFFFFFFFFULL; u = (uint64_t)(((int64_t)(u << 32)) >> 32); break;
        case TYPE_UINT32: u &= 0xFFFFFFFFULL; break;
        case TYPE_INT64:
        case TYPE_UINT64:
        default:
            break;
    }
    return zc_cv_int((int64_t)u);
}

ZCConstVal zc_cv_add(ZCConstVal a, ZCConstVal b) {
    if (a.kind == ZC_CV_COMPLEX || b.kind == ZC_CV_COMPLEX) {
        double ar = (a.kind == ZC_CV_INT) ? (double)a.i : a.r;
        double ai = (a.kind == ZC_CV_COMPLEX) ? a.im : 0.0;
        double br = (b.kind == ZC_CV_INT) ? (double)b.i : b.r;
        double bi = (b.kind == ZC_CV_COMPLEX) ? b.im : 0.0;
        return zc_cv_complex(ar + br, ai + bi);
    }
    if (a.kind == ZC_CV_FLOAT || b.kind == ZC_CV_FLOAT) {
        double ar = (a.kind == ZC_CV_INT) ? (double)a.i : a.r;
        double br = (b.kind == ZC_CV_INT) ? (double)b.i : b.r;
        return zc_cv_float(ar + br);
    }
    if (a.w128 || b.w128) {  /* 128-bit add (Sprint 109) */
        a = zc_cv_promote128(a); b = zc_cv_promote128(b);
        uint64_t alo = (uint64_t)a.i, blo = (uint64_t)b.i;
        ZCConstVal r = zc_cv_int((int64_t)(alo + blo));
        r.hi = a.hi + b.hi + (alo + blo < alo);
        r.w128 = true;
        r.signed_ = a.signed_ && b.signed_;
        return r;
    }
    return zc_cv_int(a.i + b.i);
}

ZCConstVal zc_cv_sub(ZCConstVal a, ZCConstVal b) {
    if (a.kind == ZC_CV_COMPLEX || b.kind == ZC_CV_COMPLEX) {
        double ar = (a.kind == ZC_CV_INT) ? (double)a.i : a.r;
        double ai = (a.kind == ZC_CV_COMPLEX) ? a.im : 0.0;
        double br = (b.kind == ZC_CV_INT) ? (double)b.i : b.r;
        double bi = (b.kind == ZC_CV_COMPLEX) ? b.im : 0.0;
        return zc_cv_complex(ar - br, ai - bi);
    }
    if (a.kind == ZC_CV_FLOAT || b.kind == ZC_CV_FLOAT) {
        double ar = (a.kind == ZC_CV_INT) ? (double)a.i : a.r;
        double br = (b.kind == ZC_CV_INT) ? (double)b.i : b.r;
        return zc_cv_float(ar - br);
    }
    if (a.w128 || b.w128) {  /* 128-bit sub (Sprint 109) */
        a = zc_cv_promote128(a); b = zc_cv_promote128(b);
        uint64_t alo = (uint64_t)a.i, blo = (uint64_t)b.i;
        ZCConstVal r = zc_cv_int((int64_t)(alo - blo));
        r.hi = a.hi - b.hi - (alo < blo);
        r.w128 = true;
        r.signed_ = a.signed_ && b.signed_;
        return r;
    }
    return zc_cv_int(a.i - b.i);
}

ZCConstVal zc_cv_mul(ZCConstVal a, ZCConstVal b) {
    if (a.kind == ZC_CV_COMPLEX || b.kind == ZC_CV_COMPLEX) {
        double ar = (a.kind == ZC_CV_INT) ? (double)a.i : a.r;
        double ai = (a.kind == ZC_CV_COMPLEX) ? a.im : 0.0;
        double br = (b.kind == ZC_CV_INT) ? (double)b.i : b.r;
        double bi = (b.kind == ZC_CV_COMPLEX) ? b.im : 0.0;
        return zc_cv_complex(ar * br - ai * bi, ar * bi + ai * br);
    }
    if (a.kind == ZC_CV_FLOAT || b.kind == ZC_CV_FLOAT) {
        double ar = (a.kind == ZC_CV_INT) ? (double)a.i : a.r;
        double br = (b.kind == ZC_CV_INT) ? (double)b.i : b.r;
        return zc_cv_float(ar * br);
    }
    if (a.w128 || b.w128) {  /* low 128 bits of the 128x128 product (Sprint 109) */
        a = zc_cv_promote128(a); b = zc_cv_promote128(b);
        uint64_t alo = (uint64_t)a.i, ahi = a.hi;
        uint64_t blo = (uint64_t)b.i, bhi = b.hi;
        uint64_t lo = alo * blo;
        /* carry of alo*blo via 32-bit limbs (portable, no __int128) */
        uint64_t p0 = (alo & 0xFFFFFFFFu) * (blo & 0xFFFFFFFFu);
        uint64_t p1 = (alo & 0xFFFFFFFFu) * (blo >> 32);
        uint64_t p2 = (alo >> 32) * (blo & 0xFFFFFFFFu);
        uint64_t mid = (p0 >> 32) + (p1 & 0xFFFFFFFFu) + (p2 & 0xFFFFFFFFu);
        uint64_t c = (p1 >> 32) + (p2 >> 32) + (mid >> 32);
        uint64_t hi = ahi * blo + alo * bhi + c;  /* ahi*bhi overflows the low 128 bits */
        ZCConstVal r = zc_cv_int((int64_t)lo);
        r.hi = hi;
        r.w128 = true;
        r.signed_ = a.signed_ && b.signed_;
        return r;
    }
    return zc_cv_int(a.i * b.i);
}

/* Unsigned 128/128 division via bitwise long division (compile-time only,
 * 128 iterations is fine). Quotient in (qlo,qhi), remainder in (rlo,rhi).
 * Sprint 109: 128-bit constant initializers. */
void zc_cv_udiv128(uint64_t alo, uint64_t ahi,
                              uint64_t blo, uint64_t bhi,
                              uint64_t *qlo, uint64_t *qhi,
                              uint64_t *rlo, uint64_t *rhi) {
    uint64_t rem_lo = 0, rem_hi = 0, ql = 0, qh = 0;
    for (int i = 0; i < 128; i++) {
        uint64_t bit = (i < 64) ? (ahi >> (63 - i)) : (alo >> (127 - i));
        rem_hi = (rem_hi << 1) | (rem_lo >> 63);
        rem_lo = (rem_lo << 1) | bit;
        if (rem_hi > bhi || (rem_hi == bhi && rem_lo >= blo)) {
            uint64_t old = rem_lo;
            rem_lo -= blo;
            if (old < blo) rem_hi--;
            if (i < 64) qh |= (1ULL << (63 - i));
            else        ql |= (1ULL << (127 - i));
        }
    }
    *qlo = ql; *qhi = qh; *rlo = rem_lo; *rhi = rem_hi;
}

ZCConstVal zc_cv_div(ZCConstVal a, ZCConstVal b) {
    if (a.kind == ZC_CV_COMPLEX || b.kind == ZC_CV_COMPLEX) {
        double ar = (a.kind == ZC_CV_INT) ? (double)a.i : a.r;
        double ai = (a.kind == ZC_CV_COMPLEX) ? a.im : 0.0;
        double br = (b.kind == ZC_CV_INT) ? (double)b.i : b.r;
        double bi = (b.kind == ZC_CV_COMPLEX) ? b.im : 0.0;
        double denom = br * br + bi * bi;
        if (denom == 0.0) return zc_cv_complex(0.0, 0.0);
        return zc_cv_complex((ar * br + ai * bi) / denom, (ai * br - ar * bi) / denom);
    }
    if (a.kind == ZC_CV_FLOAT || b.kind == ZC_CV_FLOAT) {
        double ar = (a.kind == ZC_CV_INT) ? (double)a.i : a.r;
        double br = (b.kind == ZC_CV_INT) ? (double)b.i : b.r;
        return zc_cv_float(br != 0.0 ? ar / br : 0.0);
    }
    if (a.w128 || b.w128) {  /* 128-bit div (Sprint 109) */
        a = zc_cv_promote128(a); b = zc_cv_promote128(b);
        uint64_t blo = (uint64_t)b.i, bhi = b.hi;
        if (blo == 0 && bhi == 0) {
            ZCConstVal r = zc_cv_int(0);
            r.w128 = true; r.hi = 0;
            return r;
        }
        bool aneg = zc_cv128_negative(a), bneg = zc_cv128_negative(b);
        ZCConstVal aa = aneg ? zc_cv128_neg(a) : a;
        ZCConstVal bb = bneg ? zc_cv128_neg(b) : b;
        uint64_t qlo, qhi;
        zc_cv_udiv128((uint64_t)aa.i, aa.hi, (uint64_t)bb.i, bb.hi, &qlo, &qhi, NULL, NULL);
        ZCConstVal r = zc_cv_int((int64_t)qlo);
        r.hi = qhi;
        r.w128 = true;
        if (aneg != bneg) r = zc_cv128_neg(r);
        r.signed_ = a.signed_ && b.signed_;
        return r;
    }
    return zc_cv_int(b.i != 0 ? a.i / b.i : 0);
}

ZCConstVal zc_cv_mod(ZCConstVal a, ZCConstVal b) {
    if (a.kind == ZC_CV_FLOAT || b.kind == ZC_CV_FLOAT) {
        /* Mirror the historical int-based float mod path. */
        return zc_cv_int(b.i != 0 ? a.i % b.i : 0);
    }
    if (a.w128 || b.w128) {  /* 128-bit mod, C99 truncation semantics (Sprint 109) */
        a = zc_cv_promote128(a); b = zc_cv_promote128(b);
        uint64_t blo = (uint64_t)b.i, bhi = b.hi;
        if (blo == 0 && bhi == 0) {
            ZCConstVal r = zc_cv_int(0);
            r.w128 = true; r.hi = 0;
            return r;
        }
        bool aneg = zc_cv128_negative(a), bneg = zc_cv128_negative(b);
        ZCConstVal aa = aneg ? zc_cv128_neg(a) : a;
        ZCConstVal bb = bneg ? zc_cv128_neg(b) : b;
        uint64_t qlo, qhi, rlo, rhi;
        zc_cv_udiv128((uint64_t)aa.i, aa.hi, (uint64_t)bb.i, bb.hi, &qlo, &qhi, &rlo, &rhi);
        ZCConstVal r = zc_cv_int((int64_t)rlo);
        r.hi = rhi;
        r.w128 = true;
        if (aneg) r = zc_cv128_neg(r);  /* sign follows the dividend */
        r.signed_ = a.signed_ && b.signed_;
        return r;
    }
    return zc_cv_int(b.i != 0 ? a.i % b.i : 0);
}

ZCConstVal zc_cv_eq(ZCConstVal a, ZCConstVal b) {
    if (a.kind == ZC_CV_COMPLEX || b.kind == ZC_CV_COMPLEX) {
        double ar = (a.kind == ZC_CV_INT) ? (double)a.i : a.r;
        double ai = (a.kind == ZC_CV_COMPLEX) ? a.im : 0.0;
        double br = (b.kind == ZC_CV_INT) ? (double)b.i : b.r;
        double bi = (b.kind == ZC_CV_COMPLEX) ? b.im : 0.0;
        return zc_cv_int((ar == br && ai == bi) ? 1 : 0);
    }
    if (a.kind == ZC_CV_FLOAT || b.kind == ZC_CV_FLOAT) {
        double ar = (a.kind == ZC_CV_INT) ? (double)a.i : a.r;
        double br = (b.kind == ZC_CV_INT) ? (double)b.i : b.r;
        return zc_cv_int(ar == br ? 1 : 0);
    }
    if (a.w128 || b.w128) {  /* 128-bit (Sprint 109) */
        a = zc_cv_promote128(a); b = zc_cv_promote128(b);
        return zc_cv_int((a.i == b.i && a.hi == b.hi) ? 1 : 0);
    }
    return zc_cv_int(a.i == b.i ? 1 : 0);
}

ZCConstVal zc_cv_ne(ZCConstVal a, ZCConstVal b) {
    ZCConstVal eq = zc_cv_eq(a, b);
    return zc_cv_int(eq.i ? 0 : 1);
}

ZCConstVal zc_cv_cmp(ZeroCCTokType op, ZCConstVal a, ZCConstVal b) {
    if (a.kind == ZC_CV_FLOAT || b.kind == ZC_CV_FLOAT || a.kind == ZC_CV_COMPLEX || b.kind == ZC_CV_COMPLEX) {
        double ar = (a.kind == ZC_CV_INT) ? (double)a.i : a.r;
        double br = (b.kind == ZC_CV_INT) ? (double)b.i : b.r;
        int res = 0;
        if (op == TK_LT) res = (ar < br);
        else if (op == TK_GT) res = (ar > br);
        else if (op == TK_LE) res = (ar <= br);
        else if (op == TK_GE) res = (ar >= br);
        return zc_cv_int(res);
    }
    if (a.w128 || b.w128) {  /* 128-bit relational (Sprint 109) */
        a = zc_cv_promote128(a); b = zc_cv_promote128(b);
        bool aneg = zc_cv128_negative(a), bneg = zc_cv128_negative(b);
        int res = 0;
        if (aneg != bneg) {
            /* Opposite signs: order follows the sign. */
            int cmp = aneg ? -1 : 1;
            if (op == TK_LT) res = (cmp < 0);
            else if (op == TK_GT) res = (cmp > 0);
            else if (op == TK_LE) res = (cmp <= 0);
            else if (op == TK_GE) res = (cmp >= 0);
        } else {
            /* Same sign (or unsigned): two's-complement order == unsigned order. */
            int cmp = (a.hi < b.hi) ? -1 : (a.hi > b.hi) ? 1 : (a.i < b.i ? -1 : (a.i > b.i ? 1 : 0));
            if (op == TK_LT) res = (cmp < 0);
            else if (op == TK_GT) res = (cmp > 0);
            else if (op == TK_LE) res = (cmp <= 0);
            else if (op == TK_GE) res = (cmp >= 0);
        }
        return zc_cv_int(res);
    }
    int64_t left = a.i, right = b.i;
    int res = 0;
    if (op == TK_LT) res = (left < right);
    else if (op == TK_GT) res = (left > right);
    else if (op == TK_LE) res = (left <= right);
    else if (op == TK_GE) res = (left >= right);
    return zc_cv_int(res);
}

/* Forward declarations for recursive functions */
ZCConstVal parse_conditional(ZeroCCCompiler *c, size_t *pos);
ZCConstVal parse_logical_or(ZeroCCCompiler *c, size_t *pos);
ZCConstVal parse_logical_and(ZeroCCCompiler *c, size_t *pos);
ZCConstVal parse_bitor(ZeroCCCompiler *c, size_t *pos);
ZCConstVal parse_bitxor(ZeroCCCompiler *c, size_t *pos);
ZCConstVal parse_bitand(ZeroCCCompiler *c, size_t *pos);
ZCConstVal parse_equality(ZeroCCCompiler *c, size_t *pos);
ZCConstVal parse_relational(ZeroCCCompiler *c, size_t *pos);
ZCConstVal parse_shift(ZeroCCCompiler *c, size_t *pos);
ZCConstVal parse_additive(ZeroCCCompiler *c, size_t *pos);
ZCConstVal parse_multiplicative(ZeroCCCompiler *c, size_t *pos);
ZCConstVal parse_unary(ZeroCCCompiler *c, size_t *pos);
ZCConstVal parse_primary(ZeroCCCompiler *c, size_t *pos);



int64_t eval_sizeof(ZeroCCCompiler *c, size_t *pos);
int64_t eval_alignof(ZeroCCCompiler *c, size_t *pos);

int64_t zero_cc_eval_const_expr(ZeroCCCompiler *c);

/* Parse a `__zc_vector_size__( <const-expr> )` marker - emitted by the
 * preprocessor in place of `__attribute__((__vector_size__(N)))` (see the
 * __attribute__ special case in core/preprocessor.h). Operates on the
 * caller's token position *pos (either &c->token_pos or a local). On
 * success consumes the marker and returns 1 with *out_size = N (bytes);
 * returns 0 (and leaves *pos untouched) when the current token is not the
 * marker. Called both BEFORE the base type specifier (the attribute may
 * lead: `__attribute__((__vector_size__(16))) short`) and AFTER it
 * (`signed char __attribute__((__vector_size__(16))) V`). N is evaluated
 * as a C constant expression (so `2 * sizeof(long long)` works); N must
 * be positive and <= 512 bytes. */
int zero_cc_parse_vector_attr(ZeroCCCompiler *c, size_t *pos, size_t *out_size) {
    if (!c || !out_size || !pos || *pos >= c->token_count) return 0;
    ZeroCCToken *t = &c->tokens[*pos];
    const char *id = t->type == TK_IDENT ? (t->allocated_str ? t->allocated_str : t->str_val) : NULL;
    if (!id || strcmp(id, "__zc_vector_size__") != 0) return 0;
    if (*pos + 1 >= c->token_count || c->tokens[*pos + 1].type != TK_LPAREN) return 0;
    size_t save = *pos;
    size_t save_tp = c->token_pos;
    c->token_pos = save + 2; /* consume ident + '(' */
    int64_t n = zero_cc_eval_const_expr(c);
    size_t after = c->token_pos;
    c->token_pos = save_tp; /* restore: advance *pos on success below */
    if (after >= c->token_count || c->tokens[after].type != TK_RPAREN) {
        *pos = save;
        return 0;
    }
    *pos = after + 1; /* consume ')' */
    if (n <= 0 || n > 512) {
        *pos = save;
        return 0;
    }
    *out_size = (size_t)n;
    return 1;
}

/* Parse a `__zc_aligned__( <const-expr> )` marker - emitted by the
 * preprocessor in place of `__attribute__((aligned(N)))` / `__aligned__(N)`
 * (see the __attribute__ special case in core/preprocessor.h). Same
 * shape as zero_cc_parse_vector_attr: on success consumes the marker and
 * returns 1 with *out_align = N (bytes); returns 0 (leaving *pos
 * untouched) when the current token is not the marker. N must be positive
 * and <= 4096 bytes. */
int zero_cc_parse_aligned_attr(ZeroCCCompiler *c, size_t *pos, size_t *out_align) {
    if (!c || !out_align || !pos || *pos >= c->token_count) return 0;
    ZeroCCToken *t = &c->tokens[*pos];
    /* C11 `_Alignas(N)` is a real token; `__attribute__((aligned(N)))` is
     * rewritten to the `__zc_aligned__(N)` marker by the preprocessor. */
    if (t->type != TK_ALIGNAS) {
        const char *id = t->type == TK_IDENT ? (t->allocated_str ? t->allocated_str : t->str_val) : NULL;
        if (!id || (strcmp(id, "__zc_aligned__") != 0 && strcmp(id, "_Alignas") != 0)) return 0;
    }
    if (*pos + 1 >= c->token_count || c->tokens[*pos + 1].type != TK_LPAREN) return 0;
    size_t save = *pos;
    size_t save_tp = c->token_pos;
    c->token_pos = save + 2; /* consume ident + '(' */
    int64_t n = zero_cc_eval_const_expr(c);
    size_t after = c->token_pos;
    c->token_pos = save_tp; /* restore: advance *pos on success below */
    if (after >= c->token_count || c->tokens[after].type != TK_RPAREN) {
        *pos = save;
        return 0;
    }
    *pos = after + 1; /* consume ')' */
    if (n <= 0 || n > 4096) {
        *pos = save;
        return 0;
    }
    *out_align = (size_t)n;
    return 1;
}

/* Parse a `__zc_mode__(QI|HI|SI|DI|TI)` marker - emitted by the
 * preprocessor in place of `__attribute__((mode(X)))` / `__mode__(X)`
 * (GNU integer-mode attribute, c-torture execute/memclr).  Same shape as
 * zero_cc_parse_aligned_attr: on success consumes the marker and returns 1
 * with *out_bytes = 1/2/4/8 (QI/HI/SI/DI); TI (16 bytes) has no target type
 * yet and is left unparsed (returns 0). Returns 0 (leaving *pos untouched)
 * when the current token is not the marker. */
int zero_cc_parse_mode_attr(ZeroCCCompiler *c, size_t *pos, size_t *out_bytes) {
    if (!c || !out_bytes || !pos || *pos >= c->token_count) return 0;
    ZeroCCToken *t = &c->tokens[*pos];
    const char *id = t->type == TK_IDENT ? (t->allocated_str ? t->allocated_str : t->str_val) : NULL;
    if (!id || strcmp(id, "__zc_mode__") != 0) return 0;
    if (*pos + 1 >= c->token_count || c->tokens[*pos + 1].type != TK_LPAREN) return 0;
    /* Consume the whole `__zc_mode__(...)` group. Recognized mode names map
     * to a byte count; anything else (byte, word, ...) still consumes the
     * marker (it must never leak into the token stream) but reports 0, so
     * callers leave the type unchanged. */
    size_t close = *pos + 2;
    while (close < c->token_count && c->tokens[close].type != TK_RPAREN) close++;
    if (close >= c->token_count) return 0;
    size_t bytes = 0;
    if (close == *pos + 3) { /* single-token argument at *pos + 2 */
        ZeroCCToken *a = &c->tokens[close - 1];
        const char *mk = a->type == TK_IDENT ? (a->allocated_str ? a->allocated_str : a->str_val) : NULL;
        size_t ml = mk ? strlen(mk) : 0;
        /* Mode names appear as `QI`, `__QI__` or `QImode` (GCC accepts all
         * three); the width is the element count in bytes. */
#define ZC_MODE_NAME(mk, ml, N) \
    (((ml) == 2 && strncmp((mk), N, 2) == 0) || \
     ((ml) == 6 && strncmp((mk), "__" N "__", 6) == 0) || \
     ((ml) == 5 && strncmp((mk), N "mode", 5) == 0))
        if (mk && ZC_MODE_NAME(mk, ml, "QI")) bytes = 1;
        else if (mk && ZC_MODE_NAME(mk, ml, "HI")) bytes = 2;
        else if (mk && ZC_MODE_NAME(mk, ml, "SI")) bytes = 4;
        else if (mk && ZC_MODE_NAME(mk, ml, "DI")) bytes = 8;
#undef ZC_MODE_NAME
    }
    *out_bytes = bytes;
    *pos = close + 1;
    return 1;
}

/* Silently consume any trailing `__zc_mode__(...)` markers in contexts
 * where the mode attribute is not applied (struct members, after a
 * typedef alias name, function parameters). Before mode() support the
 * attribute was stripped by the preprocessor entirely, so skipping
 * preserves that behavior instead of leaking the marker into the
 * declarator (parse error). */
void zero_cc_skip_mode_attr(ZeroCCCompiler *c) {
    size_t probe = 0;
    while (zero_cc_parse_mode_attr(c, &c->token_pos, &probe)) { /* drain */ }
}

/* Parse a `__zc_alias__( "target" )` marker - emitted by the preprocessor
 * in place of `__attribute__((alias("target")))` (GNU symbol aliasing).
 * On success consumes the marker and returns 1 with *out_target pointing
 * at the target name (a token string valid for the compilation); returns 0
 * (leaving *pos untouched) when the current token is not the marker. */
int zero_cc_parse_alias_attr(ZeroCCCompiler *c, size_t *pos, const char **out_target) {
    if (!c || !out_target || !pos || *pos >= c->token_count) return 0;
    ZeroCCToken *t = &c->tokens[*pos];
    const char *id = t->type == TK_IDENT ? (t->allocated_str ? t->allocated_str : t->str_val) : NULL;
    if (!id || strcmp(id, "__zc_alias__") != 0) return 0;
    if (*pos + 1 >= c->token_count || c->tokens[*pos + 1].type != TK_LPAREN) return 0;
    if (*pos + 2 >= c->token_count || c->tokens[*pos + 2].type != TK_STRING) return 0;
    if (*pos + 3 >= c->token_count || c->tokens[*pos + 3].type != TK_RPAREN) return 0;
    const ZeroCCToken *st = &c->tokens[*pos + 2];
    *out_target = st->allocated_str ? st->allocated_str : st->str_val;
    *pos = *pos + 4;
    return 1;
}

bool zero_cc_tok_is_type_start(ZeroCCCompiler *c, size_t pos) {
    ZeroCCTokType t = c->tokens[pos].type;
    if (t == TK_INT || t == TK_CHAR || t == TK_VOID || t == TK_STRUCT ||
        t == TK_UNION || t == TK_ENUM || t == TK_FLOAT_KW || t == TK_DOUBLE_KW ||
        t == TK_BOOL_KW || t == TK_SIGNED || t == TK_UNSIGNED ||
        t == TK_SHORT || t == TK_LONG || t == TK_ATOMIC ||
        t == TK_CONST || t == TK_VOLATILE || t == TK_RESTRICT ||
        t == TK_TYPEOF || t == TK_TYPEOF_UNQUAL) {
        return true;
    }
    if (t == TK_IDENT) {
        const char *name = c->tokens[pos].str_val;
        return zero_cc_find_typedef(c, name) != NULL ||
               zero_cc_tok_is_complex_spec(name) ||
               strcmp(name, "__int128") == 0 ||
               strcmp(name, "__int128_t") == 0 ||
               strcmp(name, "__uint128_t") == 0 ||
               strcmp(name, "__float128") == 0;
    }
    return false;
}

/* Size/alignment of a plain-identifier operand for sizeof(expr)/_Alignof(expr):
 * resolve against local symbols first, then the global variable table.
 * An optional [index] suffix selects the array element size.
 * Returns 0 when the operand's type cannot be determined at parse time.
 * On success the operand (incl. any [..] suffix) is consumed from *pos.
 * Runtime VLA operand: returns SIZE_MAX and reports the VLA param/local's
 * c->locals index in *out_vla_local plus the product of the CONSTANT
 * dimensions stripped by [..] suffixes in *out_vla_strip (the remaining
 * VLA size lives in the runtime slot, loaded via NODE_VLA_SLOT). Pass
 * NULL/NULL for *out* in constant-expression contexts. */
size_t zero_cc_sizeof_ident_size(ZeroCCCompiler *c, size_t *pos,
                                               int *out_vla_local, unsigned *out_vla_strip) {
    if (c->tokens[*pos].type != TK_IDENT) return 0;
    const char *name = c->tokens[*pos].str_val;
    ZeroCCType ty;
    bool found = false;
    ZeroCCSym *ls = zero_cc_find_local(c, name);
    if (ls && ls->type.size > 0) { ty = ls->type; found = true; }
    if (!found) {
        void *tls = zero_cc_find_tls(c, name);
        if (tls) {
            for (size_t i = 0; i < c->thread_local_count; i++) {
                if (strcmp(c->thread_locals[i].name, name) == 0) {
                    ty = c->thread_locals[i].type;
                    found = true;
                    break;
                }
            }
        }
    }
    if (!found) {
        ZeroCCGlobalVar *gv = find_global_var(c, name);
        if (gv && gv->type.size > 0) { ty = gv->type; found = true; }
    }
    if (!found) {
        /* Function name (not a call): decays to a function pointer
         * (C99 6.3.2.1) - sizeof of the name is the pointer size.  An extern
         * OBJECT registered in the function table (is_object) holds the
         * variable's type in return_type: use it so `sizeof extern_arr[i]`
         * computes the element size instead of a dangling 8 (c-torture
         * split-path-6). */
        bool fn_call = (*pos + 1 < c->token_count &&
                        c->tokens[*pos + 1].type == TK_LPAREN);
        ZeroCCFunction *fn = fn_call ? NULL : zero_cc_find_function(c, name);
        if (fn) {
            if (fn->is_object) { ty = fn->return_type; found = true; }
            else { (*pos)++; return 8; }
        }
    }
    if (!found) return 0;
    /* Only a plain `ident` or `ident[...]` operand is handled here.  A
     * member access (`->`/`.`), call, or inc/dec suffix means the operand
     * is a full postfix expression: consume only the identifier here and
     * the rest dangles, stalling the parser (and yielding the pointer's
     * size instead of the member's).  Fall through to the expression-type
     * walker (zero_cc_type_of_equality), which consumes the whole chain. */
    if (*pos + 1 < c->token_count) {
        ZeroCCTokType after = c->tokens[*pos + 1].type;
        if (after == TK_DOT || after == TK_ARROW || after == TK_LPAREN ||
            after == TK_INC || after == TK_DEC) {
            return 0;
        }
    }
    (*pos)++; /* consume the identifier */
    /* Chained subscripts (a[i], a[i][j], p[i]): each `[...]` reduces the
     * type to its element/pointee, so `sizeof a[i][j]` yields the innermost
     * element size (c-torture split-path-6 uses a single subscript).
     * When the peeled array still contains a VLA dim deeper down, the
     * peeled dim is a CONSTANT factor dividing the runtime slot size. */
    unsigned strip = 1;
    while (c->tokens[*pos].type == TK_LBRACKET) {
        /* a[expr] -> element size */
        size_t q = *pos;
        q++; /* [ */
        int depth = 1;
        while (q < c->token_count && depth > 0) {
            if (c->tokens[q].type == TK_LBRACKET) depth++;
            else if (c->tokens[q].type == TK_RBRACKET) depth--;
            q++;
        }
        *pos = q; /* past ] */
        if (ty.tag == TYPE_ARRAY && ty.base) {
            if (zc_type_chain_has_vla(*ty.base) &&
                ty.base->size > 0 && ty.size > 0 && (ty.size % ty.base->size) == 0) {
                strip *= (unsigned)(ty.size / ty.base->size);
            }
            ty = *ty.base;
        } else if (ty.tag == TYPE_PTR && ty.base) {
            ty = *ty.base;
        }
    }
    if (out_vla_local && ls && zc_type_chain_has_vla(ty)) {
        *out_vla_local = (int)(ls - c->locals);
        if (out_vla_strip) *out_vla_strip = strip;
        return SIZE_MAX;
    }
    return ty.size;
}

size_t zero_cc_sizeof_ident_align(ZeroCCCompiler *c, size_t *pos) {
    if (c->tokens[*pos].type != TK_IDENT) return 0;
    const char *name = c->tokens[*pos].str_val;
    ZeroCCType ty;
    bool found = false;
    ZeroCCSym *ls = zero_cc_find_local(c, name);
    if (ls && ls->type.align > 0) { ty = ls->type; found = true; }
    if (!found) {
        void *tls = zero_cc_find_tls(c, name);
        if (tls) {
            for (size_t i = 0; i < c->thread_local_count; i++) {
                if (strcmp(c->thread_locals[i].name, name) == 0) {
                    ty = c->thread_locals[i].type;
                    found = true;
                    break;
                }
            }
        }
    }
    if (!found) {
        ZeroCCGlobalVar *gv = find_global_var(c, name);
        if (gv && gv->type.align > 0) { ty = gv->type; found = true; }
    }
    if (!found) {
        /* Function name (not a call): pointer alignment. Extern objects
         * (is_object) hold the variable type in return_type. */
        bool fn_call = (*pos + 1 < c->token_count &&
                        c->tokens[*pos + 1].type == TK_LPAREN);
        ZeroCCFunction *fn = fn_call ? NULL : zero_cc_find_function(c, name);
        if (fn) {
            if (fn->is_object) { ty = fn->return_type; found = true; }
            else { (*pos)++; return fn->align > 0 ? fn->align : 8; }
        }
    }
    if (!found) return 0;
    /* See zero_cc_sizeof_ident_size: postfix continuations after the
     * identifier must be handled by the expression-type walker. */
    if (*pos + 1 < c->token_count) {
        ZeroCCTokType after = c->tokens[*pos + 1].type;
        if (after == TK_DOT || after == TK_ARROW || after == TK_LPAREN ||
            after == TK_INC || after == TK_DEC) {
            return 0;
        }
    }
    (*pos)++; /* consume the identifier */
    if (c->tokens[*pos].type == TK_LBRACKET) {
        size_t q = *pos;
        q++;
        int depth = 1;
        while (q < c->token_count && depth > 0) {
            if (c->tokens[q].type == TK_LBRACKET) depth++;
            else if (c->tokens[q].type == TK_RBRACKET) depth--;
            q++;
        }
        *pos = q;
    }
    return ty.align ? ty.align : 1;
}
int64_t eval_sizeof(ZeroCCCompiler *c, size_t *pos)
{
    (*pos)++; /* consume TK_SIZEOF */
    bool has_paren = false;
    if (c->tokens[*pos].type == TK_LPAREN) {
        has_paren = true;
        (*pos)++; /* consume '(' */
    }
    ZeroCCType ty;
    /* `sizeof(type)` / `sizeof(type *)` / `sizeof(t arr[])`: a type name
     * (including pointer/array suffixes) needs the type parser; the
     * expression grammar treats `*` as multiplication and fails. */
    if (zero_cc_tok_is_type_start(c, *pos)) {
        ty = zero_cc_parse_type_name_at_pos(c, pos);
    } else {
        size_t id_sz = zero_cc_sizeof_ident_size(c, pos, NULL, NULL);
        if (id_sz == SIZE_MAX) {
            /* Runtime VLA size in a constant-expression context: invalid C
             * (the dimension is not a constant); report 0 and move on. */
            if (has_paren && c->tokens[*pos].type == TK_RPAREN) (*pos)++;
            return 0;
        }
        if (id_sz > 0) {
            if (has_paren && c->tokens[*pos].type == TK_RPAREN) (*pos)++;
            return (int64_t)id_sz;
        }
        ty = zero_cc_type_of_equality(c, pos);
    }
    /* `sizeof (a, b)` - comma operator inside the parens: the operand's
     * type is that of the LAST expression (C99 6.5.17). */
    if (has_paren) {
        while (*pos < c->token_count && c->tokens[*pos].type == TK_COMMA) {
            (*pos)++;
            ty = zero_cc_type_of_equality(c, pos);
        }
    }
    /* Compound literal operand: `sizeof (struct s){ 1, 2 }` - the closing
     * paren (if any) precedes the brace group; value is the type size. */
    bool sz_paren_done = false;
    {
        size_t probe = *pos;
        if (has_paren && probe < c->token_count &&
            c->tokens[probe].type == TK_RPAREN) {
            probe++;
        }
        while (probe < c->token_count && c->tokens[probe].type == TK_STAR) probe++;
        if (probe < c->token_count && c->tokens[probe].type == TK_LBRACE) {
            *pos = probe;
            int bd = 0;
            while (*pos < c->token_count) {
                if (c->tokens[*pos].type == TK_LBRACE) bd++;
                else if (c->tokens[*pos].type == TK_RBRACE) {
                    bd--;
                    if (bd == 0) { (*pos)++; break; }
                }
                (*pos)++;
            }
            sz_paren_done = has_paren;
        }
    }
    if (has_paren && !sz_paren_done && c->tokens[*pos].type == TK_RPAREN) {
        (*pos)++;
    }
    return ty.size;
}

int64_t eval_alignof(ZeroCCCompiler *c, size_t *pos)
{
    (*pos)++; /* consume TK_ALIGNOF */
    bool has_paren = false;
    if (c->tokens[*pos].type == TK_LPAREN) {
        has_paren = true;
        (*pos)++; /* consume '(' */
    }
    ZeroCCType ty;
    if (zero_cc_tok_is_type_start(c, *pos)) {
        ty = zero_cc_parse_type_name_at_pos(c, pos);
    } else {
        size_t id_al = zero_cc_sizeof_ident_align(c, pos);
        if (id_al > 0) {
            if (has_paren && c->tokens[*pos].type == TK_RPAREN) (*pos)++;
            return (int64_t)id_al;
        }
        ty = zero_cc_type_of_equality(c, pos);
    }
    if (has_paren && c->tokens[*pos].type == TK_RPAREN) {
        (*pos)++;
    }
    return ty.align ? ty.align : 1;
}

ZCConstVal parse_conditional(ZeroCCCompiler *c, size_t *pos)
{
    ZCConstVal left = parse_logical_or(c, pos);
    if (*pos < c->token_count && c->tokens[*pos].type == TK_QUESTION) {
        (*pos)++;
        ZCConstVal t = parse_conditional(c, pos);
        if (*pos < c->token_count && c->tokens[*pos].type == TK_COLON) (*pos)++;
        ZCConstVal f = parse_conditional(c, pos);
        left = zc_cv_is_true(left) ? t : f;
    }
    return left;
}

ZCConstVal parse_logical_or(ZeroCCCompiler *c, size_t *pos)
{
    ZCConstVal left = parse_logical_and(c, pos);
    while (*pos < c->token_count && c->tokens[*pos].type == TK_LOGOR) {
        (*pos)++;
        ZCConstVal right = parse_logical_and(c, pos);
        left = zc_cv_int(zc_cv_is_true(left) || zc_cv_is_true(right));
    }
    return left;
}

ZCConstVal parse_logical_and(ZeroCCCompiler *c, size_t *pos)
{
    ZCConstVal left = parse_bitor(c, pos);
    while (*pos < c->token_count && c->tokens[*pos].type == TK_LOGAND) {
        (*pos)++;
        ZCConstVal right = parse_bitor(c, pos);
        left = zc_cv_int(zc_cv_is_true(left) && zc_cv_is_true(right));
    }
    return left;
}

ZCConstVal parse_bitor(ZeroCCCompiler *c, size_t *pos)
{
    ZCConstVal left = parse_bitxor(c, pos);
    while (*pos < c->token_count && c->tokens[*pos].type == TK_PIPE) {
        (*pos)++;
        ZCConstVal right = parse_bitxor(c, pos);
        if (left.w128 || right.w128) {  /* 128-bit (Sprint 109) */
            left = zc_cv_promote128(left);
            right = zc_cv_promote128(right);
            left.i = left.i | right.i;
            left.hi = left.hi | right.hi;
            left.signed_ = left.signed_ && right.signed_;
        } else {
            left = zc_cv_int(left.i | right.i);
        }
    }
    return left;
}

ZCConstVal parse_bitxor(ZeroCCCompiler *c, size_t *pos)
{
    ZCConstVal left = parse_bitand(c, pos);
    while (*pos < c->token_count && c->tokens[*pos].type == TK_CARET) {
        (*pos)++;
        ZCConstVal right = parse_bitand(c, pos);
        if (left.w128 || right.w128) {  /* 128-bit (Sprint 109) */
            left = zc_cv_promote128(left);
            right = zc_cv_promote128(right);
            left.i = left.i ^ right.i;
            left.hi = left.hi ^ right.hi;
            left.signed_ = left.signed_ && right.signed_;
        } else {
            left = zc_cv_int(left.i ^ right.i);
        }
    }
    return left;
}

ZCConstVal parse_bitand(ZeroCCCompiler *c, size_t *pos)
{
    ZCConstVal left = parse_equality(c, pos);
    while (*pos < c->token_count && c->tokens[*pos].type == TK_AMP) {
        (*pos)++;
        ZCConstVal right = parse_equality(c, pos);
        if (left.w128 || right.w128) {  /* 128-bit (Sprint 109) */
            left = zc_cv_promote128(left);
            right = zc_cv_promote128(right);
            left.i = left.i & right.i;
            left.hi = left.hi & right.hi;
            left.signed_ = left.signed_ && right.signed_;
        } else {
            left = zc_cv_int(left.i & right.i);
        }
    }
    return left;
}

ZCConstVal parse_equality(ZeroCCCompiler *c, size_t *pos)
{
    ZCConstVal left = parse_relational(c, pos);
    while (*pos < c->token_count) {
        ZeroCCTokType op = c->tokens[*pos].type;
        if (op == TK_EQ || op == TK_NE) {
            (*pos)++;
            ZCConstVal right = parse_relational(c, pos);
            if (op == TK_EQ)
                left = zc_cv_eq(left, right);
            else
                left = zc_cv_ne(left, right);
        } else {
            break;
        }
    }
    return left;
}

ZCConstVal parse_relational(ZeroCCCompiler *c, size_t *pos)
{
    ZCConstVal left = parse_shift(c, pos);
    while (true) {
        ZeroCCTokType op = c->tokens[*pos].type;
        if (op == TK_LT || op == TK_GT || op == TK_LE || op == TK_GE) {
            (*pos)++;
            ZCConstVal right = parse_shift(c, pos);
            left = zc_cv_cmp(op, left, right);
        } else {
            break;
        }
    }
    return left;
}

ZCConstVal parse_shift(ZeroCCCompiler *c, size_t *pos)
{
    size_t left_begin = *pos;
    ZCConstVal left = parse_additive(c, pos);
    while (true) {
        ZeroCCTokType op = c->tokens[*pos].type;
        if (op == TK_SHL || op == TK_SHR) {
            (*pos)++;
            ZCConstVal right = parse_additive(c, pos);
            unsigned n = (unsigned)right.i;
            if (left.w128 && left.kind == ZC_CV_INT) {
                /* 128-bit shift: counts up to 127 are meaningful (Sprint 109) */
                uint64_t lo = (uint64_t)left.i, hi = left.hi;
                if (n >= 128) { lo = 0; hi = 0; }
                else if (op == TK_SHL) {
                    if (n >= 64) { hi = lo << (n - 64); lo = 0; }
                    else { hi = (hi << n) | (lo >> (64 - n)); lo <<= n; }
                } else {
                    bool neg = left.signed_ && (hi >> 63) != 0;
                    uint64_t fill = neg ? ~0ULL : 0;
                    if (n >= 64) { lo = hi >> (n - 64); hi = fill; }
                    else { lo = (lo >> n) | (hi << (64 - n)); hi = (hi >> n) | (neg ? ~0ULL : 0); }
                }
                ZCConstVal r = zc_cv_int((int64_t)lo);
                r.hi = hi;
                r.w128 = true;
                r.signed_ = left.signed_;
                left = r;
            } else {
                unsigned long ul = (unsigned long)left.i;
                unsigned long ur = (unsigned long)right.i;
                if (ur >= 64) ur = 0; /* avoid UB; C99 leaves it undefined anyway */
                if (op == TK_SHL) {
                    left = zc_cv_int((int64_t)(ul << ur));
                } else {
                    /* C99 6.5.7: >> on a SIGNED operand is an arithmetic (sign-
                     * extending) shift; on unsigned it is logical. The evaluator
                     * has no operand types, so use the literal's suffix when the
                     * left operand is a bare integer literal (bit0 of
                     * int_suffix = U) and default to arithmetic - the signed
                     * case, matching GCC for `0x...LL >> n` initializers. */
                    bool l_unsigned = (c->tokens[left_begin].type == TK_INT) &&
                        (c->tokens[left_begin].int_suffix & 0x1);
                    if (l_unsigned) {
                        left = zc_cv_int((int64_t)(ul >> ur));
                    } else {
                        left = zc_cv_int((int64_t)left.i >> ur);
                    }
                }
            }
        } else {
            break;
        }
    }
    return left;
}

ZCConstVal parse_additive(ZeroCCCompiler *c, size_t *pos)
{
    ZCConstVal left = parse_multiplicative(c, pos);
    while (true) {
        ZeroCCTokType op = c->tokens[*pos].type;
        if (op == TK_PLUS || op == TK_MINUS) {
            (*pos)++;
            ZCConstVal right = parse_multiplicative(c, pos);
            if (op == TK_PLUS)
                left = zc_cv_add(left, right);
            else
                left = zc_cv_sub(left, right);
        } else {
            break;
        }
    }
    return left;
}

ZCConstVal parse_multiplicative(ZeroCCCompiler *c, size_t *pos)
{
    ZCConstVal left = parse_unary(c, pos);
    while (true) {
        ZeroCCTokType op = c->tokens[*pos].type;
        if (op == TK_STAR || op == TK_SLASH || op == TK_PERCENT) {
            (*pos)++;
            ZCConstVal right = parse_unary(c, pos);
            if (op == TK_STAR)
                left = zc_cv_mul(left, right);
            else if (op == TK_SLASH)
                left = zc_cv_div(left, right);
            else
                left = zc_cv_mod(left, right);
        } else {
            break;
        }
    }
    return left;
}

ZCConstVal parse_unary(ZeroCCCompiler *c, size_t *pos)
{
    ZeroCCTokType t = c->tokens[*pos].type;
    if (t == TK_PLUS) {
        (*pos)++;
        return parse_unary(c, pos);
    }
    if (t == TK_MINUS) {
        (*pos)++;
        ZCConstVal v = parse_unary(c, pos);
        if (v.kind == ZC_CV_COMPLEX) return zc_cv_complex(-v.r, -v.im);
        if (v.kind == ZC_CV_FLOAT) return zc_cv_float(-v.r);
        return zc_cv128_neg(v);
    }
    if (t == TK_TILDE) {
        (*pos)++;
        ZCConstVal v = parse_unary(c, pos);
        if (v.kind == ZC_CV_INT && v.w128) {  /* 128-bit (Sprint 109) */
            v.i = ~v.i;
            v.hi = ~v.hi;
            return v;
        }
        return zc_cv_int(~v.i);
    }
    if (t == TK_BANG) {
        (*pos)++;
        return zc_cv_int(!zc_cv_is_true(parse_unary(c, pos)));
    }
    if (t == TK_SIZEOF) {
        return zc_cv_int(eval_sizeof(c, pos));
    }
    if (t == TK_ALIGNOF) {
        return zc_cv_int(eval_alignof(c, pos));
    }
    if (t == TK_AMP) {
        size_t save = *pos;
        (*pos)++; /* consume '&' */
        /* Check for &((Type *)0)->member offsetof pattern */
        int open_parens = 0;
        while (*pos < c->token_count && c->tokens[*pos].type == TK_LPAREN) {
            if (*pos + 1 < c->token_count && zero_cc_tok_is_type_start(c, *pos + 1)) {
                break; /* start of type cast */
            }
            open_parens++;
            (*pos)++;
        }
        if (*pos < c->token_count && c->tokens[*pos].type == TK_LPAREN &&
            *pos + 1 < c->token_count && zero_cc_tok_is_type_start(c, *pos + 1)) {
            (*pos)++; /* consume '(' */
            ZeroCCType ty = zero_cc_parse_type_name_at_pos(c, pos);
            if (*pos < c->token_count && c->tokens[*pos].type == TK_RPAREN) (*pos)++;
            int64_t base_val = parse_unary(c, pos).i;
            if (open_parens > 0 && *pos < c->token_count && c->tokens[*pos].type == TK_RPAREN) {
                (*pos)++;
                open_parens--;
            }
            if (ty.tag == TYPE_PTR && ty.base) ty = *ty.base;
            int64_t total_offset = 0;
            while (*pos < c->token_count) {
                if (c->tokens[*pos].type == TK_ARROW || c->tokens[*pos].type == TK_DOT) {
                    (*pos)++;
                    if (*pos < c->token_count && c->tokens[*pos].type == TK_IDENT) {
                        const char *mname = c->tokens[*pos].str_val;
                        (*pos)++;
                        bool found = false;
                        if (ty.tag == TYPE_STRUCT || ty.tag == TYPE_UNION) {
                            for (size_t m = 0; m < ty.member_count; m++) {
                                if (strcmp(ty.members[m].name, mname) == 0) {
                                    total_offset += (int64_t)ty.members[m].offset;
                                    if (ty.members[m].base && (ty.members[m].tag == TYPE_STRUCT || ty.members[m].tag == TYPE_UNION)) {
                                        ty = *ty.members[m].base;
                                    } else if (ty.members[m].base && ty.members[m].tag == TYPE_ARRAY) {
                                        ty.base = ty.members[m].base;
                                        ty.tag = TYPE_ARRAY;
                                        ty.size = ty.members[m].size;
                                    } else {
                                        ty.tag = ty.members[m].tag;
                                        ty.size = ty.members[m].size;
                                        ty.base = ty.members[m].base;
                                        ty.member_count = 0;
                                    }
                                    found = true;
                                    break;
                                }
                            }
                        }
                        if (!found) break;
                    } else {
                        break;
                    }
                } else if (c->tokens[*pos].type == TK_LBRACKET) {
                    (*pos)++;
                    int64_t idx = parse_equality(c, pos).i;
                    if (*pos < c->token_count && c->tokens[*pos].type == TK_RBRACKET) (*pos)++;
                    size_t elem_sz = (ty.base && ty.base->size > 0) ? ty.base->size : (ty.size > 0 ? ty.size : 4);
                    total_offset += idx * (int64_t)elem_sz;
                    if (ty.base) ty = *ty.base;
                } else {
                    break;
                }
            }
            while (open_parens > 0 && *pos < c->token_count && c->tokens[*pos].type == TK_RPAREN) {
                (*pos)++;
                open_parens--;
            }
            return zc_cv_int(base_val + total_offset);
        }
        /* Fallback if not an offsetof pattern: restore *pos */
        *pos = save;
    }
    return parse_primary(c, pos);
}

int64_t zero_cc_eval_builtin_offsetof(ZeroCCCompiler *c, size_t *pos)
{
    (*pos)++; /* consume __builtin_offsetof */
    if (*pos < c->token_count && c->tokens[*pos].type == TK_LPAREN) {
        (*pos)++; /* consume '(' */
    }
    ZeroCCType ty = zero_cc_parse_type_name_at_pos(c, pos);
    if (*pos < c->token_count && c->tokens[*pos].type == TK_COMMA) {
        (*pos)++; /* consume ',' */
    }

    int64_t total_offset = 0;
    while (*pos < c->token_count && c->tokens[*pos].type != TK_RPAREN) {
        if (c->tokens[*pos].type == TK_DOT) {
            (*pos)++;
        }
        if (*pos < c->token_count && c->tokens[*pos].type == TK_IDENT) {
            const char *mname = c->tokens[*pos].str_val;
            (*pos)++;
            bool found = false;
            if (ty.tag == TYPE_STRUCT || ty.tag == TYPE_UNION) {
                for (size_t m = 0; m < ty.member_count; m++) {
                    if (strcmp(ty.members[m].name, mname) == 0) {
                        total_offset += (int64_t)ty.members[m].offset;
                        if (ty.members[m].base && (ty.members[m].tag == TYPE_STRUCT || ty.members[m].tag == TYPE_UNION)) {
                            ty = *ty.members[m].base;
                        } else if (ty.members[m].base && ty.members[m].tag == TYPE_ARRAY) {
                            ty.base = ty.members[m].base;
                            ty.tag = TYPE_ARRAY;
                            ty.size = ty.members[m].size;
                        } else {
                            ty.tag = ty.members[m].tag;
                            ty.size = ty.members[m].size;
                            ty.base = ty.members[m].base;
                            ty.member_count = 0;
                        }
                        found = true;
                        break;
                    }
                }
            }
            if (!found) {
                break;
            }
        } else if (*pos < c->token_count && c->tokens[*pos].type == TK_LBRACKET) {
            (*pos)++; /* consume '[' */
            int64_t idx = parse_equality(c, pos).i;
            if (*pos < c->token_count && c->tokens[*pos].type == TK_RBRACKET) {
                (*pos)++; /* consume ']' */
            }
            size_t elem_sz = (ty.base && ty.base->size > 0) ? ty.base->size : (ty.size > 0 ? ty.size : 4);
            total_offset += idx * (int64_t)elem_sz;
            if (ty.base) {
                ty = *ty.base;
            }
        } else {
            break;
        }
    }

    if (*pos < c->token_count && c->tokens[*pos].type == TK_RPAREN) {
        (*pos)++; /* consume ')' */
    }
    return total_offset;
}

/* Sprint 68: the value of an already-initialized scalar global, read back
 * from the data section (its bytes were emitted when the global was
 * declared).  Constant expressions may reference earlier globals:
 * `const char a = 0x42; ... double b = (double) a;` (c-torture compile/
 * pr83222).  Returns false if the identifier is not a readable scalar
 * global (non-scalar types, externs without storage, not yet emitted). */
bool zc_const_global_scalar(ZeroCCCompiler *c, const char *name,
                                          bool *out_is_float,
                                          int64_t *out_i, double *out_d) {
    ZeroCCGlobalVar *gv = find_global_var(c, name);
    if (!gv || gv->size == 0) return false;
    if (gv->data_offset + gv->size > c->data_section.length) return false;
    ZeroCCType t = gv->type;
    if (t.tag == TYPE_ARRAY && t.base) t = *t.base;
    if (t.tag == TYPE_PTR || t.tag == TYPE_STRUCT || t.tag == TYPE_UNION) return false;
    if (t.is_func || t.is_func_ptr) return false;
    const uint8_t *p = (const uint8_t *)c->data_section.data + gv->data_offset;
    uint64_t bits = 0;
    size_t sz = (t.size > 0 && t.size <= 8) ? t.size : 8;
    for (size_t i = 0; i < sz; i++) bits |= (uint64_t)p[i] << (8 * i);
    *out_is_float = (t.tag == TYPE_FLOAT || t.tag == TYPE_DOUBLE);
    if (*out_is_float) {
        if (t.tag == TYPE_FLOAT) {
            float f;
            memcpy(&f, &bits, 4);
            *out_d = f;
        } else {
            double d;
            memcpy(&d, &bits, 8);
            *out_d = d;
        }
        *out_i = 0;
    } else if (t.tag == TYPE_UINT8 || t.tag == TYPE_UINT16 ||
               t.tag == TYPE_UINT32 || t.tag == TYPE_UINT64) {
        *out_i = (int64_t)bits;
        *out_d = (double)(uint64_t)bits;
    } else {
        /* Sign-extend from the stored width. */
        int64_t sv = (int64_t)bits;
        *out_i = (sv << (64 - sz * 8)) >> (64 - sz * 8);
        *out_d = (double)*out_i;
    }
    return true;
}

ZCConstVal parse_primary(ZeroCCCompiler *c, size_t *pos)
{
    ZeroCCTokType t = c->tokens[*pos].type;
    if (t == TK_NUM) {
        int64_t val = c->tokens[*pos].num_val;
        uint8_t suf = c->tokens[*pos].int_suffix;
        (*pos)++;
        if ((suf & 8) != 0) return zc_cv_complex(0.0, (double)val);
        return zc_cv_int(val);
    }
    if (t == TK_FLOAT) {
        double val = c->tokens[*pos].float_val;
        uint8_t suf = c->tokens[*pos].int_suffix;
        (*pos)++;
        if ((suf & 8) != 0) return zc_cv_complex(0.0, val);
        return zc_cv_float(val);
    }
    if (t == TK_CHAR_LIT) {
        int64_t val = c->tokens[*pos].num_val;
        (*pos)++;
        return zc_cv_int(val);
    }
    if (t == TK_TRUE) {
        (*pos)++;
        return zc_cv_int(1);
    }
    if (t == TK_FALSE || t == TK_NULLPTR) {
        (*pos)++;
        return zc_cv_int(0);
    }
    if (t == TK_IDENT) {
        if (strcmp(c->tokens[*pos].str_val, "__builtin_offsetof") == 0) {
            return zc_cv_int(zero_cc_eval_builtin_offsetof(c, pos));
        }
        if (strcmp(c->tokens[*pos].str_val, "__builtin_constant_p") == 0) {
            /* GCC extension: 1 if the argument is a constant expression
             * (C99 6.6: literals, enum constants, sizeof/alignof, NULL,
             * true/false, and arithmetic on those) and 0 for anything else
             * (plain identifiers, calls, addresses, side effects).
             * pr61159: `typedef char check[2*!__builtin_constant_p(x)-1];`.
             * Skip the argument (balanced parens), then probe it: any
             * non-enum identifier or call-like token makes it non-constant. */
            (*pos)++;
            if (*pos < c->token_count && c->tokens[*pos].type == TK_LPAREN) (*pos)++;
            size_t a0 = *pos, a1 = *pos;
            (void)a0;
            int pd = 0;
            bool const_ok = true;
            while (a1 < c->token_count) {
                ZeroCCTokType at = c->tokens[a1].type;
                if (at == TK_LPAREN) pd++;
                else if (at == TK_RPAREN) {
                    if (pd == 0) break;
                    pd--;
                } else if (at == TK_COMMA && pd == 0) {
                    break;
                } else if (at == TK_IDENT && pd == 0) {
                    const char *an = c->tokens[a1].str_val;
                    if (strcmp(an, "true") != 0 && strcmp(an, "false") != 0 &&
                        strcmp(an, "NULL") != 0 && strcmp(an, "nullptr") != 0 &&
                        !zero_cc_find_enum_const(c, an) &&
                        strcmp(an, "__alignof__") != 0 &&
                        strcmp(an, "__builtin_offsetof") != 0) {
                        const_ok = false;
                    }
                }
                a1++;
            }
            /* Skip past the argument for the caller (to matching ')'). */
            *pos = a1;
            if (*pos < c->token_count && c->tokens[*pos].type == TK_RPAREN) (*pos)++;
            return zc_cv_int(const_ok ? 1 : 0);
        }
        if (strcmp(c->tokens[*pos].str_val, "__builtin_choose_expr") == 0) {
            /* GCC extension: __builtin_choose_expr(cond, a, b) picks a or b
             * at compile time from the constant cond (pr19449: `int z =
             * __builtin_choose_expr (!__builtin_constant_p (y), 3, 4);`).
             * Evaluate all three arguments (each must be constant in this
             * context) and select. */
            (*pos)++;
            if (*pos < c->token_count && c->tokens[*pos].type == TK_LPAREN) (*pos)++;
            ZCConstVal cond = parse_conditional(c, pos);
            if (*pos < c->token_count && c->tokens[*pos].type == TK_COMMA) (*pos)++;
            ZCConstVal a = parse_conditional(c, pos);
            if (*pos < c->token_count && c->tokens[*pos].type == TK_COMMA) (*pos)++;
            ZCConstVal b = parse_conditional(c, pos);
            if (*pos < c->token_count && c->tokens[*pos].type == TK_RPAREN) (*pos)++;
            return zc_cv_is_true(cond) ? a : b;
        }
        if (strcmp(c->tokens[*pos].str_val, "__alignof__") == 0 ||
            strcmp(c->tokens[*pos].str_val, "__alignof") == 0) {
            /* C99 6.5.3.4: __alignof__(E) = alignment of E's type.
             * E may be a type name (`__alignof__(int)`). */
            (*pos)++;
            if (*pos < c->token_count && c->tokens[*pos].type == TK_LPAREN) (*pos)++;
            ZeroCCType aty;
            if (zero_cc_tok_is_type_start(c, *pos)) {
                aty = zero_cc_parse_type_name_at_pos(c, pos);
            } else {
                /* A function-name operand uses the function's
                 * __attribute__((aligned(N))) (c-torture execute/align-3). */
                ZeroCCFunction *aln_fn = NULL;
                if (c->tokens[*pos].type == TK_IDENT) {
                    aln_fn = zero_cc_find_function(c, c->tokens[*pos].str_val);
                }
                aty = zero_cc_type_of_equality(c, pos);
                if (aln_fn && aln_fn->align > 0) {
                    return zc_cv_int((int64_t)aln_fn->align);
                }
            }
            if (*pos < c->token_count && c->tokens[*pos].type == TK_RPAREN) (*pos)++;
            return zc_cv_int((int64_t)(aty.align > 0 ? aty.align : 8));
        }
        /* Enum constants are valid in constant expressions (C99 6.6). */
        const ZeroCCEnumConst *ec = zero_cc_find_enum_const(c, c->tokens[*pos].str_val);
        if (ec) {
            (*pos)++;
            return zc_cv_int(ec->value);
        }
        /* An earlier global's value (object constants in constant
         * expressions, e.g. `const char a = 0x42; ... (double) a`). */
        {
            bool g_is_f = false;
            int64_t g_i = 0;
            double g_d = 0.0;
            if (zc_const_global_scalar(c, c->tokens[*pos].str_val, &g_is_f, &g_i, &g_d)) {
                (*pos)++;
                return g_is_f ? zc_cv_float(g_d) : zc_cv_int(g_i);
            }
        }
        /* Unknown identifier (e.g. an extern with no initializer value):
         * consume it and yield 0 so the caller's stream stays in sync
         * (c-torture compile/limits-externdecl: `(char *) X00000` inside a
         * brace initializer). */
        (*pos)++;
        return zc_cv_int(0);
    }
    if (t == TK_LPAREN) {
        (*pos)++;
        if (zero_cc_tok_is_type_start(c, *pos)) {
            ZeroCCType cast_ty = zero_cc_parse_type_name_at_pos(c, pos);
            /* Pointer cast: `(char *) expr` - consume the star run so the
             * `)` closes the cast (c-torture compile/limits-externdecl).
             * Without this the `*` was re-read as an indirection operator
             * and the brace-initializer element desynced. */
            while (*pos < c->token_count && c->tokens[*pos].type == TK_STAR) {
                cast_ty = zero_cc_type_ptr(&cast_ty);
                (*pos)++;
            }
            if (*pos < c->token_count && c->tokens[*pos].type == TK_RPAREN) (*pos)++;
            ZCConstVal val = parse_unary(c, pos);
            return zc_cv_cast(val, cast_ty);
        }
        ZCConstVal val = parse_conditional(c, pos);
        if (c->tokens[*pos].type == TK_RPAREN) {
            (*pos)++;
        }
        return val;
    }
    /* Should not reach here for valid constant expression */
    return zc_cv_int(0);
}

int64_t zero_cc_eval_const_expr(ZeroCCCompiler *c)
{
    size_t pos = c->token_pos;
    ZCConstVal result = parse_conditional(c, &pos);
    c->token_pos = pos;
    return result.i;
}

/* Like zero_cc_eval_const_expr but returns the full value, so 128-bit
 * initializers can read the high half (Sprint 109). */
ZCConstVal zero_cc_eval_const_expr_val(ZeroCCCompiler *c)
{
    size_t pos = c->token_pos;
    ZCConstVal result = parse_conditional(c, &pos);
    c->token_pos = pos;
    return result;
}

/* Probe whether the array dimension starting at token index `start` (up
 * to the matching ']') is a constant expression.  Used to split plain
 * arrays/typedefs from VLA forms: an identifier that is not an enum
 * constant / const global / true / false / NULL makes the dimension
 * runtime - except the constant-constructing operators (sizeof,
 * __alignof__, offsetof, ...), whose operands (types or expressions)
 * are constant by definition and are skipped.  A false "VLA" verdict on
 * a constant is harmless for typedefs (the size expression evaluates to
 * the same value at runtime) but changes layout for struct members, so
 * keep the sizeof/alignof/offsetof special-casing. */
bool zc_typedef_dim_is_const(ZeroCCCompiler *c, size_t start) {
    size_t p = start;
    int pd = 0;
    while (p < c->token_count) {
        ZeroCCTokType t = c->tokens[p].type;
        if (t == TK_LPAREN) {
            pd++;
        } else if (t == TK_RPAREN) {
            if (pd == 0) break;
            pd--;
        } else if (t == TK_RBRACKET && pd == 0) {
            break;
        } else if (t == TK_SIZEOF) {
            /* sizeof operand is constant by definition - skip a balanced
             * paren group or a bare type name. */
            p++;
            if (p < c->token_count && c->tokens[p].type == TK_LPAREN) {
                int d2 = 0;
                do {
                    if (c->tokens[p].type == TK_LPAREN) d2++;
                    else if (c->tokens[p].type == TK_RPAREN) d2--;
                    p++;
                } while (p < c->token_count && d2 > 0);
                continue;
            }
            if (p < c->token_count && c->tokens[p].type == TK_IDENT) p++;
            continue;
        } else if (t == TK_IDENT) {
            const char *nm = c->tokens[p].str_val;
            /* Constant-constructing operator: skip its operand (balanced
             * parens, or a bare type name) - the result is a constant. */
            bool is_cst_op = (strcmp(nm, "sizeof") == 0 ||
                              strcmp(nm, "__alignof__") == 0 ||
                              strcmp(nm, "__alignof") == 0 ||
                              strcmp(nm, "offsetof") == 0 ||
                              strcmp(nm, "__builtin_offsetof") == 0);
            if (is_cst_op) {
                p++;
                if (p < c->token_count && c->tokens[p].type == TK_LPAREN) {
                    int d2 = 0;
                    do {
                        if (c->tokens[p].type == TK_LPAREN) d2++;
                        else if (c->tokens[p].type == TK_RPAREN) d2--;
                        p++;
                    } while (p < c->token_count && d2 > 0);
                    continue;
                }
                if (p < c->token_count && c->tokens[p].type == TK_IDENT) { p++; continue; }
                continue;
            }
            bool known = (strcmp(nm, "true") == 0 || strcmp(nm, "false") == 0 ||
                          strcmp(nm, "NULL") == 0 || strcmp(nm, "nullptr") == 0 ||
                          strcmp(nm, "__alignof__") == 0 ||
                          zero_cc_find_enum_const(c, nm) != NULL ||
                          zc_const_global_scalar(c, nm, NULL, NULL, NULL));
            bool call_like = (p + 1 < c->token_count &&
                              c->tokens[p + 1].type == TK_LPAREN);
            if (!known || call_like) return false;
        }
        p++;
    }
    return true;
}




