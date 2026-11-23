#include <arm_sve.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

static void check_fail(const char *msg, int line) {
    printf("FAIL: %s at line %d\n", msg, line);
    exit(1);
}

#define ASSERT_TRUE(cond) do { if (!(cond)) check_fail(#cond, __LINE__); } while (0)
#define ASSERT_EQ(a, b) do { if ((a) != (b)) check_fail(#a " == " #b, __LINE__); } while (0)

int main(void) {
    /* 1. Predicates */
    svbool_t p_all = svptrue_b32();
    svbool_t p_false = svpfalse_b();
    ASSERT_TRUE(p_false == 0);
    ASSERT_TRUE(p_all != 0);

    /* 2. Vector length / element count queries */
    uint64_t cb = svcntb();
    uint64_t ch = svcnth();
    uint64_t cw = svcntw();
    uint64_t cd = svcntd();
    ASSERT_TRUE(cb > 0);
    ASSERT_EQ(cb, ch * 2);
    ASSERT_EQ(ch, cw * 2);
    ASSERT_EQ(cw, cd * 2);

    /* 3. Whilelt predicate */
    svbool_t pg = svwhilelt_b32(0, 4);
    ASSERT_TRUE(pg != 0);

    /* 4. Broadcasts & Arithmetic */
    svint32_t a = svdup_n_s32(10);
    svint32_t b = svdup_n_s32(20);
    svint32_t sum = svadd_s32_z(p_all, a, b);
    svint32_t diff = svsub_s32_z(p_all, b, a);
    svint32_t prod = svmul_s32_z(p_all, a, b);

    int32_t out_buf[16] = {0};
    svst1_s32(p_all, out_buf, a);
    ASSERT_EQ(out_buf[0], 10);
    svst1_s32(p_all, out_buf, sum);
    ASSERT_EQ(out_buf[0], 30);
    ASSERT_EQ(out_buf[1], 30);

    svst1_s32(p_all, out_buf, diff);
    ASSERT_EQ(out_buf[0], 10);
    ASSERT_EQ(out_buf[1], 10);

    svst1_s32(p_all, out_buf, prod);
    ASSERT_EQ(out_buf[0], 200);
    ASSERT_EQ(out_buf[1], 200);

    /* 5. Predicated load & store */
    int32_t src[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    int32_t dst[16] = {0};
    svint32_t loaded = svld1_s32(pg, src);
    svst1_s32(pg, dst, loaded);
    ASSERT_EQ(dst[0], 1);
    ASSERT_EQ(dst[1], 2);
    ASSERT_EQ(dst[2], 3);
    ASSERT_EQ(dst[3], 4);
    ASSERT_EQ(dst[4], 0);

    /* 6. Floating-point arithmetic */
    svfloat32_t fa = svdup_n_f32(1.5f);
    svfloat32_t fb = svdup_n_f32(3.0f);
    svfloat32_t fsum = svadd_f32_z(p_all, fa, fb);
    svfloat32_t fdiff = svsub_f32_z(p_all, fb, fa);
    svfloat32_t fprod = svmul_f32_z(p_all, fa, fb);
    svfloat32_t fdiv = svdiv_f32_z(p_all, fb, fa);

    float out_f[16];
    svst1_f32(p_all, out_f, fsum);
    ASSERT_TRUE(out_f[0] > 4.49f && out_f[0] < 4.51f);

    svst1_f32(p_all, out_f, fdiff);
    ASSERT_TRUE(out_f[0] > 1.49f && out_f[0] < 1.51f);

    svst1_f32(p_all, out_f, fprod);
    ASSERT_TRUE(out_f[0] > 4.49f && out_f[0] < 4.51f);

    svst1_f32(p_all, out_f, fdiv);
    ASSERT_TRUE(out_f[0] > 1.99f && out_f[0] < 2.01f);

    /* 7. Bitwise operations */
    svint32_t band = svand_s32_z(p_all, a, b);
    svint32_t borr = svorr_s32_z(p_all, a, b);
    svint32_t beor = sveor_s32_z(p_all, a, b);

    svst1_s32(p_all, out_buf, band);
    ASSERT_EQ(out_buf[0], 10 & 20);

    svst1_s32(p_all, out_buf, borr);
    ASSERT_EQ(out_buf[0], 10 | 20);

    svst1_s32(p_all, out_buf, beor);
    ASSERT_EQ(out_buf[0], 10 ^ 20);

    /* 8. Comparisons */
    svbool_t cmp_eq = svcmpeq_s32(p_all, a, a);
    svbool_t cmp_gt = svcmpgt_s32(p_all, b, a);
    ASSERT_TRUE(cmp_eq != 0);
    ASSERT_TRUE(cmp_gt != 0);

    svbool_t fcmp_eq = svcmpeq_f32(p_all, fa, fa);
    svbool_t fcmp_gt = svcmpgt_f32(p_all, fb, fa);
    ASSERT_TRUE(fcmp_eq != 0);
    ASSERT_TRUE(fcmp_gt != 0);

    /* 9. Reduction */
    int32_t red_i = svaddv_s32(pg, loaded);
    ASSERT_EQ(red_i, 1 + 2 + 3 + 4);

    float red_f = svaddv_f32(pg, fa);
    ASSERT_TRUE(red_f > 5.99f && red_f < 6.01f);

    /* 10. SVE2 Saturating arithmetic */
    svint32_t big = svdup_n_s32(2000000000);
    svint32_t sat_add = svqadd_s32(big, big);
    svst1_s32(p_all, out_buf, sat_add);
    ASSERT_EQ(out_buf[0], 2147483647);

    svint32_t neg_big = svdup_n_s32(-2000000000);
    svint32_t sat_sub = svqsub_s32(neg_big, big);
    svst1_s32(p_all, out_buf, sat_sub);
    ASSERT_EQ(out_buf[0], (int32_t)(-2147483647 - 1));

    return 0;
}
