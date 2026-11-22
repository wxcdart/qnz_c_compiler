/* Freestanding SIMD vendor intrinsics regression test (<immintrin.h> & <arm_neon.h>).
 * Tests SSE/SSE2/SSE3/SSE4.1/FMA and ARM NEON intrinsics in both JIT and AOT
 * modes across -O0..-O3 without stdio dependency (compatible with make test-san & asan).
 */

#include <immintrin.h>
#include <stdint.h>

#ifdef __zerocc__
#include <arm_neon.h>
#endif

static int near_f32(float a, float b) {
    float d = a - b;
    if (d < 0.0f) d = -d;
    return d < 1e-4f;
}

static int near_f64(double a, double b) {
    double d = a - b;
    if (d < 0.0) d = -d;
    return d < 1e-9;
}

static int test_sse(void) {
    float fa[4] = {1.5f, -2.0f, 3.25f, 4.0f};
    float fb[4] = {2.5f, 5.0f, -1.25f, 2.0f};
    float fc[4] = {0.0f, 0.0f, 0.0f, 0.0f};

    __m128 va = _mm_loadu_ps(fa);
    __m128 vb = _mm_loadu_ps(fb);
    __m128 vc = _mm_add_ps(va, vb);
    _mm_storeu_ps(fc, vc);
    if (!near_f32(fc[0], 4.0f) || !near_f32(fc[1], 3.0f) ||
        !near_f32(fc[2], 2.0f) || !near_f32(fc[3], 6.0f)) return 1;

    __m128 vd = _mm_sub_ps(va, vb);
    _mm_storeu_ps(fc, vd);
    if (!near_f32(fc[0], -1.0f) || !near_f32(fc[1], -7.0f) ||
        !near_f32(fc[2], 4.5f) || !near_f32(fc[3], 2.0f)) return 2;

    __m128 vp = _mm_mul_ps(va, _mm_set1_ps(2.0f));
    __m128 vq = _mm_div_ps(vp, _mm_setr_ps(1.0f, 2.0f, 0.5f, 4.0f));
    _mm_storeu_ps(fc, vq);
    if (!near_f32(fc[0], 3.0f) || !near_f32(fc[1], -2.0f) ||
        !near_f32(fc[2], 13.0f) || !near_f32(fc[3], 2.0f)) return 3;

    /* Min/max including negative floats and scalar lane ops */
    __m128 vmin = _mm_min_ps(va, vb);
    __m128 vmax = _mm_max_ps(va, vb);
    float fmin[4], fmax[4];
    _mm_storeu_ps(fmin, vmin);
    _mm_storeu_ps(fmax, vmax);
    if (!near_f32(fmin[0], 1.5f) || !near_f32(fmin[1], -2.0f) ||
        !near_f32(fmin[2], -1.25f) || !near_f32(fmin[3], 2.0f)) return 4;
    if (!near_f32(fmax[0], 2.5f) || !near_f32(fmax[1], 5.0f) ||
        !near_f32(fmax[2], 3.25f) || !near_f32(fmax[3], 4.0f)) return 5;

    __m128 vss = _mm_add_ss(va, _mm_set_ss(10.0f));
    if (!near_f32(_mm_cvtss_f32(vss), 11.5f)) return 6;
    _mm_storeu_ps(fc, vss);
    if (!near_f32(fc[1], -2.0f) || !near_f32(fc[3], 4.0f)) return 7;

    /* Packed double */
    double da[2] = {3.5, -7.25};
    double db[2] = {1.5, 2.25};
    double dc[2];
    __m128d d1 = _mm_loadu_pd(da);
    __m128d d2 = _mm_loadu_pd(db);
    _mm_storeu_pd(dc, _mm_add_pd(d1, d2));
    if (!near_f64(dc[0], 5.0) || !near_f64(dc[1], -5.0)) return 8;
    _mm_storeu_pd(dc, _mm_min_pd(d1, d2));
    if (!near_f64(dc[0], 1.5) || !near_f64(dc[1], -7.25)) return 9;
    _mm_storeu_pd(dc, _mm_max_pd(d1, d2));
    if (!near_f64(dc[0], 3.5) || !near_f64(dc[1], 2.25)) return 10;

    /* Integer ops, shifts, bitwise, comparisons, movemask, shuffle */
    __m128i ia = _mm_setr_epi32(-10, 20, -30, 40);
    __m128i ib = _mm_setr_epi32(3, 5, -10, 8);
    __m128i isum = _mm_add_epi32(ia, ib);
    __m128i idif = _mm_sub_epi32(ia, ib);
    int out_i[4];
    _mm_storeu_si128((__m128i *)out_i, isum);
    if (out_i[0] != -7 || out_i[1] != 25 || out_i[2] != -40 || out_i[3] != 48) return 11;
    _mm_storeu_si128((__m128i *)out_i, idif);
    if (out_i[0] != -13 || out_i[1] != 15 || out_i[2] != -20 || out_i[3] != 32) return 12;

    __m128i ishl = _mm_slli_epi32(_mm_setr_epi32(1, 2, 3, 4), 3);
    __m128i ishr = _mm_srli_epi32(_mm_setr_epi32(16, 32, 64, 128), 2);
    __m128i isar = _mm_srai_epi32(_mm_setr_epi32(-16, 16, -8, 8), 2);
    if (_mm_cvtsi128_si32(ishl) != 8) return 13;
    _mm_storeu_si128((__m128i *)out_i, ishr);
    if (out_i[0] != 4 || out_i[3] != 32) return 14;
    _mm_storeu_si128((__m128i *)out_i, isar);
    if (out_i[0] != -4 || out_i[1] != 4 || out_i[2] != -2 || out_i[3] != 2) return 15;

    __m128 mcmp = _mm_cmplt_ps(_mm_setr_ps(1.0f, 5.0f, -2.0f, 3.0f),
                               _mm_setr_ps(2.0f, 4.0f, -1.0f, 3.0f));
    if (_mm_movemask_ps(mcmp) != (1 | 4)) return 16;

    __m128 sh = _mm_shuffle_ps(_mm_setr_ps(10.0f, 20.0f, 30.0f, 40.0f),
                               _mm_setr_ps(50.0f, 60.0f, 70.0f, 80.0f),
                               (3 << 6) | (0 << 4) | (2 << 2) | 1);
    _mm_storeu_ps(fc, sh);
    if (!near_f32(fc[0], 20.0f) || !near_f32(fc[1], 30.0f) ||
        !near_f32(fc[2], 50.0f) || !near_f32(fc[3], 80.0f)) return 17;

    __m128 mhl = _mm_movehl_ps(_mm_setr_ps(1.0f, 2.0f, 3.0f, 4.0f),
                               _mm_setr_ps(5.0f, 6.0f, 7.0f, 8.0f));
    _mm_storeu_ps(fc, mhl);
    if (!near_f32(fc[0], 7.0f) || !near_f32(fc[1], 8.0f) ||
        !near_f32(fc[2], 3.0f) || !near_f32(fc[3], 4.0f)) return 18;

    __m128i bnot = _mm_andnot_si128(_mm_set1_epi32(0x0F), _mm_set1_epi32(0xFF));
    if (_mm_cvtsi128_si32(bnot) != 0xF0) return 19;

    __m128i i64 = _mm_add_epi64(_mm_set_epi64x(100LL, 200LL), _mm_set1_epi64x(50LL));
    if (_mm_cvtsi128_si64(i64) != 250LL) return 20;

#ifdef __zerocc__
    /* SSE3 / SSSE3 / SSE4.1 / FMA intrinsics supported in virtual header */
    __m128i mul32 = _mm_mullo_epi32(_mm_setr_epi32(2, -3, 4, -5),
                                    _mm_setr_epi32(10, 7, -6, -8));
    if (_mm_extract_epi32(mul32, 0) != 20 || _mm_extract_epi32(mul32, 1) != -21 ||
        _mm_extract_epi32(mul32, 2) != -24 || _mm_extract_epi32(mul32, 3) != 40) return 21;

    __m128i abs32 = _mm_abs_epi32(_mm_setr_epi32(-42, 0, 17, -99));
    if (_mm_extract_epi32(abs32, 0) != 42 || _mm_extract_epi32(abs32, 1) != 0 ||
        _mm_extract_epi32(abs32, 2) != 17 || _mm_extract_epi32(abs32, 3) != 99) return 22;

    __m128i imin = _mm_min_epi32(_mm_setr_epi32(-5, 10, -20, 30),
                                 _mm_setr_epi32(-10, 5, 20, -30));
    __m128i imax = _mm_max_epi32(_mm_setr_epi32(-5, 10, -20, 30),
                                 _mm_setr_epi32(-10, 5, 20, -30));
    if (_mm_extract_epi32(imin, 0) != -10 || _mm_extract_epi32(imin, 1) != 5 ||
        _mm_extract_epi32(imax, 0) != -5 || _mm_extract_epi32(imax, 2) != 20) return 23;

    __m128i umin = _mm_min_epu32(_mm_setr_epi32((int)0x80000001u, 10, 3, 99),
                                 _mm_setr_epi32(5, 20, 4, 88));
    __m128i umax = _mm_max_epu32(_mm_setr_epi32((int)0x80000001u, 10, 3, 99),
                                 _mm_setr_epi32(5, 20, 4, 88));
    if (_mm_extract_epi32(umin, 0) != 5 ||
        (uint32_t)_mm_extract_epi32(umax, 0) != 0x80000001u) return 24;

    __m128i ins = _mm_insert_epi32(_mm_setr_epi32(1, 2, 3, 4), 777, 2);
    if (_mm_extract_epi32(ins, 2) != 777 || _mm_extract_epi32(ins, 3) != 4) return 25;

    __m128 fma = _mm_fmadd_ps(_mm_set1_ps(2.0f), _mm_setr_ps(1.0f, 2.0f, 3.0f, 4.0f),
                              _mm_setr_ps(10.0f, 20.0f, 30.0f, 40.0f));
    __m128 fms = _mm_fmsub_ps(_mm_set1_ps(3.0f), _mm_setr_ps(2.0f, 4.0f, 6.0f, 8.0f),
                              _mm_set1_ps(1.0f));
    _mm_storeu_ps(fc, fma);
    if (!near_f32(fc[0], 12.0f) || !near_f32(fc[3], 48.0f)) return 26;
    _mm_storeu_ps(fc, fms);
    if (!near_f32(fc[0], 5.0f) || !near_f32(fc[2], 17.0f)) return 27;

    __m128 h = _mm_hadd_ps(_mm_setr_ps(1.0f, 2.0f, 3.0f, 4.0f),
                           _mm_setr_ps(5.0f, 6.0f, 7.0f, 8.0f));
    _mm_storeu_ps(fc, h);
    if (!near_f32(fc[0], 3.0f) || !near_f32(fc[1], 7.0f) ||
        !near_f32(fc[2], 11.0f) || !near_f32(fc[3], 15.0f)) return 28;

    __m128 cvtf = _mm_cvtepi32_ps(_mm_setr_epi32(-3, 4, 5, 6));
    __m128i cvti = _mm_cvttps_epi32(_mm_setr_ps(-3.9f, 4.9f, 5.1f, -6.7f));
    if (!near_f32(_mm_cvtss_f32(cvtf), -3.0f)) return 29;
    if (_mm_extract_epi32(cvti, 0) != -3 || _mm_extract_epi32(cvti, 1) != 4 ||
        _mm_extract_epi32(cvti, 3) != -6) return 30;
#endif

    return 0;
}

