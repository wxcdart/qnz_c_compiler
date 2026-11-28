/* test/jit/avx512_ext_test.c - AVX-512 min/max (signed+unsigned epi32) and
 * permutexvar (epi32/ps) with mask/maskz variants (Sprint 277).
 * Runs through the scalar emulation unless -mavx512f is given; the
 * encodings are covered by tools/verify/avx512_diff.py. */
#include <stdio.h>
#include <string.h>
#include <immintrin.h>

#ifndef __ZEROCC__
__attribute__((target("avx512f,avx512dq")))
#endif
static int run_ext_tests(void) {
    int a[16] = {0, 1, -1, 2147483647, (int)0x80000000, 5, -5, 100,
                 -100, 7, 7, (int)0xFFFFFFF0, 0x7FFFFFF0, 3, -3, 42};
    int b[16] = {0, -1, 1, (int)0x80000000, 2147483647, 5, 6, -100,
                 100, 8, 6, 16, -16, (int)0x80000000, 3, 42};
    int out[16];
    unsigned int ua[16], ub[16];
    memcpy(ua, a, sizeof(a));
    memcpy(ub, b, sizeof(b));

    __m512i va = _mm512_loadu_si512((const __m512i *)a);
    __m512i vb = _mm512_loadu_si512((const __m512i *)b);

    _mm512_storeu_si512((__m512i *)out, _mm512_min_epi32(va, vb));
    for (int i = 0; i < 16; i++) {
        int e = a[i] < b[i] ? a[i] : b[i];
        if (out[i] != e) { printf("FAIL min_epi32[%d] got %d expected %d\n", i, out[i], e); return 1; }
    }
    _mm512_storeu_si512((__m512i *)out, _mm512_max_epi32(va, vb));
    for (int i = 0; i < 16; i++) {
        int e = a[i] > b[i] ? a[i] : b[i];
        if (out[i] != e) { printf("FAIL max_epi32[%d] got %d expected %d\n", i, out[i], e); return 1; }
    }
    _mm512_storeu_si512((__m512i *)out, _mm512_min_epu32(va, vb));
    for (int i = 0; i < 16; i++) {
        unsigned e = ua[i] < ub[i] ? ua[i] : ub[i];
        if ((unsigned)out[i] != e) { printf("FAIL min_epu32[%d] got %u expected %u\n", i, (unsigned)out[i], e); return 1; }
    }
    _mm512_storeu_si512((__m512i *)out, _mm512_max_epu32(va, vb));
    for (int i = 0; i < 16; i++) {
        unsigned e = ua[i] > ub[i] ? ua[i] : ub[i];
        if ((unsigned)out[i] != e) { printf("FAIL max_epu32[%d] got %u expected %u\n", i, (unsigned)out[i], e); return 1; }
    }

    /* masked / zero-masked */
    __mmask16 k = 0xA5A5;
    int src[16];
    for (int i = 0; i < 16; i++) src[i] = 1000 + i;
    __m512i vsrc = _mm512_loadu_si512((const __m512i *)src);
    _mm512_storeu_si512((__m512i *)out, _mm512_mask_min_epi32(vsrc, k, va, vb));
    for (int i = 0; i < 16; i++) {
        int e = ((k >> i) & 1) ? (a[i] < b[i] ? a[i] : b[i]) : src[i];
        if (out[i] != e) { printf("FAIL mask_min_epi32[%d] got %d expected %d\n", i, out[i], e); return 1; }
    }
    _mm512_storeu_si512((__m512i *)out, _mm512_maskz_max_epu32(k, va, vb));
    for (int i = 0; i < 16; i++) {
        unsigned e = ((k >> i) & 1) ? (ua[i] > ub[i] ? ua[i] : ub[i]) : 0u;
        if ((unsigned)out[i] != e) { printf("FAIL maskz_max_epu32[%d] got %u expected %u\n", i, (unsigned)out[i], e); return 1; }
    }

    /* permutexvar: indices use only the low 4 bits */
    int idx[16] = {15, 14, 13, 12, 0, 1, 2, 3, 16, 17, 31, 32, 7, 7, 7, 7};
    __m512i vidx = _mm512_loadu_si512((const __m512i *)idx);
    _mm512_storeu_si512((__m512i *)out, _mm512_permutexvar_epi32(vidx, va));
    for (int i = 0; i < 16; i++) {
        int e = a[idx[i] & 15];
        if (out[i] != e) { printf("FAIL permutexvar_epi32[%d] got %d expected %d\n", i, out[i], e); return 1; }
    }
    float fa[16], fo[16];
    for (int i = 0; i < 16; i++) fa[i] = (float)i * 1.5f - 3.0f;
    __m512 vf = _mm512_loadu_ps(fa);
    _mm512_storeu_ps(fo, _mm512_permutexvar_ps(vidx, vf));
    for (int i = 0; i < 16; i++) {
        float e = fa[idx[i] & 15];
        if (fo[i] != e) { printf("FAIL permutexvar_ps[%d] got %f expected %f\n", i, fo[i], e); return 1; }
    }
    printf("AVX-512 ext (min/max/permutexvar) OK\n");
    return 0;
}

int main(void) {
#ifndef __ZEROCC__
    __builtin_cpu_init();
    if (!__builtin_cpu_supports("avx512f")) {
        printf("AVX-512 not supported by host CPU (skipped)\n");
        return 0;
    }
#endif
    return run_ext_tests();
}
