#include <riscv_vector.h>
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
    /* 1. Vector length configuration */
    size_t vl32 = __riscv_vsetvl_e32m1(4);
    size_t vl64 = __riscv_vsetvl_e64m1(2);
    ASSERT_TRUE(vl32 > 0);
    ASSERT_TRUE(vl64 > 0);

    /* 2. Broadcasts & Arithmetic */
    vint32m1_t a = __riscv_vmv_v_x_i32m1(10, vl32);
    vint32m1_t b = __riscv_vmv_v_x_i32m1(20, vl32);
    vint32m1_t sum = __riscv_vadd_vv_i32m1(a, b, vl32);
    vint32m1_t diff = __riscv_vsub_vv_i32m1(b, a, vl32);
    vint32m1_t prod = __riscv_vmul_vv_i32m1(a, b, vl32);
    vint32m1_t quot = __riscv_vdiv_vv_i32m1(b, a, vl32);

    int32_t out_buf[4] = {0};
    __riscv_vse32_v_i32m1(out_buf, a, vl32);
    ASSERT_EQ(out_buf[0], 10);
    ASSERT_EQ(out_buf[1], 10);

    __riscv_vse32_v_i32m1(out_buf, sum, vl32);
    ASSERT_EQ(out_buf[0], 30);
    ASSERT_EQ(out_buf[1], 30);

    __riscv_vse32_v_i32m1(out_buf, diff, vl32);
    ASSERT_EQ(out_buf[0], 10);

    __riscv_vse32_v_i32m1(out_buf, prod, vl32);
    ASSERT_EQ(out_buf[0], 200);

    __riscv_vse32_v_i32m1(out_buf, quot, vl32);
    ASSERT_EQ(out_buf[0], 2);

    /* 3. Extract scalar */
    int32_t a_scal = __riscv_vmv_x_s_i32m1_i32(a);
    int32_t sum_scal = __riscv_vmv_x_s_i32m1_i32(sum);
    ASSERT_EQ(a_scal, 10);
    ASSERT_EQ(sum_scal, 30);

    /* 4. Unit-stride Load & Store */
    int32_t src[4] = {100, 200, 300, 400};
    int32_t dst[4] = {0};
    vint32m1_t loaded = __riscv_vle32_v_i32m1(src, vl32);
    __riscv_vse32_v_i32m1(dst, loaded, vl32);
    ASSERT_EQ(dst[0], 100);
    ASSERT_EQ(dst[1], 200);
    ASSERT_EQ(dst[2], 300);
    ASSERT_EQ(dst[3], 400);

    /* 5. Floating-point operations */
    vfloat32m1_t fa = __riscv_vfmv_v_f_f32m1(1.5f, vl32);
    vfloat32m1_t fb = __riscv_vfmv_v_f_f32m1(3.0f, vl32);
    vfloat32m1_t fsum = __riscv_vfadd_vv_f32m1(fa, fb, vl32);
    vfloat32m1_t fdiff = __riscv_vfsub_vv_f32m1(fb, fa, vl32);
    vfloat32m1_t fprod = __riscv_vfmul_vv_f32m1(fa, fb, vl32);
    vfloat32m1_t fdiv = __riscv_vfdiv_vv_f32m1(fb, fa, vl32);

    float out_f[4] = {0};
    __riscv_vse32_v_f32m1(out_f, fsum, vl32);
    ASSERT_TRUE(out_f[0] > 4.49f && out_f[0] < 4.51f);

    __riscv_vse32_v_f32m1(out_f, fdiff, vl32);
    ASSERT_TRUE(out_f[0] > 1.49f && out_f[0] < 1.51f);

    __riscv_vse32_v_f32m1(out_f, fprod, vl32);
    ASSERT_TRUE(out_f[0] > 4.49f && out_f[0] < 4.51f);

    __riscv_vse32_v_f32m1(out_f, fdiv, vl32);
    ASSERT_TRUE(out_f[0] > 1.99f && out_f[0] < 2.01f);

    float fa_scal = __riscv_vfmv_f_s_f32m1_f32(fa);
    ASSERT_TRUE(fa_scal > 1.49f && fa_scal < 1.51f);

    /* 6. Bitwise operations */
    vint32m1_t band = __riscv_vand_vv_i32m1(a, b, vl32);
    vint32m1_t borr = __riscv_vor_vv_i32m1(a, b, vl32);
    vint32m1_t bxor = __riscv_vxor_vv_i32m1(a, b, vl32);

    __riscv_vse32_v_i32m1(out_buf, band, vl32);
    ASSERT_EQ(out_buf[0], 10 & 20);

    __riscv_vse32_v_i32m1(out_buf, borr, vl32);
    ASSERT_EQ(out_buf[0], 10 | 20);

    __riscv_vse32_v_i32m1(out_buf, bxor, vl32);
    ASSERT_EQ(out_buf[0], 10 ^ 20);

    /* 7. Comparisons */
    vbool32_t cmp_eq = __riscv_vmseq_vv_i32m1_b32(a, a, vl32);
    vbool32_t cmp_lt = __riscv_vmslt_vv_i32m1_b32(a, b, vl32);
    int32_t cmp_eq_s = __riscv_vmv_x_s_i32m1_i32((vint32m1_t)cmp_eq);
    int32_t cmp_lt_s = __riscv_vmv_x_s_i32m1_i32((vint32m1_t)cmp_lt);
    ASSERT_TRUE(cmp_eq_s != 0);
    ASSERT_TRUE(cmp_lt_s != 0);

    /* 8. Reductions */
    vint32m1_t zero_init = __riscv_vmv_v_x_i32m1(0, vl32);
    vint32m1_t red_v = __riscv_vredsum_vs_i32m1_i32m1(loaded, zero_init, vl32);
    int32_t red_i = __riscv_vmv_x_s_i32m1_i32(red_v);
    ASSERT_EQ(red_i, 100 + 200 + 300 + 400);

    vfloat32m1_t fzero_init = __riscv_vfmv_v_f_f32m1(0.0f, vl32);
    vfloat32m1_t fred_v = __riscv_vfredusum_vs_f32m1_f32m1(fa, fzero_init, vl32);
    float red_f = __riscv_vfmv_f_s_f32m1_f32(fred_v);
    ASSERT_TRUE(red_f > 5.99f && red_f < 6.01f);

    return 0;
}