#ifdef __zerocc__
static int test_neon(void) {
    float32_t fa[4] = {1.0f, -4.5f, 2.5f, 8.0f};
    float32_t fb[4] = {3.0f, 1.5f, -0.5f, 2.0f};
    float32_t fc[4] = {0.0f, 0.0f, 0.0f, 0.0f};

    float32x4_t va = vld1q_f32(fa);
    float32x4_t vb = vld1q_f32(fb);
    float32x4_t vsum = vaddq_f32(va, vb);
    float32x4_t vdif = vsubq_f32(va, vb);
    float32x4_t vmul = vmulq_f32(va, vb);
    float32x4_t vdiv = vdivq_f32(va, vb);

    if (!near_f32(vgetq_lane_f32(vsum, 0), 4.0f) ||
        !near_f32(vgetq_lane_f32(vsum, 1), -3.0f)) return 50;
    if (!near_f32(vgetq_lane_f32(vdif, 2), 3.0f) ||
        !near_f32(vgetq_lane_f32(vdif, 3), 6.0f)) return 51;
    if (!near_f32(vgetq_lane_f32(vmul, 0), 3.0f) ||
        !near_f32(vgetq_lane_f32(vmul, 3), 16.0f)) return 52;
    if (!near_f32(vgetq_lane_f32(vdiv, 3), 4.0f)) return 53;

    /* FMA / MLA / MLS (NEON order: acc + b*c, acc - b*c) */
    float32x4_t acc = vdupq_n_f32(10.0f);
    float32x4_t vmla = vmlaq_f32(acc, va, vdupq_n_f32(2.0f));
    float32x4_t vmls = vmlsq_f32(acc, vb, vdupq_n_f32(2.0f));
    vst1q_f32(fc, vmla);
    if (!near_f32(fc[0], 12.0f) || !near_f32(fc[1], 1.0f) || !near_f32(fc[3], 26.0f)) return 54;
    vst1q_f32(fc, vmls);
    if (!near_f32(fc[0], 4.0f) || !near_f32(fc[2], 11.0f) || !near_f32(fc[3], 6.0f)) return 55;

    /* Float min/max/abs/neg + reduction + lane set */
    float32x4_t vmin = vminq_f32(va, vb);
    float32x4_t vmax = vmaxq_f32(va, vb);
    float32x4_t vab = vabsq_f32(va);
    if (!near_f32(vgetq_lane_f32(vmin, 1), -4.5f) ||
        !near_f32(vgetq_lane_f32(vmax, 1), 1.5f)) return 56;
    if (!near_f32(vgetq_lane_f32(vab, 1), 4.5f) ||
        !near_f32(vgetq_lane_f32(vab, 0), 1.0f)) return 57;

    float32x4_t vset = vsetq_lane_f32(99.5f, vab, 2);
    if (!near_f32(vgetq_lane_f32(vset, 2), 99.5f) ||
        !near_f32(vgetq_lane_f32(vset, 1), 4.5f)) return 58;
    if (!near_f32(vaddvq_f32(vdupq_n_f32(2.25f)), 9.0f)) return 59;

    /* Integer NEON ops, bic, bsl, comparisons, shifts, conversions, ext, zip, reinterpret */
    int32_t s_a[4] = {-10, 20, -30, 40};
    int32_t s_b[4] = {3, 4, 5, 6};
    int32x4_t isa = vld1q_s32(s_a);
    int32x4_t isb = vld1q_s32(s_b);
    int32x4_t isum = vaddq_s32(isa, isb);
    int32x4_t imla = vmlaq_s32(vdupq_n_s32(100), isa, isb);
    int32x4_t imls = vmlsq_s32(vdupq_n_s32(100), isb, vdupq_n_s32(5));
    if (vaddvq_s32(isum) != ( -7 + 24 + -25 + 46 )) return 60;
    if (vgetq_lane_s32(imla, 0) != 70 || vgetq_lane_s32(imls, 1) != 80) return 61;

    int32x4_t sabs = vabsq_s32(isa);
    if (vgetq_lane_s32(sabs, 0) != 10 || vgetq_lane_s32(sabs, 2) != 30) return 62;

    uint32x4_t ucmp = vcgtq_s32(isa, vdupq_n_s32(0));
    int32x4_t sel = vbslq_s32(ucmp, isa, vdupq_n_s32(999));
    if (vgetq_lane_s32(sel, 0) != 999 || vgetq_lane_s32(sel, 1) != 20 ||
        vgetq_lane_s32(sel, 2) != 999 || vgetq_lane_s32(sel, 3) != 40) return 63;

    uint32x4_t bic = vbicq_u32(vdupq_n_u32(0xFFu), vdupq_n_u32(0x0Fu));
    if (vgetq_lane_u32(bic, 0) != 0xF0u) return 64;

    int32x4_t shl = vshlq_n_s32(isb, 2);
    int32x4_t shr = vshrq_n_s32(isa, 1);
    if (vgetq_lane_s32(shl, 0) != 12 || vgetq_lane_s32(shr, 0) != -5) return 65;

    int32x4_t ext = vextq_s32(isa, isb, 2);
    if (vgetq_lane_s32(ext, 0) != -30 || vgetq_lane_s32(ext, 1) != 40 ||
        vgetq_lane_s32(ext, 2) != 3 || vgetq_lane_s32(ext, 3) != 4) return 66;

    float32x4_t z1 = vzip1q_f32(va, vb);
    if (!near_f32(vgetq_lane_f32(z1, 0), 1.0f) || !near_f32(vgetq_lane_f32(z1, 1), 3.0f) ||
        !near_f32(vgetq_lane_f32(z1, 2), -4.5f) || !near_f32(vgetq_lane_f32(z1, 3), 1.5f)) return 67;

    int32x4_t bits = vreinterpretq_s32_f32(vdupq_n_f32(1.0f));
    if (vgetq_lane_s32(bits, 0) != 0x3F800000) return 68;

    float64_t da[2] = {2.5, -6.5};
    float64_t db[2] = {1.5, 2.5};
    float64x2_t d1 = vld1q_f64(da);
    float64x2_t d2 = vld1q_f64(db);
    float64x2_t dsum = vaddq_f64(d1, d2);
    float64x2_t dmin = vminq_f64(d1, d2);
    float64x2_t dmax = vmaxq_f64(d1, d2);
    if (!near_f64(vgetq_lane_f64(dsum, 0), 4.0) ||
        !near_f64(vgetq_lane_f64(dsum, 1), -4.0)) return 69;
    if (!near_f64(vgetq_lane_f64(dmin, 1), -6.5) ||
        !near_f64(vgetq_lane_f64(dmax, 0), 2.5)) return 70;

    return 0;
}
#endif

int main(void) {
    int rc = test_sse();
    if (rc != 0) return rc;
#ifdef __zerocc__
    rc = test_neon();
    if (rc != 0) return rc;
#endif
    return 0;
}
