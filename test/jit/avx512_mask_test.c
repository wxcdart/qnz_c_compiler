/* test/jit/avx512_mask_test.c - AVX-512 Opmask and Predication test */
#include <stdio.h>
#include <stdint.h>
#include <immintrin.h>

static float diff_f(float a, float b) {
    float d = a - b;
    return d < 0 ? -d : d;
}

static double diff_d(double a, double b) {
    double d = a - b;
    return d < 0 ? -d : d;
}

#ifndef __ZEROCC__
__attribute__((target("avx512f,avx512dq,avx512bw")))
#endif
static int run_avx512_mask_tests(void) {
    /* 1. Opmask bitwise and conversion operations */
    __mmask16 k16_a = _cvtu32_mask16(0x00FF);
    __mmask16 k16_b = _cvtu32_mask16(0x0F0F);
    if (_cvtmask16_u32(_kand_mask16(k16_a, k16_b)) != 0x000F) {
        printf("FAIL: kand_mask16\n");
        return 1;
    }
    if (_cvtmask16_u32(_kor_mask16(k16_a, k16_b)) != 0x0FFF) {
        printf("FAIL: kor_mask16\n");
        return 2;
    }
    if (_cvtmask16_u32(_kxor_mask16(k16_a, k16_b)) != 0x0FF0) {
        printf("FAIL: kxor_mask16\n");
        return 3;
    }
    if (_cvtmask16_u32(_knot_mask16(k16_a)) != 0xFF00) {
        printf("FAIL: knot_mask16\n");
        return 4;
    }

    __mmask8 k8_a = _cvtu32_mask8(0x0F);
    __mmask8 k8_b = _cvtu32_mask8(0x33);
    if (_cvtmask8_u32(_kand_mask8(k8_a, k8_b)) != 0x03) {
        printf("FAIL: kand_mask8\n");
        return 5;
    }
    if (_cvtmask8_u32(_kor_mask8(k8_a, k8_b)) != 0x3F) {
        printf("FAIL: kor_mask8\n");
        return 6;
    }
    if (_cvtmask8_u32(_kxor_mask8(k8_a, k8_b)) != 0x3C) {
        printf("FAIL: kxor_mask8\n");
        return 60;
    }
    if (_cvtmask8_u32(_knot_mask8(k8_a)) != 0xF0) {
        printf("FAIL: knot_mask8\n");
        return 61;
    }

    /* Load / Store Mask */
    __mmask16 m16_val = 0xA5A5;
    __mmask16 lm16 = _load_mask16(&m16_val);
    if (_cvtmask16_u32(lm16) != 0xA5A5) {
        printf("FAIL: load_mask16\n");
        return 62;
    }
    __mmask16 sm16_out = 0;
    _store_mask16(&sm16_out, lm16);
    if (sm16_out != 0xA5A5) {
        printf("FAIL: store_mask16\n");
        return 63;
    }

    __mmask8 m8_val = 0x5A;
    __mmask8 lm8 = _load_mask8(&m8_val);
    if (_cvtmask8_u32(lm8) != 0x5A) {
        printf("FAIL: load_mask8\n");
        return 64;
    }
    __mmask8 sm8_out = 0;
    _store_mask8(&sm8_out, lm8);
    if (sm8_out != 0x5A) {
        printf("FAIL: store_mask8\n");
        return 65;
    }

    /* kortestz / kortestc */
    __mmask16 kz1 = _cvtu32_mask16(0x0000);
    __mmask16 kz2 = _cvtu32_mask16(0x0000);
    if (_kortestz_mask16_u8(kz1, kz2) != 1 || _kortestc_mask16_u8(kz1, kz2) != 0) {
        printf("FAIL: kortest 16 zero\n");
        return 66;
    }
    __mmask16 kc1 = _cvtu32_mask16(0xFFFF);
    __mmask16 kc2 = _cvtu32_mask16(0x0000);
    if (_kortestz_mask16_u8(kc1, kc2) != 0 || _kortestc_mask16_u8(kc1, kc2) != 1) {
        printf("FAIL: kortest 16 all-ones\n");
        return 67;
    }
    __mmask8 kz8_1 = _cvtu32_mask8(0x00);
    __mmask8 kz8_2 = _cvtu32_mask8(0x00);
    if (_kortestz_mask8_u8(kz8_1, kz8_2) != 1 || _kortestc_mask8_u8(kz8_1, kz8_2) != 0) {
        printf("FAIL: kortest 8 zero\n");
        return 68;
    }
    __mmask8 kc8_1 = _cvtu32_mask8(0x0F);
    __mmask8 kc8_2 = _cvtu32_mask8(0xF0);
    if (_kortestz_mask8_u8(kc8_1, kc8_2) != 0 || _kortestc_mask8_u8(kc8_1, kc8_2) != 1) {
        printf("FAIL: kortest 8 all-ones\n");
        return 69;
    }

    /* Masked Blend */
    __m512 b_va = _mm512_set1_ps(1.0f);
    __m512 b_vb = _mm512_set1_ps(2.0f);
    __mmask16 kblend = _cvtu32_mask16(0x5555);
    __m512 b_vr = _mm512_mask_blend_ps(kblend, b_va, b_vb);
    float bout_f[16];
    _mm512_storeu_ps(bout_f, b_vr);
    for (int i = 0; i < 16; i++) {
        float exp_f = (i % 2 == 0) ? 2.0f : 1.0f;
        if (diff_f(bout_f[i], exp_f) > 1e-5f) {
            printf("FAIL: mask_blend_ps[%d]\n", i);
            return 70;
        }
    }

    __m512d b_vda = _mm512_set1_pd(10.0);
    __m512d b_vdb = _mm512_set1_pd(20.0);
    __mmask8 kdblend = _cvtu32_mask8(0x0F);
    __m512d b_vdr = _mm512_mask_blend_pd(kdblend, b_vda, b_vdb);
    double bout_d[8];
    _mm512_storeu_pd(bout_d, b_vdr);
    for (int i = 0; i < 8; i++) {
        double exp_d = (i < 4) ? 20.0 : 10.0;
        if (diff_d(bout_d[i], exp_d) > 1e-5) {
            printf("FAIL: mask_blend_pd[%d]\n", i);
            return 71;
        }
    }

    /* 2. Masked load and store (float, 16 lanes) */
    float fa[16], fb[16], fc[16];
    for (int i = 0; i < 16; i++) {
        fa[i] = (float)(i + 1);
        fb[i] = (float)((i + 1) * 10);
        fc[i] = -999.0f;
    }

    /* k = 0xAAAA: even indices (0, 2, 4...) are 0, odd indices (1, 3, 5...) are 1 */
    __mmask16 mask_odd = _cvtu32_mask16(0xAAAA);
    __m512 src_f = _mm512_loadu_ps(fb);

    /* Merging load: unmasked lanes should retain fb, masked lanes should get fa */
    __m512 v_mload = _mm512_mask_loadu_ps(src_f, mask_odd, fa);
    _mm512_storeu_ps(fc, v_mload);
    for (int i = 0; i < 16; i++) {
        float expected = (i % 2 == 1) ? fa[i] : fb[i];
        if (diff_f(fc[i], expected) > 1e-5f) {
            printf("FAIL: mask_loadu_ps[%d] got %f expected %f\n", i, fc[i], expected);
            return 7;
        }
    }

    /* Zeroing load: unmasked lanes should be 0.0, masked lanes should get fa */
    __m512 v_zload = _mm512_maskz_loadu_ps(mask_odd, fa);
    _mm512_storeu_ps(fc, v_zload);
    for (int i = 0; i < 16; i++) {
        float expected = (i % 2 == 1) ? fa[i] : 0.0f;
        if (diff_f(fc[i], expected) > 1e-5f) {
            printf("FAIL: maskz_loadu_ps[%d] got %f expected %f\n", i, fc[i], expected);
            return 8;
        }
    }

    /* Masked store: write only odd lanes to fc */
    for (int i = 0; i < 16; i++) fc[i] = 100.0f;
    _mm512_mask_storeu_ps(fc, mask_odd, _mm512_loadu_ps(fa));
    for (int i = 0; i < 16; i++) {
        float expected = (i % 2 == 1) ? fa[i] : 100.0f;
        if (diff_f(fc[i], expected) > 1e-5f) {
            printf("FAIL: mask_storeu_ps[%d] got %f expected %f\n", i, fc[i], expected);
            return 9;
        }
    }

    /* 3. Masked load and store (double, 8 lanes) */
    double da[8], db[8], dc[8];
    for (int i = 0; i < 8; i++) {
        da[i] = (double)(i + 1);
        db[i] = (double)((i + 1) * 20);
        dc[i] = -999.0;
    }
    __mmask8 mask_d = _cvtu32_mask8(0x55); /* 01010101: even indices 1, odd indices 0 */
    __m512d src_d = _mm512_loadu_pd(db);
    __m512d v_dmload = _mm512_mask_loadu_pd(src_d, mask_d, da);
    _mm512_storeu_pd(dc, v_dmload);
    for (int i = 0; i < 8; i++) {
        double expected = (i % 2 == 0) ? da[i] : db[i];
        if (diff_d(dc[i], expected) > 1e-5) {
            printf("FAIL: mask_loadu_pd[%d] got %f expected %f\n", i, dc[i], expected);
            return 10;
        }
    }

    __m512d v_dzload = _mm512_maskz_loadu_pd(mask_d, da);
    _mm512_storeu_pd(dc, v_dzload);
    for (int i = 0; i < 8; i++) {
        double expected = (i % 2 == 0) ? da[i] : 0.0;
        if (diff_d(dc[i], expected) > 1e-5) {
            printf("FAIL: maskz_loadu_pd[%d] got %f expected %f\n", i, dc[i], expected);
            return 11;
        }
    }

    /* 4. Masked Vector Arithmetic */
    __m512 va = _mm512_loadu_ps(fa);
    __m512 vb = _mm512_loadu_ps(fb);
    __m512 v_madd = _mm512_mask_add_ps(src_f, mask_odd, va, vb);
    _mm512_storeu_ps(fc, v_madd);
    for (int i = 0; i < 16; i++) {
        float expected = (i % 2 == 1) ? (fa[i] + fb[i]) : fb[i];
        if (diff_f(fc[i], expected) > 1e-5f) {
            printf("FAIL: mask_add_ps[%d] got %f expected %f\n", i, fc[i], expected);
            return 12;
        }
    }

    __m512 v_zadd = _mm512_maskz_add_ps(mask_odd, va, vb);
    _mm512_storeu_ps(fc, v_zadd);
    for (int i = 0; i < 16; i++) {
        float expected = (i % 2 == 1) ? (fa[i] + fb[i]) : 0.0f;
        if (diff_f(fc[i], expected) > 1e-5f) {
            printf("FAIL: maskz_add_ps[%d] got %f expected %f\n", i, fc[i], expected);
            return 13;
        }
    }

    __m512 v_zmul = _mm512_maskz_mul_ps(mask_odd, va, vb);
    _mm512_storeu_ps(fc, v_zmul);
    for (int i = 0; i < 16; i++) {
        float expected = (i % 2 == 1) ? (fa[i] * fb[i]) : 0.0f;
        if (diff_f(fc[i], expected) > 1e-5f) {
            printf("FAIL: maskz_mul_ps[%d] got %f expected %f\n", i, fc[i], expected);
            return 14;
        }
    }

    /* 5. Masked Move */
    __m512 v_mmov = _mm512_mask_mov_ps(src_f, mask_odd, va);
    _mm512_storeu_ps(fc, v_mmov);
    for (int i = 0; i < 16; i++) {
        float expected = (i % 2 == 1) ? fa[i] : fb[i];
        if (diff_f(fc[i], expected) > 1e-5f) {
            printf("FAIL: mask_mov_ps[%d] got %f expected %f\n", i, fc[i], expected);
            return 15;
        }
    }

    /* 6. Comparisons producing masks */
    float cmp_a[16], cmp_b[16];
    for (int i = 0; i < 16; i++) {
        cmp_a[i] = (float)i;
        cmp_b[i] = (i < 8) ? (float)i : 99.0f;
    }
    __m512 v_ca = _mm512_loadu_ps(cmp_a);
    __m512 v_cb = _mm512_loadu_ps(cmp_b);
    __mmask16 eq_mask = _mm512_cmpeq_ps_mask(v_ca, v_cb);
    if (_cvtmask16_u32(eq_mask) != 0x00FF) {
        printf("FAIL: cmpeq_ps_mask got 0x%04X expected 0x00FF\n", _cvtmask16_u32(eq_mask));
        return 16;
    }

    __mmask16 lt_mask = _mm512_cmplt_ps_mask(v_ca, v_cb);
    if (_cvtmask16_u32(lt_mask) != 0xFF00) {
        printf("FAIL: cmplt_ps_mask got 0x%04X expected 0xFF00\n", _cvtmask16_u32(lt_mask));
        return 17;
    }

    /* 7. Double comparisons producing masks */
    double dcmp_a[8], dcmp_b[8];
    for (int i = 0; i < 8; i++) {
        dcmp_a[i] = (double)i;
        dcmp_b[i] = (i >= 4) ? (double)i : -1.0;
    }
    __m512d v_dca = _mm512_loadu_pd(dcmp_a);
    __m512d v_dcb = _mm512_loadu_pd(dcmp_b);
    __mmask8 deq_mask = _mm512_cmpeq_pd_mask(v_dca, v_dcb);
    if (_cvtmask8_u32(deq_mask) != 0xF0) {
        printf("FAIL: cmpeq_pd_mask got 0x%02X expected 0xF0\n", _cvtmask8_u32(deq_mask));
        return 18;
    }

    printf("AVX-512 Opmask / Predication tests OK\n");
    return 0;
}

int main(void) {
#ifndef __ZEROCC__
    __builtin_cpu_init();
    if (!__builtin_cpu_supports("avx512f") || !__builtin_cpu_supports("avx512bw")) {
        printf("AVX-512 not supported by host CPU (skipped)\n");
        return 0;
    }
#endif
    return run_avx512_mask_tests();
}
