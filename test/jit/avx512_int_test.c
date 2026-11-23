/* test/jit/avx512_int_test.c - AVX-512 Integer SIMD and AVX-512BW/DQ test */
#include <stdio.h>
#include <stdint.h>
#include <immintrin.h>

#ifndef __ZEROCC__
__attribute__((target("avx512f,avx512dq,avx512bw")))
#endif
static int run_avx512_int_tests(void) {
    /* 1. 32-bit integer arithmetic (16 lanes) */
    int32_t a32[16], b32[16], c32[16];
    for (int i = 0; i < 16; i++) {
        a32[i] = (i + 1) * 10;
        b32[i] = i + 1;
        c32[i] = 0;
    }
    __m512i va32 = _mm512_loadu_si512((const __m512i *)a32);
    __m512i vb32 = _mm512_loadu_si512((const __m512i *)b32);

    /* add_epi32 */
    __m512i v_add32 = _mm512_add_epi32(va32, vb32);
    _mm512_storeu_si512((__m512i *)c32, v_add32);
    for (int i = 0; i < 16; i++) {
        int32_t expected = a32[i] + b32[i];
        if (c32[i] != expected) {
            printf("FAIL: add_epi32[%d] got %d expected %d\n", i, c32[i], expected);
            return 1;
        }
    }

    /* sub_epi32 */
    __m512i v_sub32 = _mm512_sub_epi32(va32, vb32);
    _mm512_storeu_si512((__m512i *)c32, v_sub32);
    for (int i = 0; i < 16; i++) {
        int32_t expected = a32[i] - b32[i];
        if (c32[i] != expected) {
            printf("FAIL: sub_epi32[%d] got %d expected %d\n", i, c32[i], expected);
            return 2;
        }
    }

    /* mullo_epi32 */
    __m512i v_mul32 = _mm512_mullo_epi32(va32, vb32);
    _mm512_storeu_si512((__m512i *)c32, v_mul32);
    for (int i = 0; i < 16; i++) {
        int32_t expected = a32[i] * b32[i];
        if (c32[i] != expected) {
            printf("FAIL: mullo_epi32[%d] got %d expected %d\n", i, c32[i], expected);
            return 3;
        }
    }

    /* 2. 64-bit integer arithmetic (8 lanes) */
    int64_t a64[8], b64[8], c64[8];
    for (int i = 0; i < 8; i++) {
        a64[i] = (int64_t)(i + 1) * 100000000LL;
        b64[i] = (int64_t)(i + 5) * 20000LL;
        c64[i] = 0;
    }
    __m512i va64 = _mm512_loadu_si512((const __m512i *)a64);
    __m512i vb64 = _mm512_loadu_si512((const __m512i *)b64);

    /* add_epi64 */
    __m512i v_add64 = _mm512_add_epi64(va64, vb64);
    _mm512_storeu_si512((__m512i *)c64, v_add64);
    for (int i = 0; i < 8; i++) {
        int64_t expected = a64[i] + b64[i];
        if (c64[i] != expected) {
            printf("FAIL: add_epi64[%d] got %lld expected %lld\n", i, (long long)c64[i], (long long)expected);
            return 4;
        }
    }

    /* sub_epi64 */
    __m512i v_sub64 = _mm512_sub_epi64(va64, vb64);
    _mm512_storeu_si512((__m512i *)c64, v_sub64);
    for (int i = 0; i < 8; i++) {
        int64_t expected = a64[i] - b64[i];
        if (c64[i] != expected) {
            printf("FAIL: sub_epi64[%d] got %lld expected %lld\n", i, (long long)c64[i], (long long)expected);
            return 5;
        }
    }

    /* mullo_epi64 */
    __m512i v_mul64 = _mm512_mullo_epi64(va64, vb64);
    _mm512_storeu_si512((__m512i *)c64, v_mul64);
    for (int i = 0; i < 8; i++) {
        int64_t expected = a64[i] * b64[i];
        if (c64[i] != expected) {
            printf("FAIL: mullo_epi64[%d] got %lld expected %lld\n", i, (long long)c64[i], (long long)expected);
            return 6;
        }
    }

    /* 3. Bitwise logic */
    __m512i v_and = _mm512_and_si512(va32, vb32);
    _mm512_storeu_si512((__m512i *)c32, v_and);
    for (int i = 0; i < 16; i++) {
        int32_t expected = a32[i] & b32[i];
        if (c32[i] != expected) {
            printf("FAIL: and_si512[%d] got %d expected %d\n", i, c32[i], expected);
            return 7;
        }
    }

    __m512i v_or = _mm512_or_si512(va32, vb32);
    _mm512_storeu_si512((__m512i *)c32, v_or);
    for (int i = 0; i < 16; i++) {
        int32_t expected = a32[i] | b32[i];
        if (c32[i] != expected) {
            printf("FAIL: or_si512[%d] got %d expected %d\n", i, c32[i], expected);
            return 8;
        }
    }

    __m512i v_xor = _mm512_xor_si512(va32, vb32);
    _mm512_storeu_si512((__m512i *)c32, v_xor);
    for (int i = 0; i < 16; i++) {
        int32_t expected = a32[i] ^ b32[i];
        if (c32[i] != expected) {
            printf("FAIL: xor_si512[%d] got %d expected %d\n", i, c32[i], expected);
            return 9;
        }
    }

    __m512i v_andnot = _mm512_andnot_si512(va32, vb32);
    _mm512_storeu_si512((__m512i *)c32, v_andnot);
    for (int i = 0; i < 16; i++) {
        int32_t expected = (~a32[i]) & b32[i];
        if (c32[i] != expected) {
            printf("FAIL: andnot_si512[%d] got %d expected %d\n", i, c32[i], expected);
            return 10;
        }
    }

    /* 4. 8-bit and 16-bit operations (AVX-512BW) */
    int8_t a8[64], b8[64], c8[64];
    for (int i = 0; i < 64; i++) {
        a8[i] = (int8_t)(i * 2);
        b8[i] = (int8_t)(i + 3);
        c8[i] = 0;
    }
    __m512i va8 = _mm512_loadu_si512((const __m512i *)a8);
    __m512i vb8 = _mm512_loadu_si512((const __m512i *)b8);
    __m512i v_add8 = _mm512_add_epi8(va8, vb8);
    _mm512_storeu_si512((__m512i *)c8, v_add8);
    for (int i = 0; i < 64; i++) {
        int8_t expected = (int8_t)(a8[i] + b8[i]);
        if (c8[i] != expected) {
            printf("FAIL: add_epi8[%d] got %d expected %d\n", i, c8[i], expected);
            return 11;
        }
    }

    __m512i v_sub8 = _mm512_sub_epi8(va8, vb8);
    _mm512_storeu_si512((__m512i *)c8, v_sub8);
    for (int i = 0; i < 64; i++) {
        int8_t expected = (int8_t)(a8[i] - b8[i]);
        if (c8[i] != expected) {
            printf("FAIL: sub_epi8[%d] got %d expected %d\n", i, c8[i], expected);
            return 12;
        }
    }

    int16_t a16[32], b16[32], c16[32];
    for (int i = 0; i < 32; i++) {
        a16[i] = (int16_t)(i * 50);
        b16[i] = (int16_t)(i * 3 + 7);
        c16[i] = 0;
    }
    __m512i va16 = _mm512_loadu_si512((const __m512i *)a16);
    __m512i vb16 = _mm512_loadu_si512((const __m512i *)b16);
    __m512i v_add16 = _mm512_add_epi16(va16, vb16);
    _mm512_storeu_si512((__m512i *)c16, v_add16);
    for (int i = 0; i < 32; i++) {
        int16_t expected = (int16_t)(a16[i] + b16[i]);
        if (c16[i] != expected) {
            printf("FAIL: add_epi16[%d] got %d expected %d\n", i, c16[i], expected);
            return 13;
        }
    }

    __m512i v_mul16 = _mm512_mullo_epi16(va16, vb16);
    _mm512_storeu_si512((__m512i *)c16, v_mul16);
    for (int i = 0; i < 32; i++) {
        int16_t expected = (int16_t)(a16[i] * b16[i]);
        if (c16[i] != expected) {
            printf("FAIL: mullo_epi16[%d] got %d expected %d\n", i, c16[i], expected);
            return 14;
        }
    }

    /* 5. Masked and Zeroing operations */
    __mmask16 k16 = 0xAA55;
    int32_t src32[16];
    for (int i = 0; i < 16; i++) src32[i] = 9999;
    __m512i vsrc32 = _mm512_loadu_si512((const __m512i *)src32);

    /* mask_add_epi32 */
    __m512i v_madd32 = _mm512_mask_add_epi32(vsrc32, k16, va32, vb32);
    _mm512_storeu_si512((__m512i *)c32, v_madd32);
    for (int i = 0; i < 16; i++) {
        int32_t expected = ((k16 >> i) & 1) ? (a32[i] + b32[i]) : src32[i];
        if (c32[i] != expected) {
            printf("FAIL: mask_add_epi32[%d] got %d expected %d\n", i, c32[i], expected);
            return 15;
        }
    }

    /* maskz_add_epi32 */
    __m512i v_zadd32 = _mm512_maskz_add_epi32(k16, va32, vb32);
    _mm512_storeu_si512((__m512i *)c32, v_zadd32);
    for (int i = 0; i < 16; i++) {
        int32_t expected = ((k16 >> i) & 1) ? (a32[i] + b32[i]) : 0;
        if (c32[i] != expected) {
            printf("FAIL: maskz_add_epi32[%d] got %d expected %d\n", i, c32[i], expected);
            return 16;
        }
    }

    /* mask_add_epi64 */
    __mmask8 k8 = 0x5A;
    int64_t src64[8];
    for (int i = 0; i < 8; i++) src64[i] = 77777777LL;
    __m512i vsrc64 = _mm512_loadu_si512((const __m512i *)src64);
    __m512i v_madd64 = _mm512_mask_add_epi64(vsrc64, k8, va64, vb64);
    _mm512_storeu_si512((__m512i *)c64, v_madd64);
    for (int i = 0; i < 8; i++) {
        int64_t expected = ((k8 >> i) & 1) ? (a64[i] + b64[i]) : src64[i];
        if (c64[i] != expected) {
            printf("FAIL: mask_add_epi64[%d] got %lld expected %lld\n", i, (long long)c64[i], (long long)expected);
            return 17;
        }
    }

    /* maskz_add_epi64 */
    __m512i v_zadd64 = _mm512_maskz_add_epi64(k8, va64, vb64);
    _mm512_storeu_si512((__m512i *)c64, v_zadd64);
    for (int i = 0; i < 8; i++) {
        int64_t expected = ((k8 >> i) & 1) ? (a64[i] + b64[i]) : 0;
        if (c64[i] != expected) {
            printf("FAIL: maskz_add_epi64[%d] got %lld expected %lld\n", i, (long long)c64[i], (long long)expected);
            return 18;
        }
    }

    /* 6. Integer comparisons */
    int32_t cmp_a[16], cmp_b[16];
    for (int i = 0; i < 16; i++) {
        cmp_a[i] = (i % 3 == 0) ? 100 : i;
        cmp_b[i] = (i % 3 == 0) ? 100 : (i + 1);
    }
    __m512i vca = _mm512_loadu_si512((const __m512i *)cmp_a);
    __m512i vcb = _mm512_loadu_si512((const __m512i *)cmp_b);

    __mmask16 m_eq = _mm512_cmpeq_epi32_mask(vca, vcb);
    for (int i = 0; i < 16; i++) {
        int bit = (m_eq >> i) & 1;
        int exp_bit = (cmp_a[i] == cmp_b[i]) ? 1 : 0;
        if (bit != exp_bit) {
            printf("FAIL: cmpeq_epi32_mask[%d] got %d expected %d\n", i, bit, exp_bit);
            return 19;
        }
    }

    __mmask16 m_lt = _mm512_cmplt_epi32_mask(vca, vcb);
    for (int i = 0; i < 16; i++) {
        int bit = (m_lt >> i) & 1;
        int exp_bit = (cmp_a[i] < cmp_b[i]) ? 1 : 0;
        if (bit != exp_bit) {
            printf("FAIL: cmplt_epi32_mask[%d] got %d expected %d\n", i, bit, exp_bit);
            return 20;
        }
    }

    __mmask16 m_gt = _mm512_cmpgt_epi32_mask(vca, vcb);
    for (int i = 0; i < 16; i++) {
        int bit = (m_gt >> i) & 1;
        int exp_bit = (cmp_a[i] > cmp_b[i]) ? 1 : 0;
        if (bit != exp_bit) {
            printf("FAIL: cmpgt_epi32_mask[%d] got %d expected %d\n", i, bit, exp_bit);
            return 21;
        }
    }

    /* 7. Broadcast / Set */
    __m512i v_bcast64 = _mm512_set1_epi64(0x123456789ABCDEF0LL);
    _mm512_storeu_si512((__m512i *)c64, v_bcast64);
    for (int i = 0; i < 8; i++) {
        if (c64[i] != 0x123456789ABCDEF0LL) {
            printf("FAIL: set1_epi64[%d] got 0x%llx\n", i, (long long)c64[i]);
            return 22;
        }
    }

    __m512i v_bcast16 = _mm512_set1_epi16((short)0x1234);
    _mm512_storeu_si512((__m512i *)c16, v_bcast16);
    for (int i = 0; i < 32; i++) {
        if (c16[i] != 0x1234) {
            printf("FAIL: set1_epi16[%d] got 0x%x\n", i, c16[i]);
            return 23;
        }
    }

    __m512i v_bcast8 = _mm512_set1_epi8((char)0x42);
    _mm512_storeu_si512((__m512i *)c8, v_bcast8);
    for (int i = 0; i < 64; i++) {
        if (c8[i] != 0x42) {
            printf("FAIL: set1_epi8[%d] got 0x%x\n", i, c8[i]);
            return 24;
        }
    }

    /* set_epi32 and setr_epi32 */
    __m512i v_set32 = _mm512_set_epi32(15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0);
    _mm512_storeu_si512((__m512i *)c32, v_set32);
    for (int i = 0; i < 16; i++) {
        if (c32[i] != i) {
            printf("FAIL: set_epi32[%d] got %d expected %d\n", i, c32[i], i);
            return 25;
        }
    }

    __m512i v_setr32 = _mm512_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
    _mm512_storeu_si512((__m512i *)c32, v_setr32);
    for (int i = 0; i < 16; i++) {
        if (c32[i] != i) {
            printf("FAIL: setr_epi32[%d] got %d expected %d\n", i, c32[i], i);
            return 26;
        }
    }

    /* 8. Masked moves */
    __m512i v_mmov32 = _mm512_mask_mov_epi32(vsrc32, k16, va32);
    _mm512_storeu_si512((__m512i *)c32, v_mmov32);
    for (int i = 0; i < 16; i++) {
        int32_t expected = ((k16 >> i) & 1) ? a32[i] : src32[i];
        if (c32[i] != expected) {
            printf("FAIL: mask_mov_epi32[%d] got %d expected %d\n", i, c32[i], expected);
            return 27;
        }
    }

    __m512i v_zmov32 = _mm512_maskz_mov_epi32(k16, va32);
    _mm512_storeu_si512((__m512i *)c32, v_zmov32);
    for (int i = 0; i < 16; i++) {
        int32_t expected = ((k16 >> i) & 1) ? a32[i] : 0;
        if (c32[i] != expected) {
            printf("FAIL: maskz_mov_epi32[%d] got %d expected %d\n", i, c32[i], expected);
            return 28;
        }
    }

    return 0;
}

int main(void) {
#ifndef __ZEROCC__
    __builtin_cpu_init();
    if (!__builtin_cpu_supports("avx512f") || !__builtin_cpu_supports("avx512bw") || !__builtin_cpu_supports("avx512dq")) {
        printf("AVX-512 not supported by host CPU (skipped)\n");
        return 0;
    }
#endif
    int rc = run_avx512_int_tests();
    if (rc == 0) {
        printf("PASS: AVX-512 Integer SIMD and AVX-512BW/DQ tests succeeded\n");
    }
    return rc;
}
