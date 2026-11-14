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
    v.ptr = NULL;
    v.ptr_elem_size = 0;
    v.ptr_elem_tag = 0;
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
    v.ptr = NULL;
    v.ptr_elem_size = 0;
    v.ptr_elem_tag = 0;
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
    v.ptr = NULL;
    v.ptr_elem_size = 0;
    v.ptr_elem_tag = 0;
    return v;
}

ZCConstVal zc_cv_ptr(uint8_t *ptr, uint32_t elem_size, uint8_t elem_tag) {
    ZCConstVal v;
    v.kind = ZC_CV_PTR;
    v.i = (int64_t)(uintptr_t)ptr;
    v.r = 0.0;
    v.im = 0.0;
    v.hi = 0;
    v.w128 = false;
    v.signed_ = false;
    v.ptr = ptr;
    v.ptr_elem_size = elem_size ? elem_size : 1;
    v.ptr_elem_tag = elem_tag;
    return v;
}

ZCConstVal zc_cv_deref(ZCConstVal p, int64_t index) {
    if (p.kind != ZC_CV_PTR || !p.ptr) return zc_cv_int(0);
    uint32_t esz = p.ptr_elem_size ? p.ptr_elem_size : 1;
    const uint8_t *addr = p.ptr + index * (int64_t)esz;
    switch ((ZeroTypeTag)p.ptr_elem_tag) {
        case TYPE_FLOAT: {
            float f = 0.0f;
            memcpy(&f, addr, 4);
            return zc_cv_float((double)f);
        }
        case TYPE_DOUBLE: {
            double d = 0.0;
            memcpy(&d, addr, 8);
            return zc_cv_float(d);
        }
        case TYPE_BOOL:
            return zc_cv_int(addr[0] ? 1 : 0);
        case TYPE_INT8:
            return zc_cv_int((int64_t)(int8_t)addr[0]);
        case TYPE_UINT8:
            return zc_cv_int((int64_t)(uint8_t)addr[0]);
        case TYPE_INT16: {
            int16_t v = 0;
            memcpy(&v, addr, 2);
            return zc_cv_int((int64_t)v);
        }
        case TYPE_UINT16: {
            uint16_t v = 0;
            memcpy(&v, addr, 2);
            return zc_cv_int((int64_t)v);
        }
        case TYPE_INT32: {
            int32_t v = 0;
            memcpy(&v, addr, 4);
            return zc_cv_int((int64_t)v);
        }
        case TYPE_UINT32: {
            uint32_t v = 0;
            memcpy(&v, addr, 4);
            return zc_cv_int((int64_t)v);
        }
        case TYPE_INT64:
        case TYPE_UINT64: {
            int64_t v = 0;
            memcpy(&v, addr, 8);
            return zc_cv_int(v);
        }
        case TYPE_PTR: {
            uintptr_t raw = 0;
            memcpy(&raw, addr, sizeof(uintptr_t));
            return zc_cv_ptr((uint8_t *)raw, 1, TYPE_INT8);
        }
        default: {
            uint64_t bits = 0;
            size_t sz = (esz > 0 && esz <= 8) ? esz : 4;
            for (size_t b = 0; b < sz; b++) bits |= (uint64_t)addr[b] << (8 * b);
            int64_t sv = (int64_t)bits;
            if (sz < 8) sv = (sv << (64 - sz * 8)) >> (64 - sz * 8);
            return zc_cv_int(sv);
        }
    }
}

