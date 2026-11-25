/*
 * simd_slp_tree_test.c - Test Advanced SLP Tree Vectorization (Sprint 224)
 *
 * Verifies multi-expression DAG tree vectorization for non-loop basic block code:
 * - 4D vector fused multiply-add (a * b + c)
 * - 4D vector compound arithmetic ((a + b) * (c - d))
 * - 4D vector dual multiply-subtract (a * b - c * d)
 * - 4D vector self-square operand reuse (a * a + b * b)
 * - Float Vec4f compound arithmetic and FMA
 * - Double Vec2d compound arithmetic and dual multiply-add
 * - Array 4-element int 3-stage chain ((a + b - c) * d)
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <math.h>
#include <assert.h>

struct Vec4i {
    int x;
    int y;
    int z;
    int w;
};

struct Vec4f {
    float x;
    float y;
    float z;
    float w;
};

struct Vec2d {
    double x;
    double y;
};

/* 1. Vec4i Fused Multiply-Add (a * b + c) */
void test_vec4i_fma(struct Vec4i *dst, const struct Vec4i *a, const struct Vec4i *b, const struct Vec4i *c) {
    dst->x = a->x * b->x + c->x;
    dst->y = a->y * b->y + c->y;
    dst->z = a->z * b->z + c->z;
    dst->w = a->w * b->w + c->w;
}

/* 2. Vec4i Compound Arithmetic ((a + b) * (c - d)) */
void test_vec4i_compound(struct Vec4i *dst, const struct Vec4i *a, const struct Vec4i *b, const struct Vec4i *c, const struct Vec4i *d) {
    dst->x = (a->x + b->x) * (c->x - d->x);
    dst->y = (a->y + b->y) * (c->y - d->y);
    dst->z = (a->z + b->z) * (c->z - d->z);
    dst->w = (a->w + b->w) * (c->w - d->w);
}

/* 3. Vec4i Dual Multiply-Subtract (a * b - c * d) */
void test_vec4i_mul_sub(struct Vec4i *dst, const struct Vec4i *a, const struct Vec4i *b, const struct Vec4i *c, const struct Vec4i *d) {
    dst->x = a->x * b->x - c->x * d->x;
    dst->y = a->y * b->y - c->y * d->y;
    dst->z = a->z * b->z - c->z * d->z;
    dst->w = a->w * b->w - c->w * d->w;
}

/* 4. Vec4i Self-Square (a * a + b * b) */
void test_vec4i_square(struct Vec4i *dst, const struct Vec4i *a, const struct Vec4i *b) {
    dst->x = a->x * a->x + b->x * b->x;
    dst->y = a->y * a->y + b->y * b->y;
    dst->z = a->z * a->z + b->z * b->z;
    dst->w = a->w * a->w + b->w * b->w;
}

/* 5. Vec4f Compound Arithmetic ((a + b) * (c - d)) */
void test_vec4f_compound(struct Vec4f *dst, const struct Vec4f *a, const struct Vec4f *b, const struct Vec4f *c, const struct Vec4f *d) {
    dst->x = (a->x + b->x) * (c->x - d->x);
    dst->y = (a->y + b->y) * (c->y - d->y);
    dst->z = (a->z + b->z) * (c->z - d->z);
    dst->w = (a->w + b->w) * (c->w - d->w);
}

/* 6. Vec4f FMA (a * b + c) */
void test_vec4f_fma(struct Vec4f *dst, const struct Vec4f *a, const struct Vec4f *b, const struct Vec4f *c) {
    dst->x = a->x * b->x + c->x;
    dst->y = a->y * b->y + c->y;
    dst->z = a->z * b->z + c->z;
    dst->w = a->w * b->w + c->w;
}

/* 7. Vec2d Compound Arithmetic ((a + b) * (c - d)) */
void test_vec2d_compound(struct Vec2d *dst, const struct Vec2d *a, const struct Vec2d *b, const struct Vec2d *c, const struct Vec2d *d) {
    dst->x = (a->x + b->x) * (c->x - d->x);
    dst->y = (a->y + b->y) * (c->y - d->y);
}