void zc_cv_store(ZCConstVal p, int64_t index, ZCConstVal val) {
    if (p.kind != ZC_CV_PTR || !p.ptr) return;
    uint32_t esz = p.ptr_elem_size ? p.ptr_elem_size : 1;
    uint8_t *addr = p.ptr + index * (int64_t)esz;
    switch ((ZeroTypeTag)p.ptr_elem_tag) {
        case TYPE_FLOAT: {
            float f = (val.kind == ZC_CV_INT) ? (float)val.i : (float)val.r;
            memcpy(addr, &f, 4);
            break;
        }
        case TYPE_DOUBLE: {
            double d = (val.kind == ZC_CV_INT) ? (double)val.i : val.r;
            memcpy(addr, &d, 8);
            break;
        }
        case TYPE_BOOL:
            addr[0] = zc_cv_is_true(val) ? 1 : 0;
            break;
        case TYPE_INT8:
        case TYPE_UINT8: {
            int64_t iv = (val.kind == ZC_CV_FLOAT) ? (int64_t)val.r : val.i;
            addr[0] = (uint8_t)iv;
            break;
        }
        case TYPE_INT16:
        case TYPE_UINT16: {
            int64_t iv = (val.kind == ZC_CV_FLOAT) ? (int64_t)val.r : val.i;
            uint16_t u = (uint16_t)iv;
            memcpy(addr, &u, 2);
            break;
        }
        case TYPE_INT32:
        case TYPE_UINT32: {
            int64_t iv = (val.kind == ZC_CV_FLOAT) ? (int64_t)val.r : val.i;
            uint32_t u = (uint32_t)iv;
            memcpy(addr, &u, 4);
            break;
        }
        case TYPE_INT64:
        case TYPE_UINT64:
        case TYPE_PTR:
        default: {
            int64_t iv = (val.kind == ZC_CV_FLOAT) ? (int64_t)val.r : val.i;
            size_t sz = (esz > 0 && esz <= 8) ? esz : 8;
            memcpy(addr, &iv, sz);
            break;
        }
    }
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
    if (ty.tag == TYPE_PTR || ty.tag == TYPE_ARRAY) {
        uint32_t esz = (ty.base && ty.base->size > 0) ? (uint32_t)ty.base->size : 1;
        uint8_t etag = (ty.base && ty.base->tag != TYPE_VOID) ? (uint8_t)ty.base->tag : TYPE_INT8;
        if (v.kind == ZC_CV_PTR) {
            if (ty.base && ty.base->tag != TYPE_VOID) {
                v.ptr_elem_size = esz;
                v.ptr_elem_tag = etag;
            }
            return v;
        }
        int64_t addr = (v.kind == ZC_CV_INT) ? v.i : (int64_t)v.r;
        return zc_cv_ptr((uint8_t *)(uintptr_t)addr, esz, etag);
    }
    if (ty.is_complex) {
        double r = (v.kind == ZC_CV_INT || v.kind == ZC_CV_PTR) ? (double)v.i : v.r;
        double im = (v.kind == ZC_CV_COMPLEX) ? v.im : 0.0;
        return zc_cv_complex(r, im);
    }
    if (ty.tag == TYPE_FLOAT || ty.tag == TYPE_DOUBLE) {
        double r = (v.kind == ZC_CV_INT || v.kind == ZC_CV_PTR) ? (double)v.i : v.r;
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
    int64_t val = (v.kind == ZC_CV_INT || v.kind == ZC_CV_PTR) ? v.i : (int64_t)v.r;
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
    if (a.kind == ZC_CV_PTR && b.kind == ZC_CV_INT) {
        uint32_t esz = a.ptr_elem_size ? a.ptr_elem_size : 1;
        uint8_t *np = a.ptr ? (a.ptr + b.i * (int64_t)esz) : (uint8_t *)(uintptr_t)(b.i * (int64_t)esz);
        return zc_cv_ptr(np, esz, a.ptr_elem_tag);
    }
    if (a.kind == ZC_CV_INT && b.kind == ZC_CV_PTR) {
        uint32_t esz = b.ptr_elem_size ? b.ptr_elem_size : 1;
        uint8_t *np = b.ptr ? (b.ptr + a.i * (int64_t)esz) : (uint8_t *)(uintptr_t)(a.i * (int64_t)esz);
        return zc_cv_ptr(np, esz, b.ptr_elem_tag);
    }
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
    if (a.kind == ZC_CV_PTR && b.kind == ZC_CV_INT) {
        uint32_t esz = a.ptr_elem_size ? a.ptr_elem_size : 1;
        uint8_t *np = a.ptr ? (a.ptr - b.i * (int64_t)esz) : (uint8_t *)(uintptr_t)(-b.i * (int64_t)esz);
        return zc_cv_ptr(np, esz, a.ptr_elem_tag);
    }
    if (a.kind == ZC_CV_PTR && b.kind == ZC_CV_PTR) {
        int64_t esz = (int64_t)(a.ptr_elem_size ? a.ptr_elem_size : (b.ptr_elem_size ? b.ptr_elem_size : 1));
        ptrdiff_t diff = a.ptr - b.ptr;
        return zc_cv_int((int64_t)(diff / esz));
    }
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
    if (a.kind == ZC_CV_PTR || b.kind == ZC_CV_PTR) {
        uintptr_t ua = (uintptr_t)a.i;
        uintptr_t ub = (uintptr_t)b.i;
        int res = 0;
        if (op == TK_LT) res = (ua < ub);
        else if (op == TK_GT) res = (ua > ub);
        else if (op == TK_LE) res = (ua <= ub);
        else if (op == TK_GE) res = (ua >= ub);
        return zc_cv_int(res);
    }
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
/* Result of evaluating a statement during CTFE. */
typedef enum {
    CTF_NORMAL,     /* statement completed normally */
    CTF_RETURN,     /* return <expr>; - result in .val */
    CTF_BREAK,      /* break */
    CTF_CONTINUE,   /* continue */
} CtfKind;

typedef struct {
    CtfKind kind;
    ZCConstVal val;
} CtfResult;

ZCConstVal ctfe_eval_body(ZeroCCCompiler *c, size_t start, size_t end);
static CtfResult ctfe_eval_stmt(ZeroCCCompiler *c, size_t *pos, size_t end);
static CtfResult ctfe_eval_block(ZeroCCCompiler *c, size_t *pos, size_t end);
static CtfResult ctfe_eval_if(ZeroCCCompiler *c, size_t *pos, size_t end);
static CtfResult ctfe_eval_while(ZeroCCCompiler *c, size_t *pos, size_t end);
static CtfResult ctfe_eval_for(ZeroCCCompiler *c, size_t *pos, size_t end);
static CtfResult ctfe_eval_decl(ZeroCCCompiler *c, size_t *pos, size_t end);
static CtfResult ctfe_eval_expr_stmt(ZeroCCCompiler *c, size_t *pos, size_t end);
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
               strcmp(name, "__float128") == 0 ||
               strcmp(name, "_Float32") == 0 ||
               strcmp(name, "_Float64") == 0 ||
               strcmp(name, "_Float128") == 0 ||
               strcmp(name, "_Float32x") == 0 ||
               strcmp(name, "_Float64x") == 0;
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

static bool ctfe_is_assign_op(ZeroCCTokType t) {
    return t == TK_ASSIGN || t == TK_ADD_ASSIGN || t == TK_SUB_ASSIGN ||
           t == TK_MUL_ASSIGN || t == TK_DIV_ASSIGN || t == TK_MOD_ASSIGN ||
           t == TK_AND_ASSIGN || t == TK_OR_ASSIGN || t == TK_XOR_ASSIGN ||
           t == TK_SHL_ASSIGN || t == TK_SHR_ASSIGN;
}

static ZCConstVal ctfe_apply_assign_op(ZeroCCTokType op, ZCConstVal old_v, ZCConstVal rhs) {
    switch (op) {
        case TK_ASSIGN:     return rhs;
        case TK_ADD_ASSIGN: return zc_cv_add(old_v, rhs);
        case TK_SUB_ASSIGN: return zc_cv_sub(old_v, rhs);
        case TK_MUL_ASSIGN: return zc_cv_mul(old_v, rhs);
        case TK_DIV_ASSIGN: return zc_cv_div(old_v, rhs);
        case TK_MOD_ASSIGN: return zc_cv_mod(old_v, rhs);
        case TK_AND_ASSIGN: return zc_cv_int(old_v.i & rhs.i);
        case TK_OR_ASSIGN:  return zc_cv_int(old_v.i | rhs.i);
        case TK_XOR_ASSIGN: return zc_cv_int(old_v.i ^ rhs.i);
        case TK_SHL_ASSIGN: return zc_cv_int(old_v.i << rhs.i);
        case TK_SHR_ASSIGN: return zc_cv_int(old_v.i >> rhs.i);
        default:            return rhs;
    }
}

static ssize_t ctfe_find_local_idx(ZeroCCCompiler *c, const char *name) {
    for (size_t li = c->ctfe_local_count; li > c->ctfe_local_base; li--) {
        if (c->ctfe_local_names[li - 1] &&
            strcmp(c->ctfe_local_names[li - 1], name) == 0) {
            return (ssize_t)(li - 1);
        }
    }
    return -1;
}

static ssize_t ctfe_find_param_idx(ZeroCCCompiler *c, const char *name) {
    for (size_t pi = 0; pi < c->ctfe_param_count; pi++) {
        if (c->ctfe_param_names[pi] &&
            strcmp(c->ctfe_param_names[pi], name) == 0) {
            return (ssize_t)pi;
        }
    }
    return -1;
}

static ZCConstVal ctfe_read_local(ZeroCCCompiler *c, size_t idx) {
    if (c->ctfe_local_kinds[idx] == (uint8_t)ZC_CV_PTR) {
        return zc_cv_ptr((uint8_t *)(uintptr_t)c->ctfe_local_values_i[idx],
                         c->ctfe_local_elem_sizes ? c->ctfe_local_elem_sizes[idx] : 1,
                         c->ctfe_local_elem_tags ? c->ctfe_local_elem_tags[idx] : TYPE_INT8);
    }
    if (c->ctfe_local_bufs && c->ctfe_local_bufs[idx]) {
        uint32_t esz = (c->ctfe_local_elem_sizes && c->ctfe_local_elem_sizes[idx]) ? c->ctfe_local_elem_sizes[idx] : 4;
        uint8_t etag = (c->ctfe_local_elem_tags && c->ctfe_local_elem_tags[idx]) ? c->ctfe_local_elem_tags[idx] : TYPE_INT32;
        return zc_cv_deref(zc_cv_ptr(c->ctfe_local_bufs[idx], esz, etag), 0);
    }
    ZCConstVal v = zc_cv_int(0);
    v.kind = (ZCConstValKind)c->ctfe_local_kinds[idx];
    v.i = c->ctfe_local_values_i[idx];
    v.r = c->ctfe_local_values_r[idx];
    return v;
}

static void ctfe_write_local(ZeroCCCompiler *c, size_t idx, ZCConstVal val) {
    if (c->ctfe_local_bufs && c->ctfe_local_bufs[idx] &&
        c->ctfe_local_kinds[idx] != (uint8_t)ZC_CV_PTR) {
        uint32_t esz = (c->ctfe_local_elem_sizes && c->ctfe_local_elem_sizes[idx]) ? c->ctfe_local_elem_sizes[idx] : 4;
        uint8_t etag = (c->ctfe_local_elem_tags && c->ctfe_local_elem_tags[idx]) ? c->ctfe_local_elem_tags[idx] : TYPE_INT32;
        ZCConstVal p = zc_cv_ptr(c->ctfe_local_bufs[idx], esz, etag);
        zc_cv_store(p, 0, val);
        ZCConstVal rv = zc_cv_deref(p, 0);
        c->ctfe_local_values_i[idx] = rv.i;
        c->ctfe_local_values_r[idx] = rv.r;
        return;
    }
    c->ctfe_local_values_i[idx] = val.i;
    c->ctfe_local_values_r[idx] = val.r;
    c->ctfe_local_kinds[idx] = (uint8_t)val.kind;
    if (val.kind == ZC_CV_PTR && c->ctfe_local_elem_sizes && c->ctfe_local_elem_tags) {
        c->ctfe_local_elem_sizes[idx] = val.ptr_elem_size;
        c->ctfe_local_elem_tags[idx] = val.ptr_elem_tag;
    }
}

static ZCConstVal ctfe_read_param(ZeroCCCompiler *c, size_t pi) {
    if (c->ctfe_param_kinds[pi] == (uint8_t)ZC_CV_PTR) {
        return zc_cv_ptr((uint8_t *)(uintptr_t)c->ctfe_param_values_i[pi],
                         c->ctfe_param_elem_sizes[pi] ? c->ctfe_param_elem_sizes[pi] : 1,
                         c->ctfe_param_elem_tags[pi]);
    }
    ZCConstVal v = zc_cv_int(0);
    v.kind = (ZCConstValKind)c->ctfe_param_kinds[pi];
    v.i = c->ctfe_param_values_i[pi];
    v.r = c->ctfe_param_values_r[pi];
    return v;
}

static void ctfe_write_param(ZeroCCCompiler *c, size_t pi, ZCConstVal val) {
    c->ctfe_param_values_i[pi] = val.i;
    c->ctfe_param_values_r[pi] = val.r;
    c->ctfe_param_kinds[pi] = (uint8_t)val.kind;
    if (val.kind == ZC_CV_PTR) {
        c->ctfe_param_elem_sizes[pi] = val.ptr_elem_size;
        c->ctfe_param_elem_tags[pi] = val.ptr_elem_tag;
    }
}

static ZCConstVal parse_postfix(ZeroCCCompiler *c, size_t *pos) {
    ZCConstVal v = parse_primary(c, pos);
    while (*pos < c->token_count) {
        ZeroCCTokType pt = c->tokens[*pos].type;
        if (pt == TK_LBRACKET) {
            (*pos)++; /* consume '[' */
            ZCConstVal idx = parse_conditional(c, pos);
            if (*pos < c->token_count && c->tokens[*pos].type == TK_RBRACKET) (*pos)++;
            ZCConstVal ptr_val = (v.kind == ZC_CV_PTR) ? v : idx;
            int64_t index_val = (v.kind == ZC_CV_PTR) ? idx.i : v.i;
            if (*pos < c->token_count && ctfe_is_assign_op(c->tokens[*pos].type)) {
                ZeroCCTokType op = c->tokens[*pos].type;
                (*pos)++;
                ZCConstVal rhs = parse_conditional(c, pos);
                ZCConstVal old_v = (op == TK_ASSIGN) ? zc_cv_int(0) : zc_cv_deref(ptr_val, index_val);
                ZCConstVal new_v = ctfe_apply_assign_op(op, old_v, rhs);
                zc_cv_store(ptr_val, index_val, new_v);
                v = new_v;
            } else if (*pos < c->token_count &&
                       (c->tokens[*pos].type == TK_INC || c->tokens[*pos].type == TK_DEC)) {
                bool is_inc = (c->tokens[*pos].type == TK_INC);
                (*pos)++;
                ZCConstVal old_v = zc_cv_deref(ptr_val, index_val);
                ZCConstVal new_v = is_inc ? zc_cv_add(old_v, zc_cv_int(1))
                                          : zc_cv_sub(old_v, zc_cv_int(1));
                zc_cv_store(ptr_val, index_val, new_v);
                v = old_v;
            } else {
                v = zc_cv_deref(ptr_val, index_val);
            }
        } else {
            break;
        }
    }
    return v;
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
    if (t == TK_STAR) {
        int star_depth = 0;
        while (*pos < c->token_count && c->tokens[*pos].type == TK_STAR) {
            star_depth++;
            (*pos)++;
        }
        ZCConstVal ptr_val = (*pos < c->token_count &&
                              (c->tokens[*pos].type == TK_INC || c->tokens[*pos].type == TK_DEC))
                             ? parse_unary(c, pos)
                             : parse_postfix(c, pos);
        for (int s = 1; s < star_depth; s++) {
            ptr_val = zc_cv_deref(ptr_val, 0);
        }
        if (*pos < c->token_count && ctfe_is_assign_op(c->tokens[*pos].type)) {
            ZeroCCTokType op = c->tokens[*pos].type;
            (*pos)++;
            ZCConstVal rhs = parse_conditional(c, pos);
            ZCConstVal old_v = (op == TK_ASSIGN) ? zc_cv_int(0) : zc_cv_deref(ptr_val, 0);
            ZCConstVal new_v = ctfe_apply_assign_op(op, old_v, rhs);
            zc_cv_store(ptr_val, 0, new_v);
            return new_v;
        }
        return zc_cv_deref(ptr_val, 0);
    }
    if (t == TK_INC || t == TK_DEC) {
        bool is_inc = (t == TK_INC);
        (*pos)++; /* consume '++' or '--' */
        if (*pos < c->token_count && c->tokens[*pos].type == TK_STAR) {
            (*pos)++; /* consume '*' */
            ZCConstVal ptr_val = parse_unary(c, pos);
            ZCConstVal old_v = zc_cv_deref(ptr_val, 0);
            ZCConstVal new_v = is_inc ? zc_cv_add(old_v, zc_cv_int(1))
                                      : zc_cv_sub(old_v, zc_cv_int(1));
            zc_cv_store(ptr_val, 0, new_v);
            return new_v;
        }
        if (*pos < c->token_count && c->tokens[*pos].type == TK_IDENT) {
            const char *name = c->tokens[*pos].str_val;
            ssize_t li = ctfe_find_local_idx(c, name);
            ssize_t pi = (li < 0) ? ctfe_find_param_idx(c, name) : -1;
            if (li >= 0 || pi >= 0) {
                (*pos)++; /* consume ident */
                ZCConstVal cur = (li >= 0) ? ctfe_read_local(c, (size_t)li)
                                           : ctfe_read_param(c, (size_t)pi);
                if (*pos < c->token_count && c->tokens[*pos].type == TK_LBRACKET) {
                    (*pos)++;
                    ZCConstVal idx = parse_conditional(c, pos);
                    if (*pos < c->token_count && c->tokens[*pos].type == TK_RBRACKET) (*pos)++;
                    ZCConstVal old_v = zc_cv_deref(cur, idx.i);
                    ZCConstVal new_v = is_inc ? zc_cv_add(old_v, zc_cv_int(1))
                                              : zc_cv_sub(old_v, zc_cv_int(1));
                    zc_cv_store(cur, idx.i, new_v);
                    return new_v;
                }
                ZCConstVal new_v = is_inc ? zc_cv_add(cur, zc_cv_int(1))
                                          : zc_cv_sub(cur, zc_cv_int(1));
                if (li >= 0) ctfe_write_local(c, (size_t)li, new_v);
                else ctfe_write_param(c, (size_t)pi, new_v);
                return new_v;
            }
        }
        ZCConstVal v = parse_unary(c, pos);
        return is_inc ? zc_cv_add(v, zc_cv_int(1)) : zc_cv_sub(v, zc_cv_int(1));
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
        /* Address-of in CTFE / constant expression: &*p, &ident, &arr[idx] */
        *pos = save + 1;
        if (*pos < c->token_count && c->tokens[*pos].type == TK_STAR) {
            (*pos)++;
            return parse_unary(c, pos);
        }
        if (*pos < c->token_count && c->tokens[*pos].type == TK_IDENT) {
            const char *name = c->tokens[*pos].str_val;
            ssize_t li = ctfe_find_local_idx(c, name);
            if (li >= 0) {
                (*pos)++;
                size_t idx = (size_t)li;
                uint32_t esz = (c->ctfe_local_elem_sizes && c->ctfe_local_elem_sizes[idx]) ? c->ctfe_local_elem_sizes[idx] : 4;
                uint8_t etag = (c->ctfe_local_elem_tags && c->ctfe_local_elem_tags[idx]) ? c->ctfe_local_elem_tags[idx] : TYPE_INT32;
                ZCConstVal base_ptr;
                if (c->ctfe_local_kinds[idx] == (uint8_t)ZC_CV_PTR) {
                    base_ptr = zc_cv_ptr((uint8_t *)(uintptr_t)c->ctfe_local_values_i[idx], esz, etag);
                } else {
                    if (!c->ctfe_local_bufs[idx]) {
                        c->ctfe_local_bufs[idx] = (uint8_t *)calloc(1, 8);
                        ZCConstVal cur = zc_cv_int(0);
                        cur.kind = (ZCConstValKind)c->ctfe_local_kinds[idx];
                        cur.i = c->ctfe_local_values_i[idx];
                        cur.r = c->ctfe_local_values_r[idx];
                        zc_cv_store(zc_cv_ptr(c->ctfe_local_bufs[idx], esz, etag), 0, cur);
                    }
                    base_ptr = zc_cv_ptr(c->ctfe_local_bufs[idx], esz, etag);
                }
                if (*pos < c->token_count && c->tokens[*pos].type == TK_LBRACKET) {
                    (*pos)++;
                    ZCConstVal sub = parse_conditional(c, pos);
                    if (*pos < c->token_count && c->tokens[*pos].type == TK_RBRACKET) (*pos)++;
                    return zc_cv_add(base_ptr, sub);
                }
                return base_ptr;
            }
            ssize_t pi = ctfe_find_param_idx(c, name);
            if (pi >= 0) {
                (*pos)++;
                size_t pidx = (size_t)pi;
                ZCConstVal base_ptr;
                if (c->ctfe_param_kinds[pidx] == (uint8_t)ZC_CV_PTR) {
                    base_ptr = ctfe_read_param(c, pidx);
                } else if (c->ctfe_param_kinds[pidx] == (uint8_t)ZC_CV_FLOAT) {
                    base_ptr = zc_cv_ptr((uint8_t *)&c->ctfe_param_values_r[pidx], 8, TYPE_DOUBLE);
                } else {
                    base_ptr = zc_cv_ptr((uint8_t *)&c->ctfe_param_values_i[pidx], 8, TYPE_INT64);
                }
                if (*pos < c->token_count && c->tokens[*pos].type == TK_LBRACKET) {
                    (*pos)++;
                    ZCConstVal sub = parse_conditional(c, pos);
                    if (*pos < c->token_count && c->tokens[*pos].type == TK_RBRACKET) (*pos)++;
                    return zc_cv_add(base_ptr, sub);
                }
                return base_ptr;
            }
            ZeroCCGlobalVar *gv = find_global_var(c, name);
            if (gv && gv->size > 0 && gv->data_offset + gv->size <= c->data_section.length) {
                (*pos)++;
                ZeroCCType et = (gv->type.tag == TYPE_ARRAY && gv->type.base) ? *gv->type.base : gv->type;
                uint32_t esz = (et.size > 0) ? (uint32_t)et.size : 4;
                uint8_t etag = (uint8_t)et.tag;
                ZCConstVal base_ptr = zc_cv_ptr((uint8_t *)c->data_section.data + gv->data_offset, esz, etag);
                if (*pos < c->token_count && c->tokens[*pos].type == TK_LBRACKET) {
                    (*pos)++;
                    ZCConstVal sub = parse_conditional(c, pos);
                    if (*pos < c->token_count && c->tokens[*pos].type == TK_RBRACKET) (*pos)++;
                    return zc_cv_add(base_ptr, sub);
                }
                return base_ptr;
            }
        }
        /* Fallback if not an address pattern: restore *pos */
        *pos = save;
    }
    if (t == TK_IDENT && *pos + 1 < c->token_count &&
        ctfe_is_assign_op(c->tokens[*pos + 1].type)) {
        const char *id_name = c->tokens[*pos].str_val;
        ssize_t li = ctfe_find_local_idx(c, id_name);
        ssize_t pi = (li < 0) ? ctfe_find_param_idx(c, id_name) : -1;
        if (li >= 0 || pi >= 0) {
            ZeroCCTokType op = c->tokens[*pos + 1].type;
            (*pos) += 2;
            ZCConstVal cur = (li >= 0) ? ctfe_read_local(c, (size_t)li)
                                       : ctfe_read_param(c, (size_t)pi);
            ZCConstVal rhs = parse_conditional(c, pos);
            ZCConstVal new_v = ctfe_apply_assign_op(op, cur, rhs);
            if (li >= 0) ctfe_write_local(c, (size_t)li, new_v);
            else ctfe_write_param(c, (size_t)pi, new_v);
            return new_v;
        }
    }
    return parse_postfix(c, pos);
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
    bool is_fl = (t.tag == TYPE_FLOAT || t.tag == TYPE_DOUBLE);
    if (out_is_float) *out_is_float = is_fl;
    if (is_fl) {
        double d_val;
        if (t.tag == TYPE_FLOAT) {
            float f;
            memcpy(&f, &bits, 4);
            d_val = f;
        } else {
            memcpy(&d_val, &bits, 8);
        }
        if (out_d) *out_d = d_val;
        if (out_i) *out_i = 0;
    } else if (t.tag == TYPE_UINT8 || t.tag == TYPE_UINT16 ||
               t.tag == TYPE_UINT32 || t.tag == TYPE_UINT64) {
        if (out_i) *out_i = (int64_t)bits;
        if (out_d) *out_d = (double)(uint64_t)bits;
    } else {
        /* Sign-extend from the stored width. */
        int64_t sv = (int64_t)bits;
        int64_t ext = (sv << (64 - sz * 8)) >> (64 - sz * 8);
        if (out_i) *out_i = ext;
        if (out_d) *out_d = (double)ext;
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
    if (t == TK_FALSE) {
        (*pos)++;
        return zc_cv_int(0);
    }
    if (t == TK_NULLPTR) {
        (*pos)++;
        return zc_cv_ptr(NULL, 1, TYPE_INT8);
    }
    if (t == TK_STRING) {
        const char *first = c->tokens[*pos].str_val;
        (*pos)++;
        if (*pos < c->token_count && c->tokens[*pos].type == TK_STRING) {
            size_t cap = strlen(first) + 64;
            char *buf = (char *)malloc(cap);
            size_t len = strlen(first);
            memcpy(buf, first, len);
            while (*pos < c->token_count && c->tokens[*pos].type == TK_STRING) {
                const char *part = c->tokens[*pos].str_val;
                size_t plen = strlen(part);
                if (len + plen + 1 > cap) {
                    cap = (len + plen + 1) * 2;
                    buf = (char *)realloc(buf, cap);
                }
                memcpy(buf + len, part, plen);
                len += plen;
                (*pos)++;
            }
            buf[len] = '\0';
            const char *interned = zero_cc_intern_string(c, buf);
            free(buf);
            return zc_cv_ptr((uint8_t *)(uintptr_t)interned, 1, TYPE_INT8);
        }
        const char *interned = zero_cc_intern_string(c, first);
        return zc_cv_ptr((uint8_t *)(uintptr_t)interned, 1, TYPE_INT8);
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
        if (strcmp(c->tokens[*pos].str_val, "NULL") == 0) {
            (*pos)++;
            return zc_cv_ptr(NULL, 1, TYPE_INT8);
        }
        /* Enum constants are valid in constant expressions (C99 6.6). */
        const ZeroCCEnumConst *ec = zero_cc_find_enum_const(c, c->tokens[*pos].str_val);
        if (ec) {
            (*pos)++;
            return zc_cv_int(ec->value);
        }

        /* C23 constexpr: local variable or parameter reference during CTFE.
         * Locals in the current function call frame shadow parameters, and
         * both shadow file-scope globals. */
        {
            const char *id_name = c->tokens[*pos].str_val;
            ssize_t li = ctfe_find_local_idx(c, id_name);
            ssize_t pi = (li < 0) ? ctfe_find_param_idx(c, id_name) : -1;
            if (li >= 0 || pi >= 0) {
                (*pos)++;
                ZCConstVal cur = (li >= 0) ? ctfe_read_local(c, (size_t)li)
                                           : ctfe_read_param(c, (size_t)pi);
                if (*pos < c->token_count &&
                    (c->tokens[*pos].type == TK_INC || c->tokens[*pos].type == TK_DEC)) {
                    bool is_inc = (c->tokens[*pos].type == TK_INC);
                    (*pos)++;
                    ZCConstVal new_v = is_inc ? zc_cv_add(cur, zc_cv_int(1))
                                              : zc_cv_sub(cur, zc_cv_int(1));
                    if (li >= 0) ctfe_write_local(c, (size_t)li, new_v);
                    else ctfe_write_param(c, (size_t)pi, new_v);
                    return cur;
                }
                return cur;
            }
        }

        /* Global array or pointer variable in constant / CTFE expressions. */
        {
            ZeroCCGlobalVar *gv = find_global_var(c, c->tokens[*pos].str_val);
            if (gv && gv->type.tag == TYPE_ARRAY && gv->size > 0 &&
                gv->data_offset + gv->size <= c->data_section.length) {
                (*pos)++;
                ZeroCCType et = gv->type.base ? *gv->type.base : zero_cc_type_int();
                uint32_t esz = (et.size > 0) ? (uint32_t)et.size : 4;
                return zc_cv_ptr((uint8_t *)c->data_section.data + gv->data_offset,
                                 esz, (uint8_t)et.tag);
            }
            if (gv && gv->type.tag == TYPE_PTR && gv->size == 8 &&
                gv->data_offset + 8 <= c->data_section.length) {
                ZeroCCType et = gv->type.base ? *gv->type.base : zero_cc_type_char();
                uint32_t esz = (et.size > 0) ? (uint32_t)et.size : 1;
                uint8_t *target = NULL;
                for (size_t ri = 0; ri < c->data_reloc_count; ri++) {
                    if (c->data_relocs[ri].data_patch_offset == gv->data_offset) {
                        if (c->data_relocs[ri].target_kind == 0 &&
                            c->data_relocs[ri].rodata_offset < c->rodata_section.length) {
                            target = (uint8_t *)c->rodata_section.data +
                                     c->data_relocs[ri].rodata_offset +
                                     c->data_relocs[ri].target_data_offset;
                        } else if (c->data_relocs[ri].target_kind == 1 &&
                                   c->data_relocs[ri].target_data_offset <= c->data_section.length) {
                            target = (uint8_t *)c->data_section.data +
                                     c->data_relocs[ri].target_data_offset;
                        }
                        break;
                    }
                }
                if (target) {
                    (*pos)++;
                    return zc_cv_ptr(target, esz, (uint8_t)et.tag);
                }
            }
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

        /* C23 constexpr function call: f(args) where f is a constexpr
         * function.  Evaluate arguments as constant expressions, bind them
         * to the function's parameters, and evaluate the function body. */
        if (*pos + 1 < c->token_count && c->tokens[*pos + 1].type == TK_LPAREN) {
            ZeroCCFunction *fn = zero_cc_find_function(c, c->tokens[*pos].str_val);
            if (fn && fn->is_constexpr &&
                fn->constexpr_body_start > 0 && fn->constexpr_body_end > fn->constexpr_body_start) {
                /* Consume the function name and opening paren. */
                size_t call_pos = *pos;
                call_pos++; /* identifier */
                call_pos++; /* '(' */

                /* Evaluate comma-separated arguments as constant expressions. */
                ZCConstVal arg_values[16];
                size_t arg_count = 0;
                while (call_pos < c->token_count &&
                       c->tokens[call_pos].type != TK_RPAREN &&
                       arg_count < 16) {
                    size_t saved_pos = c->token_pos;
                    c->token_pos = call_pos;
                    ZCConstVal arg_val = parse_conditional(c, &c->token_pos);
                    call_pos = c->token_pos;
                    c->token_pos = saved_pos;
                    arg_values[arg_count++] = arg_val;
                    if (call_pos < c->token_count &&
                        c->tokens[call_pos].type == TK_COMMA) {
                        call_pos++; /* consume ',' */
                    }
                }
                /* Consume closing ')'. */
                if (call_pos < c->token_count &&
                    c->tokens[call_pos].type == TK_RPAREN) {
                    call_pos++;
                }

                /* Set up CTFE parameter bindings from fn->param_names.
                 * Copy argument values into the inline arrays so they remain
                 * valid after parse_primary() returns (arg_values is a stack
                 * array that would dangle otherwise). */
                const char **saved_param_names = c->ctfe_param_names;
                int64_t saved_param_values_i[16];
                double saved_param_values_r[16];
                uint8_t saved_param_kinds[16];
                uint32_t saved_param_elem_sizes[16];
                uint8_t saved_param_elem_tags[16];
                size_t saved_param_count = c->ctfe_param_count;
                size_t saved_local_base = c->ctfe_local_base;

                for (size_t ai = 0; ai < c->ctfe_param_count; ai++) {
                    saved_param_values_i[ai] = c->ctfe_param_values_i[ai];
                    saved_param_values_r[ai] = c->ctfe_param_values_r[ai];
                    saved_param_kinds[ai] = c->ctfe_param_kinds[ai];
                    saved_param_elem_sizes[ai] = c->ctfe_param_elem_sizes[ai];
                    saved_param_elem_tags[ai] = c->ctfe_param_elem_tags[ai];
                }

                c->ctfe_param_names = (const char **)fn->param_names;
                for (size_t ai = 0; ai < arg_count; ai++) {
                    ZCConstVal av = arg_values[ai];
                    if (fn->param_types && ai < fn->param_count) {
                        av = zc_cv_cast(av, fn->param_types[ai]);
                    }
                    c->ctfe_param_values_i[ai] = av.i;
                    c->ctfe_param_values_r[ai] = av.r;
                    c->ctfe_param_kinds[ai] = (uint8_t)av.kind;
                    c->ctfe_param_elem_sizes[ai] = av.ptr_elem_size;
                    c->ctfe_param_elem_tags[ai] = av.ptr_elem_tag;
                }
                c->ctfe_param_count = (fn->param_count < 16) ? fn->param_count : 16;
                c->ctfe_local_base = c->ctfe_local_count;

                /* Save token position, evaluate the body, then restore. */
                size_t saved_token_pos = c->token_pos;
                c->token_pos = fn->constexpr_body_start;

                /* Evaluate the full body (supports if/else, while, for,
                 * blocks, declarations, etc.). */
                ZCConstVal result = ctfe_eval_body(c, fn->constexpr_body_start,
                                                   fn->constexpr_body_end);
                result = zc_cv_cast(result, fn->return_type);

                /* Restore CTFE state and token position. */
                c->ctfe_param_names = saved_param_names;
                for (size_t ai = 0; ai < saved_param_count; ai++) {
                    c->ctfe_param_values_i[ai] = saved_param_values_i[ai];
                    c->ctfe_param_values_r[ai] = saved_param_values_r[ai];
                    c->ctfe_param_kinds[ai] = saved_param_kinds[ai];
                    c->ctfe_param_elem_sizes[ai] = saved_param_elem_sizes[ai];
                    c->ctfe_param_elem_tags[ai] = saved_param_elem_tags[ai];
                }
                c->ctfe_param_count = saved_param_count;
                c->ctfe_local_base = saved_local_base;
                c->token_pos = saved_token_pos;

                /* Consume the full call (identifier through ')') for the caller. */
                *pos = call_pos;
                return result;
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
            bool is_arr_lit = (cast_ty.tag == TYPE_ARRAY);
            int64_t arr_cnt = (is_arr_lit && cast_ty.base && cast_ty.base->size > 0)
                              ? (int64_t)(cast_ty.size / cast_ty.base->size) : -1;
            if (*pos < c->token_count && c->tokens[*pos].type == TK_LBRACKET) {
                is_arr_lit = true;
                (*pos)++;
                if (*pos < c->token_count && c->tokens[*pos].type != TK_RBRACKET) {
                    arr_cnt = parse_conditional(c, pos).i;
                }
                if (*pos < c->token_count && c->tokens[*pos].type == TK_RBRACKET) (*pos)++;
            }
            if (*pos < c->token_count && c->tokens[*pos].type == TK_RPAREN) (*pos)++;
            if (*pos < c->token_count && c->tokens[*pos].type == TK_LBRACE) {
                /* Compound literal: `(const int[]){1, 2, 3}` or `(int[4]){1, 2}` */
                (*pos)++; /* consume '{' */
                ZeroCCType et = (cast_ty.tag == TYPE_ARRAY && cast_ty.base) ? *cast_ty.base : cast_ty;
                uint32_t esz = (et.size > 0) ? (uint32_t)et.size : 4;
                uint8_t etag = (uint8_t)et.tag;
                ZCConstVal elems[256];
                size_t ecnt = 0;
                while (*pos < c->token_count && c->tokens[*pos].type != TK_RBRACE) {
                    if (ecnt < 256) {
                        elems[ecnt++] = parse_conditional(c, pos);
                    } else {
                        (void)parse_conditional(c, pos);
                    }
                    if (*pos < c->token_count && c->tokens[*pos].type == TK_COMMA) (*pos)++;
                }
                if (*pos < c->token_count && c->tokens[*pos].type == TK_RBRACE) (*pos)++;
                size_t total_elems = (arr_cnt > 0 && (size_t)arr_cnt > ecnt) ? (size_t)arr_cnt : (ecnt > 0 ? ecnt : 1);
                uint8_t *buf = (uint8_t *)arena_alloc(zero_cc_get_fn_arena(c), total_elems * esz);
                memset(buf, 0, total_elems * esz);
                ZCConstVal p = zc_cv_ptr(buf, esz, etag);
                for (size_t ei = 0; ei < ecnt; ei++) {
                    zc_cv_store(p, (int64_t)ei, elems[ei]);
                }
                if (is_arr_lit) return p;
                return zc_cv_deref(p, 0);
            }
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

/* Push a new block scope marker. */
static void ctfe_push_scope(ZeroCCCompiler *c) {
    if (c->ctfe_scope_count >= c->ctfe_scope_cap) {
        size_t newcap = c->ctfe_scope_cap ? c->ctfe_scope_cap * 2 : 16;
        size_t *ns = (size_t *)realloc(c->ctfe_scope_stack, newcap * sizeof(size_t));
        if (!ns) return;
        c->ctfe_scope_stack = ns;
        c->ctfe_scope_cap = newcap;
    }
    c->ctfe_scope_stack[c->ctfe_scope_count++] = c->ctfe_local_count;
}

/* Pop the most recent block scope, removing locals declared inside it. */
static void ctfe_pop_scope(ZeroCCCompiler *c) {
    if (c->ctfe_scope_count == 0) return;
    size_t mark = c->ctfe_scope_stack[--c->ctfe_scope_count];
    while (c->ctfe_local_count > mark) {
        c->ctfe_local_count--;
        free((void *)c->ctfe_local_names[c->ctfe_local_count]);
        if (c->ctfe_local_bufs && c->ctfe_local_bufs[c->ctfe_local_count]) {
            free(c->ctfe_local_bufs[c->ctfe_local_count]);
            c->ctfe_local_bufs[c->ctfe_local_count] = NULL;
        }
    }
}

/* Declare a local variable (or reassign an existing one in the current scope). */
static void ctfe_declare_local_full(ZeroCCCompiler *c, const char *name,
                                    int64_t val_i, double val_r, uint8_t kind,
                                    uint32_t elem_size, uint8_t elem_tag,
                                    uint8_t *buf) {
    /* Check for existing declaration in the current (innermost) scope. */
    size_t mark = (c->ctfe_scope_count > 0) ? c->ctfe_scope_stack[c->ctfe_scope_count - 1] : c->ctfe_local_base;
    for (size_t idx = mark; idx < c->ctfe_local_count; idx++) {
        if (strcmp(c->ctfe_local_names[idx], name) == 0) {
            c->ctfe_local_values_i[idx] = val_i;
            c->ctfe_local_values_r[idx] = val_r;
            c->ctfe_local_kinds[idx] = kind;
            if (c->ctfe_local_elem_sizes) c->ctfe_local_elem_sizes[idx] = elem_size;
            if (c->ctfe_local_elem_tags) c->ctfe_local_elem_tags[idx] = elem_tag;
            if (c->ctfe_local_bufs) {
                if (c->ctfe_local_bufs[idx]) free(c->ctfe_local_bufs[idx]);
                c->ctfe_local_bufs[idx] = buf;
            }
            return;
        }
    }
    /* New declaration. */
    if (c->ctfe_local_count >= c->ctfe_local_cap) {
        size_t newcap = c->ctfe_local_cap ? c->ctfe_local_cap * 2 : 16;
        char **nn = (char **)realloc(c->ctfe_local_names, newcap * sizeof(char *));
        int64_t *nvi = (int64_t *)realloc(c->ctfe_local_values_i, newcap * sizeof(int64_t));
        double *nvr = (double *)realloc(c->ctfe_local_values_r, newcap * sizeof(double));
        uint8_t *nvk = (uint8_t *)realloc(c->ctfe_local_kinds, newcap * sizeof(uint8_t));
        uint32_t *nes = (uint32_t *)realloc(c->ctfe_local_elem_sizes, newcap * sizeof(uint32_t));
        uint8_t *net = (uint8_t *)realloc(c->ctfe_local_elem_tags, newcap * sizeof(uint8_t));
        uint8_t **nb = (uint8_t **)realloc(c->ctfe_local_bufs, newcap * sizeof(uint8_t *));
        if (!nn || !nvi || !nvr || !nvk || !nes || !net || !nb) {
            free(buf);
            return;
        }
        c->ctfe_local_names = nn;
        c->ctfe_local_values_i = nvi;
        c->ctfe_local_values_r = nvr;
        c->ctfe_local_kinds = nvk;
        c->ctfe_local_elem_sizes = nes;
        c->ctfe_local_elem_tags = net;
        c->ctfe_local_bufs = nb;
        c->ctfe_local_cap = newcap;
    }
    size_t idx = c->ctfe_local_count++;
    c->ctfe_local_names[idx] = strdup(name);
    c->ctfe_local_values_i[idx] = val_i;
    c->ctfe_local_values_r[idx] = val_r;
    c->ctfe_local_kinds[idx] = kind;
    c->ctfe_local_elem_sizes[idx] = elem_size;
    c->ctfe_local_elem_tags[idx] = elem_tag;
    c->ctfe_local_bufs[idx] = buf;
}

/* Evaluate a declaration inside a constexpr function body, supporting
 * scalar variables, pointers, local arrays (`T arr[N] = { ... }`,
 * `char s[] = "..."`), and comma-separated declarators. */
static CtfResult ctfe_eval_decl(ZeroCCCompiler *c, size_t *pos, size_t end) {
    CtfResult r;
    r.kind = CTF_NORMAL;
    r.val = zc_cv_int(0);

    while (*pos < end && (c->tokens[*pos].type == TK_CONSTEXPR ||
                          c->tokens[*pos].type == TK_CONST ||
                          c->tokens[*pos].type == TK_VOLATILE ||
                          c->tokens[*pos].type == TK_RESTRICT)) {
        (*pos)++;
    }
    bool is_auto = false;
    ZeroCCType base_ty = zero_cc_type_int();
    if (*pos < end && c->tokens[*pos].type == TK_AUTO) {
        is_auto = true;
        (*pos)++;
    } else if (*pos < end && zero_cc_tok_is_type_start(c, *pos)) {
        base_ty = zero_cc_parse_type_name_at_pos(c, pos);
        /* Walk back over any trailing '*' consumed by parse_type_name_at_pos
         * so each comma-separated declarator parses its own pointer stars. */
        while (base_ty.tag == TYPE_PTR && base_ty.base &&
               *pos > 0 && c->tokens[*pos - 1].type == TK_STAR) {
            base_ty = *base_ty.base;
            (*pos)--;
        }
    } else {
        while (*pos < end && c->tokens[*pos].type != TK_IDENT &&
               c->tokens[*pos].type != TK_STAR &&
               c->tokens[*pos].type != TK_SEMI) {
            (*pos)++;
        }
    }

    while (*pos < end && c->tokens[*pos].type != TK_SEMI) {
        ZeroCCType decl_ty = base_ty;
        while (*pos < end && (c->tokens[*pos].type == TK_STAR ||
                              c->tokens[*pos].type == TK_CONST ||
                              c->tokens[*pos].type == TK_VOLATILE ||
                              c->tokens[*pos].type == TK_RESTRICT)) {
            if (c->tokens[*pos].type == TK_STAR) {
                decl_ty = zero_cc_type_ptr(&decl_ty);
            }
            (*pos)++;
        }
        if (*pos >= end || c->tokens[*pos].type != TK_IDENT) {
            while (*pos < end && c->tokens[*pos].type != TK_SEMI) (*pos)++;
            break;
        }
        const char *name = c->tokens[*pos].str_val;
        (*pos)++;

        bool is_array = false;
        int64_t array_len = -1;
        if (*pos < end && c->tokens[*pos].type == TK_LBRACKET) {
            is_array = true;
            (*pos)++; /* consume '[' */
            if (*pos < end && c->tokens[*pos].type != TK_RBRACKET) {
                size_t saved = c->token_pos;
                c->token_pos = *pos;
                ZCConstVal dim_v = parse_conditional(c, &c->token_pos);
                *pos = c->token_pos;
                c->token_pos = saved;
                array_len = dim_v.i;
            }
            if (*pos < end && c->tokens[*pos].type == TK_RBRACKET) (*pos)++;
        }

        if (is_array) {
            uint32_t esz = (decl_ty.size > 0) ? (uint32_t)decl_ty.size : 4;
            uint8_t etag = (uint8_t)decl_ty.tag;
            uint8_t *buf = NULL;
            if (*pos < end && c->tokens[*pos].type == TK_ASSIGN) {
                (*pos)++; /* consume '=' */
                if (*pos < end && c->tokens[*pos].type == TK_STRING && esz == 1) {
                    size_t saved = c->token_pos;
                    c->token_pos = *pos;
                    ZCConstVal sv = parse_primary(c, &c->token_pos);
                    *pos = c->token_pos;
                    c->token_pos = saved;
                    const char *str = sv.ptr ? (const char *)sv.ptr : "";
                    size_t slen = strlen(str) + 1;
                    size_t total_elems = (array_len > 0) ? (size_t)array_len : slen;
                    buf = (uint8_t *)calloc(total_elems > 0 ? total_elems : 1, 1);
                    size_t copy_n = (slen < total_elems) ? slen : total_elems;
                    memcpy(buf, str, copy_n);
                } else if (*pos < end && c->tokens[*pos].type == TK_LBRACE) {
                    (*pos)++; /* consume '{' */
                    ZCConstVal elems[256];
                    size_t ecnt = 0;
                    while (*pos < end && c->tokens[*pos].type != TK_RBRACE) {
                        size_t saved = c->token_pos;
                        c->token_pos = *pos;
                        ZCConstVal ev = parse_conditional(c, &c->token_pos);
                        *pos = c->token_pos;
                        c->token_pos = saved;
                        if (ecnt < 256) elems[ecnt++] = ev;
                        if (*pos < end && c->tokens[*pos].type == TK_COMMA) (*pos)++;
                    }
                    if (*pos < end && c->tokens[*pos].type == TK_RBRACE) (*pos)++;
                    size_t total_elems = (array_len > 0 && (size_t)array_len > ecnt)
                                         ? (size_t)array_len : (ecnt > 0 ? ecnt : 1);
                    buf = (uint8_t *)calloc(total_elems, esz);
                    ZCConstVal p = zc_cv_ptr(buf, esz, etag);
                    for (size_t ei = 0; ei < ecnt; ei++) {
                        zc_cv_store(p, (int64_t)ei, elems[ei]);
                    }
                }
            }
            if (!buf) {
                size_t total_elems = (array_len > 0) ? (size_t)array_len : 1;
                buf = (uint8_t *)calloc(total_elems, esz);
            }
            ctfe_declare_local_full(c, name, (int64_t)(uintptr_t)buf, 0.0,
                                    (uint8_t)ZC_CV_PTR, esz, etag, buf);
        } else {
            ZCConstVal init_val = (decl_ty.tag == TYPE_PTR)
                                  ? zc_cv_ptr(NULL,
                                              (decl_ty.base && decl_ty.base->size > 0) ? (uint32_t)decl_ty.base->size : 1,
                                              (decl_ty.base) ? (uint8_t)decl_ty.base->tag : TYPE_INT8)
                                  : zc_cv_int(0);
            if (*pos < end && c->tokens[*pos].type == TK_ASSIGN) {
                (*pos)++; /* consume '=' */
                bool braced = (*pos < end && c->tokens[*pos].type == TK_LBRACE);
                if (braced) (*pos)++;
                if (!braced || (*pos < end && c->tokens[*pos].type != TK_RBRACE)) {
                    size_t saved = c->token_pos;
                    c->token_pos = *pos;
                    init_val = parse_conditional(c, &c->token_pos);
                    *pos = c->token_pos;
                    c->token_pos = saved;
                }
                if (braced) {
                    if (*pos < end && c->tokens[*pos].type == TK_COMMA) (*pos)++;
                    if (*pos < end && c->tokens[*pos].type == TK_RBRACE) (*pos)++;
                }
                if (!is_auto) {
                    init_val = zc_cv_cast(init_val, decl_ty);
                }
            }
            uint32_t esz = (init_val.kind == ZC_CV_PTR)
                           ? init_val.ptr_elem_size
                           : ((decl_ty.size > 0) ? (uint32_t)decl_ty.size : 4);
            uint8_t etag = (init_val.kind == ZC_CV_PTR)
                           ? init_val.ptr_elem_tag
                           : (uint8_t)decl_ty.tag;
            ctfe_declare_local_full(c, name, init_val.i, init_val.r,
                                    (uint8_t)init_val.kind, esz, etag, NULL);
        }

        if (*pos < end && c->tokens[*pos].type == TK_COMMA) {
            (*pos)++;
            continue;
        }
        break;
    }

    if (*pos < end && c->tokens[*pos].type == TK_SEMI) {
        (*pos)++;
    }
    return r;
}

/* Evaluate an expression statement: `expr;`, supporting comma-separated expressions. */
static CtfResult ctfe_eval_expr_stmt(ZeroCCCompiler *c, size_t *pos, size_t end) {
    CtfResult r;
    r.kind = CTF_NORMAL;
    r.val = zc_cv_int(0);
    if (*pos < end && c->tokens[*pos].type == TK_SEMI) {
        (*pos)++;
        return r;
    }
    size_t saved = c->token_pos;
    c->token_pos = *pos;
    (void)parse_conditional(c, &c->token_pos);
    while (c->token_pos < end && c->tokens[c->token_pos].type == TK_COMMA) {
        c->token_pos++;
        (void)parse_conditional(c, &c->token_pos);
    }
    *pos = c->token_pos;
    c->token_pos = saved;
    if (*pos < end && c->tokens[*pos].type == TK_SEMI) (*pos)++;
    return r;
}

/* Evaluate a block: `{ stmt* }`.  Assumes current token is '{'. */
static CtfResult ctfe_eval_block(ZeroCCCompiler *c, size_t *pos, size_t end) {
    CtfResult r;
    r.kind = CTF_NORMAL;
    r.val = zc_cv_int(0);

    (*pos)++; /* consume '{' */
    ctfe_push_scope(c);

    while (*pos < end && c->tokens[*pos].type != TK_RBRACE) {
        size_t prev_pos = *pos;
        r = ctfe_eval_stmt(c, pos, end);
        if (r.kind != CTF_NORMAL) {
            /* Advance *pos to the matching '}' of this block so callers
             * (e.g. ctfe_eval_while on break/continue) stay synchronized. */
            int depth = 1;
            while (*pos < end && depth > 0) {
                if (c->tokens[*pos].type == TK_LBRACE) depth++;
                else if (c->tokens[*pos].type == TK_RBRACE) {
                    depth--;
                    if (depth == 0) break;
                }
                (*pos)++;
            }
            break;
        }
        if (*pos == prev_pos) (*pos)++;
    }

    ctfe_pop_scope(c);

    if (*pos < end && c->tokens[*pos].type == TK_RBRACE) (*pos)++; /* consume '}' */
    return r;
}

/* Evaluate an if/else statement. */
static CtfResult ctfe_eval_if(ZeroCCCompiler *c, size_t *pos, size_t end) {
    CtfResult r;
    r.kind = CTF_NORMAL;
    r.val = zc_cv_int(0);

    (*pos)++; /* consume 'if' */
    if (*pos < end && c->tokens[*pos].type == TK_LPAREN) (*pos)++;
    size_t saved = c->token_pos;
    c->token_pos = *pos;
    ZCConstVal cond = parse_conditional(c, &c->token_pos);
    *pos = c->token_pos;
    c->token_pos = saved;
    if (*pos < end && c->tokens[*pos].type == TK_RPAREN) (*pos)++;

    if (zc_cv_is_true(cond)) {
        /* Then branch - condition true, evaluate it. */
        if (*pos < end && c->tokens[*pos].type == TK_LBRACE) {
            r = ctfe_eval_block(c, pos, end);
        } else {
            r = ctfe_eval_stmt(c, pos, end);
        }
        /* Skip else branch if present so outer block continues after if/else. */
        if (r.kind == CTF_NORMAL && *pos < end && c->tokens[*pos].type == TK_ELSE) {
            (*pos)++; /* consume 'else' */
            if (*pos < end && c->tokens[*pos].type == TK_LBRACE) {
                int depth = 1;
                (*pos)++;
                while (*pos < end && depth > 0) {
                    if (c->tokens[*pos].type == TK_LBRACE) depth++;
                    else if (c->tokens[*pos].type == TK_RBRACE) depth--;
                    (*pos)++;
                }
            } else {
                while (*pos < end &&
                       c->tokens[*pos].type != TK_SEMI &&
                       c->tokens[*pos].type != TK_RBRACE) {
                    if (c->tokens[*pos].type == TK_LPAREN ||
                        c->tokens[*pos].type == TK_LBRACE) {
                        int depth = 1;
                        (*pos)++;
                        while (*pos < end && depth > 0) {
                            if (c->tokens[*pos].type == TK_LPAREN ||
                                c->tokens[*pos].type == TK_LBRACE) depth++;
                            else if (c->tokens[*pos].type == TK_RPAREN ||
                                     c->tokens[*pos].type == TK_RBRACE) depth--;
                            (*pos)++;
                        }
                    } else {
                        (*pos)++;
                    }
                }
                if (*pos < end && c->tokens[*pos].type == TK_SEMI) (*pos)++;
            }
        }
    } else {
        /* Condition false: skip the then-branch without evaluating it.
         * We must still consume its tokens so the parser stays in sync. */
        if (*pos < end && c->tokens[*pos].type == TK_LBRACE) {
            /* Skip balanced braces. */
            int depth = 1;
            (*pos)++;
            while (*pos < end && depth > 0) {
                if (c->tokens[*pos].type == TK_LBRACE) depth++;
                else if (c->tokens[*pos].type == TK_RBRACE) depth--;
                (*pos)++;
            }
        } else {
            /* Skip a single statement: consume tokens until ';' or '}'. */
            while (*pos < end &&
                   c->tokens[*pos].type != TK_SEMI &&
                   c->tokens[*pos].type != TK_RBRACE &&
                   c->tokens[*pos].type != TK_ELSE) {
                /* Skip nested parens/braces. */
                if (c->tokens[*pos].type == TK_LPAREN ||
                    c->tokens[*pos].type == TK_LBRACE) {
                    int depth = 1;
                    (*pos)++;
                    while (*pos < end && depth > 0) {
                        if (c->tokens[*pos].type == TK_LPAREN ||
                            c->tokens[*pos].type == TK_LBRACE) depth++;
                        else if (c->tokens[*pos].type == TK_RPAREN ||
                                 c->tokens[*pos].type == TK_RBRACE) depth--;
                        (*pos)++;
                    }
                } else {
                    (*pos)++;
                }
            }
            if (*pos < end && c->tokens[*pos].type == TK_SEMI) (*pos)++;
        }
        /* Then check for else. */
        if (*pos < end && c->tokens[*pos].type == TK_ELSE) {
            (*pos)++; /* consume 'else' */
            if (*pos < end && c->tokens[*pos].type == TK_LBRACE) {
                r = ctfe_eval_block(c, pos, end);
            } else {
                r = ctfe_eval_stmt(c, pos, end);
            }
        }
    }
    return r;
}

/* Evaluate a while loop. */
static CtfResult ctfe_eval_while(ZeroCCCompiler *c, size_t *pos, size_t end) {
    CtfResult r;
    r.kind = CTF_NORMAL;
    r.val = zc_cv_int(0);

    (*pos)++; /* consume 'while' */
    if (*pos < end && c->tokens[*pos].type == TK_LPAREN) (*pos)++;

    /* Guard against infinite loops in CTFE. */
    int iterations = 0;
    const int max_iter = 100000;

    while (*pos < end) {
        /* Save position before condition. */
        size_t cond_start = *pos;
        size_t saved = c->token_pos;
        c->token_pos = *pos;
        ZCConstVal cond = parse_conditional(c, &c->token_pos);
        *pos = c->token_pos;
        c->token_pos = saved;
        if (*pos < end && c->tokens[*pos].type == TK_RPAREN) (*pos)++;

        if (!zc_cv_is_true(cond)) {
            /* Condition false: skip the while body without evaluating it,
             * to keep the token stream in sync. */
            if (*pos < end && c->tokens[*pos].type == TK_LBRACE) {
                int depth = 1;
                (*pos)++;
                while (*pos < end && depth > 0) {
                    if (c->tokens[*pos].type == TK_LBRACE) depth++;
                    else if (c->tokens[*pos].type == TK_RBRACE) depth--;
                    (*pos)++;
                }
            } else {
                while (*pos < end &&
                       c->tokens[*pos].type != TK_SEMI &&
                       c->tokens[*pos].type != TK_RBRACE) {
                    if (c->tokens[*pos].type == TK_LPAREN ||
                        c->tokens[*pos].type == TK_LBRACE) {
                        int depth = 1;
                        (*pos)++;
                        while (*pos < end && depth > 0) {
                            if (c->tokens[*pos].type == TK_LPAREN ||
                                c->tokens[*pos].type == TK_LBRACE) depth++;
                            else if (c->tokens[*pos].type == TK_RPAREN ||
                                     c->tokens[*pos].type == TK_RBRACE) depth--;
                            (*pos)++;
                        }
                    } else {
                        (*pos)++;
                    }
                }
                if (*pos < end && c->tokens[*pos].type == TK_SEMI) (*pos)++;
            }
            break;
        }

        /* Evaluate body. */
        if (*pos < end && c->tokens[*pos].type == TK_LBRACE) {
            r = ctfe_eval_block(c, pos, end);
        } else {
            r = ctfe_eval_stmt(c, pos, end);
        }
        if (r.kind == CTF_BREAK) { r.kind = CTF_NORMAL; break; }
        if (r.kind == CTF_RETURN) break;
        if (r.kind == CTF_CONTINUE) r.kind = CTF_NORMAL;

        /* Loop back: re-evaluate condition from cond_start. */
        *pos = cond_start;
        if (++iterations > max_iter) {
            /* Bail out to avoid infinite CTFE loop. */
            r.kind = CTF_NORMAL;
            break;
        }
    }
    return r;
}

/* Helper to advance past a for-loop body starting at `bstart`. */
static size_t ctfe_skip_for_body(ZeroCCCompiler *c, size_t bstart, size_t end) {
    if (bstart < end && c->tokens[bstart].type == TK_LBRACE) {
        int depth2 = 1;
        bstart++;
        while (bstart < end && depth2 > 0) {
            if (c->tokens[bstart].type == TK_LBRACE) depth2++;
            else if (c->tokens[bstart].type == TK_RBRACE) depth2--;
            bstart++;
        }
    } else {
        while (bstart < end &&
               c->tokens[bstart].type != TK_SEMI &&
               c->tokens[bstart].type != TK_RBRACE) {
            if (c->tokens[bstart].type == TK_LPAREN ||
                c->tokens[bstart].type == TK_LBRACE) {
                int depth2 = 1;
                bstart++;
                while (bstart < end && depth2 > 0) {
                    if (c->tokens[bstart].type == TK_LPAREN ||
                        c->tokens[bstart].type == TK_LBRACE) depth2++;
                    else if (c->tokens[bstart].type == TK_RPAREN ||
                             c->tokens[bstart].type == TK_RBRACE) depth2--;
                    bstart++;
                }
            } else {
                bstart++;
            }
        }
        if (bstart < end && c->tokens[bstart].type == TK_SEMI) bstart++;
    }
    return bstart;
}

/* Evaluate a for loop: `for (init; cond; incr) body`. */
static CtfResult ctfe_eval_for(ZeroCCCompiler *c, size_t *pos, size_t end) {
    CtfResult r;
    r.kind = CTF_NORMAL;
    r.val = zc_cv_int(0);

    (*pos)++; /* consume 'for' */
    if (*pos < end && c->tokens[*pos].type == TK_LPAREN) (*pos)++;

    ctfe_push_scope(c);

    /* Init (a declaration or expression). */
    if (*pos < end && c->tokens[*pos].type != TK_SEMI) {
        ZeroCCTokType it = c->tokens[*pos].type;
        if (zero_cc_tok_is_type_start(c, *pos) || it == TK_AUTO || it == TK_CONSTEXPR) {
            r = ctfe_eval_decl(c, pos, end);
        } else {
            r = ctfe_eval_expr_stmt(c, pos, end);
        }
        if (r.kind != CTF_NORMAL) {
            ctfe_pop_scope(c);
            return r;
        }
    } else if (*pos < end && c->tokens[*pos].type == TK_SEMI) {
        (*pos)++;
    }

    /* Condition start position. */
    size_t cond_start = *pos;

    /* Find increment start and closing ')'. */
    size_t incr_start = cond_start;
    int depth = 0;
    for (size_t scan = cond_start; scan < end; scan++) {
        ZeroCCTokType tt = c->tokens[scan].type;
        if (tt == TK_LPAREN) depth++;
        else if (tt == TK_RPAREN) { if (depth > 0) depth--; }
        else if (tt == TK_SEMI && depth == 0) {
            incr_start = scan + 1;
            break;
        }
    }

    size_t body_pos = cond_start;
    depth = 0;
    while (body_pos < end) {
        ZeroCCTokType tt = c->tokens[body_pos].type;
        if (tt == TK_LPAREN) depth++;
        else if (tt == TK_RPAREN) {
            if (depth == 0) break;
            depth--;
        }
        body_pos++;
    }
    size_t bstart = body_pos + 1;

    int iterations = 0;
    const int max_iter = 100000;

    for (;;) {
        /* Evaluate condition from cond_start. */
        if (cond_start < end && c->tokens[cond_start].type == TK_SEMI) {
            /* Empty condition = true. */
        } else {
            size_t saved = c->token_pos;
            c->token_pos = cond_start;
            ZCConstVal cond = parse_conditional(c, &c->token_pos);
            c->token_pos = saved;
            if (!zc_cv_is_true(cond)) {
                *pos = ctfe_skip_for_body(c, bstart, end);
                break;
            }
        }

        /* Evaluate body. */
        size_t bp = bstart;
        if (bstart < end && c->tokens[bstart].type == TK_LBRACE) {
            r = ctfe_eval_block(c, &bp, end);
        } else {
            r = ctfe_eval_stmt(c, &bp, end);
        }
        if (r.kind == CTF_BREAK) {
            r.kind = CTF_NORMAL;
            *pos = ctfe_skip_for_body(c, bstart, end);
            break;
        }
        if (r.kind == CTF_RETURN) break;
        if (r.kind == CTF_CONTINUE) r.kind = CTF_NORMAL;

        /* Evaluate increment from incr_start (handles `i++`, `i += 1`,
         * `p++`, `i = i + 1`, and comma-separated increments `i++, j--`). */
        if (incr_start < end && incr_start < body_pos &&
            c->tokens[incr_start].type != TK_RPAREN) {
            size_t saved = c->token_pos;
            c->token_pos = incr_start;
            (void)parse_conditional(c, &c->token_pos);
            while (c->token_pos < body_pos && c->tokens[c->token_pos].type == TK_COMMA) {
                c->token_pos++;
                (void)parse_conditional(c, &c->token_pos);
            }
            c->token_pos = saved;
        }

        if (++iterations > max_iter) {
            r.kind = CTF_NORMAL;
            *pos = ctfe_skip_for_body(c, bstart, end);
            break;
        }
    }

    ctfe_pop_scope(c);
    return r;
}

/* Evaluate a single statement during CTFE. */
static CtfResult ctfe_eval_stmt(ZeroCCCompiler *c, size_t *pos, size_t end) {
    CtfResult r;
    r.kind = CTF_NORMAL;
    r.val = zc_cv_int(0);

    if (*pos >= end) return r;

    ZeroCCTokType t = c->tokens[*pos].type;

    if (t == TK_LBRACE) {
        return ctfe_eval_block(c, pos, end);
    }
    if (t == TK_IF) {
        return ctfe_eval_if(c, pos, end);
    }
    if (t == TK_WHILE) {
        return ctfe_eval_while(c, pos, end);
    }
    if (t == TK_FOR) {
        return ctfe_eval_for(c, pos, end);
    }
    if (t == TK_RETURN) {
        (*pos)++; /* consume 'return' */
        if (*pos < end && c->tokens[*pos].type == TK_SEMI) {
            (*pos)++;
            r.kind = CTF_RETURN;
            return r;
        }
        size_t saved = c->token_pos;
        c->token_pos = *pos;
        r.val = parse_conditional(c, &c->token_pos);
        *pos = c->token_pos;
        c->token_pos = saved;
        if (*pos < end && c->tokens[*pos].type == TK_SEMI) (*pos)++;
        r.kind = CTF_RETURN;
        return r;
    }
    if (t == TK_RPAREN) {
        /* Closing paren of for(…): skip it and continue. */
        (*pos)++;
        r.kind = CTF_NORMAL;
        return r;
    }
    if (t == TK_BREAK) {
        (*pos)++;
        if (*pos < end && c->tokens[*pos].type == TK_SEMI) (*pos)++;
        r.kind = CTF_BREAK;
        return r;
    }
    if (t == TK_CONTINUE) {
        (*pos)++;
        if (*pos < end && c->tokens[*pos].type == TK_SEMI) (*pos)++;
        r.kind = CTF_CONTINUE;
        return r;
    }
    /* Declaration: type specifier / qualifier / auto / constexpr. */
    if (zero_cc_tok_is_type_start(c, *pos) || t == TK_AUTO || t == TK_CONSTEXPR) {
        return ctfe_eval_decl(c, pos, end);
    }

    /* Expression statement (handles assignments, compound assignments,
     * pre/post inc/dec, pointer stores `*p = ...`, `arr[i] = ...`, etc.). */
    return ctfe_eval_expr_stmt(c, pos, end);
}

/* Evaluate a constexpr function body between [start, end).  Returns the
 * value of the return statement, or 0 if no return was reached. */
ZCConstVal ctfe_eval_body(ZeroCCCompiler *c, size_t start, size_t end) {
    size_t pos = start;
    /* Skip opening '{' if present. */
    if (pos < end && c->tokens[pos].type == TK_LBRACE) pos++;

    ctfe_push_scope(c);
    ZCConstVal result = zc_cv_int(0);
    while (pos < end) {
        if (c->tokens[pos].type == TK_RBRACE) {
            pos++;
            break;
        }
        size_t prev_pos = pos;
        CtfResult r = ctfe_eval_stmt(c, &pos, end);
        if (r.kind == CTF_RETURN) {
            result = r.val;
            break;
        }
        if (pos == prev_pos) pos++;
    }
    ctfe_pop_scope(c);
    return result;
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
            bool call_like = (p + 1 < c->token_count &&
                              c->tokens[p + 1].type == TK_LPAREN);
            if (call_like) {
                ZeroCCFunction *fn = zero_cc_find_function(c, nm);
                if (fn && fn->is_constexpr) {
                    p++;
                    continue;
                }
                return false;
            }
            ZeroCCGlobalVar *gv = find_global_var(c, nm);
            bool known = (strcmp(nm, "true") == 0 || strcmp(nm, "false") == 0 ||
                          strcmp(nm, "NULL") == 0 || strcmp(nm, "nullptr") == 0 ||
                          strcmp(nm, "__alignof__") == 0 ||
                          zero_cc_find_enum_const(c, nm) != NULL ||
                          zc_const_global_scalar(c, nm, NULL, NULL, NULL) ||
                          (gv && (gv->type.tag == TYPE_ARRAY || gv->type.tag == TYPE_PTR)));
            if (!known) return false;
        }
        p++;
    }
    return true;
}