/* 8. Vec2d Dual Multiply-Add (a * b + c * d) */
void test_vec2d_mul_add(struct Vec2d *dst, const struct Vec2d *a, const struct Vec2d *b, const struct Vec2d *c, const struct Vec2d *d) {
    dst->x = a->x * b->x + c->x * d->x;
    dst->y = a->y * b->y + c->y * d->y;
}

/* 9. 4-element int array 3-stage chain ((a + b - c) * d) */
void test_arr4i_chain(int *dst, const int *a, const int *b, const int *c, const int *d) {
    dst[0] = (a[0] + b[0] - c[0]) * d[0];
    dst[1] = (a[1] + b[1] - c[1]) * d[1];
    dst[2] = (a[2] + b[2] - c[2]) * d[2];
    dst[3] = (a[3] + b[3] - c[3]) * d[3];
}

/* 10. 4-element int array compound (a * b + c * d) */
void test_arr4i_compound(int *dst, const int *a, const int *b, const int *c, const int *d) {
    dst[0] = a[0] * b[0] + c[0] * d[0];
    dst[1] = a[1] * b[1] + c[1] * d[1];
    dst[2] = a[2] * b[2] + c[2] * d[2];
    dst[3] = a[3] * b[3] + c[3] * d[3];
}

int main(void) {
    /* Test 1: Vec4i FMA */
    {
        struct Vec4i a = { 2, 3, 4, 5 };
        struct Vec4i b = { 10, 20, 30, 40 };
        struct Vec4i c = { 1, 2, 3, 4 };
        struct Vec4i dst = { 0 };
        test_vec4i_fma(&dst, &a, &b, &c);
        assert(dst.x == 21);
        assert(dst.y == 62);
        assert(dst.z == 123);
        assert(dst.w == 204);
        printf("1 ok\n");
    }

    /* Test 2: Vec4i Compound ((a + b) * (c - d)) */
    {
        struct Vec4i a = { 10, 20, 30, 40 };
        struct Vec4i b = { 5, 4, 3, 2 };
        struct Vec4i c = { 8, 9, 10, 11 };
        struct Vec4i d = { 2, 3, 4, 5 };
        struct Vec4i dst = { 0 };
        test_vec4i_compound(&dst, &a, &b, &c, &d);
        assert(dst.x == 15 * 6);
        assert(dst.y == 24 * 6);
        assert(dst.z == 33 * 6);
        assert(dst.w == 42 * 6);
        printf("2 ok\n");
    }

    /* Test 3: Vec4i Dual Multiply-Subtract (a * b - c * d) */
    {
        struct Vec4i a = { 10, 20, 30, 40 };
        struct Vec4i b = { 3, 3, 3, 3 };
        struct Vec4i c = { 2, 4, 6, 8 };
        struct Vec4i d = { 5, 5, 5, 5 };
        struct Vec4i dst = { 0 };
        test_vec4i_mul_sub(&dst, &a, &b, &c, &d);
        assert(dst.x == 30 - 10);
        assert(dst.y == 60 - 20);
        assert(dst.z == 90 - 30);
        assert(dst.w == 120 - 40);
        printf("3 ok\n");
    }

    /* Test 4: Vec4i Self-Square (a * a + b * b) */
    {
        struct Vec4i a = { 3, 4, 5, 6 };
        struct Vec4i b = { 4, 5, 6, 7 };
        struct Vec4i dst = { 0 };
        test_vec4i_square(&dst, &a, &b);
        assert(dst.x == 9 + 16);
        assert(dst.y == 16 + 25);
        assert(dst.z == 25 + 36);
        assert(dst.w == 36 + 49);
        printf("4 ok\n");
    }

    /* Test 5: Vec4f Compound ((a + b) * (c - d)) */
    {
        struct Vec4f a = { 1.5f, 2.5f, 3.5f, 4.5f };
        struct Vec4f b = { 0.5f, 0.5f, 0.5f, 0.5f };
        struct Vec4f c = { 5.0f, 6.0f, 7.0f, 8.0f };
        struct Vec4f d = { 2.0f, 2.0f, 2.0f, 2.0f };
        struct Vec4f dst = { 0 };
        test_vec4f_compound(&dst, &a, &b, &c, &d);
        assert(fabsf(dst.x - 2.0f * 3.0f) < 1e-4f);
        assert(fabsf(dst.y - 3.0f * 4.0f) < 1e-4f);
        assert(fabsf(dst.z - 4.0f * 5.0f) < 1e-4f);
        assert(fabsf(dst.w - 5.0f * 6.0f) < 1e-4f);
        printf("5 ok\n");
    }

    /* Test 6: Vec4f FMA (a * b + c) */
    {
        struct Vec4f a = { 2.0f, 3.0f, 4.0f, 5.0f };
        struct Vec4f b = { 1.5f, 2.0f, 2.5f, 3.0f };
        struct Vec4f c = { 0.5f, 1.0f, 1.5f, 2.0f };
        struct Vec4f dst = { 0 };
        test_vec4f_fma(&dst, &a, &b, &c);
        assert(fabsf(dst.x - 3.5f) < 1e-4f);
        assert(fabsf(dst.y - 7.0f) < 1e-4f);
        assert(fabsf(dst.z - 11.5f) < 1e-4f);
        assert(fabsf(dst.w - 17.0f) < 1e-4f);
        printf("6 ok\n");
    }

    /* Test 7: Vec2d Compound ((a + b) * (c - d)) */
    {
        struct Vec2d a = { 10.5, 20.5 };
        struct Vec2d b = { 1.5, 2.5 };
        struct Vec2d c = { 8.0, 10.0 };
        struct Vec2d d = { 2.0, 3.0 };
        struct Vec2d dst = { 0 };
        test_vec2d_compound(&dst, &a, &b, &c, &d);
        assert(fabs(dst.x - 12.0 * 6.0) < 1e-6);
        assert(fabs(dst.y - 23.0 * 7.0) < 1e-6);
        printf("7 ok\n");
    }

    /* Test 8: Vec2d Dual Multiply-Add (a * b + c * d) */
    {
        struct Vec2d a = { 3.0, 4.0 };
        struct Vec2d b = { 5.0, 6.0 };
        struct Vec2d c = { 2.0, 3.0 };
        struct Vec2d d = { 7.0, 8.0 };
        struct Vec2d dst = { 0 };
        test_vec2d_mul_add(&dst, &a, &b, &c, &d);
        assert(fabs(dst.x - (15.0 + 14.0)) < 1e-6);
        assert(fabs(dst.y - (24.0 + 24.0)) < 1e-6);
        printf("8 ok\n");
    }

    /* Test 9: Array 4-element int chain ((a + b - c) * d) */
    {
        int a[4] = { 10, 20, 30, 40 };
        int b[4] = { 5, 6, 7, 8 };
        int c[4] = { 2, 3, 4, 5 };
        int d[4] = { 2, 2, 2, 2 };
        int dst[4] = { 0 };
        test_arr4i_chain(dst, a, b, c, d);
        assert(dst[0] == (10 + 5 - 2) * 2);
        assert(dst[1] == (20 + 6 - 3) * 2);
        assert(dst[2] == (30 + 7 - 4) * 2);
        assert(dst[3] == (40 + 8 - 5) * 2);
        printf("9 ok\n");
    }

    /* Test 10: Array 4-element int compound (a * b + c * d) */
    {
        int a[4] = { 1, 2, 3, 4 };
        int b[4] = { 10, 20, 30, 40 };
        int c[4] = { 5, 5, 5, 5 };
        int d[4] = { 2, 3, 4, 5 };
        int dst[4] = { 0 };
        test_arr4i_compound(dst, a, b, c, d);
        assert(dst[0] == 10 + 10);
        assert(dst[1] == 40 + 15);
        assert(dst[2] == 90 + 20);
        assert(dst[3] == 160 + 25);
        printf("10 ok\n");
    }

    printf("ALL SLP TREE TESTS PASSED!\n");
    return 0;
}
