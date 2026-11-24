/* Test suite for SLP (Superword-Level Parallelism) Vectorization */
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <math.h>

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

/* 1. Struct Vec4i addition */
void test_vec4i_add(struct Vec4i *dst, const struct Vec4i *a, const struct Vec4i *b) {
    dst->x = a->x + b->x;
    dst->y = a->y + b->y;
    dst->z = a->z + b->z;
    dst->w = a->w + b->w;
}

/* 2. Struct Vec4i subtraction */
void test_vec4i_sub(struct Vec4i *dst, const struct Vec4i *a, const struct Vec4i *b) {
    dst->x = a->x - b->x;
    dst->y = a->y - b->y;
    dst->z = a->z - b->z;
    dst->w = a->w - b->w;
}

/* 3. Struct Vec4i multiplication */
void test_vec4i_mul(struct Vec4i *dst, const struct Vec4i *a, const struct Vec4i *b) {
    dst->x = a->x * b->x;
    dst->y = a->y * b->y;
    dst->z = a->z * b->z;
    dst->w = a->w * b->w;
}

/* 4. Struct Vec4f addition */
void test_vec4f_add(struct Vec4f *dst, const struct Vec4f *a, const struct Vec4f *b) {
    dst->x = a->x + b->x;
    dst->y = a->y + b->y;
    dst->z = a->z + b->z;
    dst->w = a->w + b->w;
}

/* 5. Struct Vec4f multiplication */
void test_vec4f_mul(struct Vec4f *dst, const struct Vec4f *a, const struct Vec4f *b) {
    dst->x = a->x * b->x;
    dst->y = a->y * b->y;
    dst->z = a->z * b->z;
    dst->w = a->w * b->w;
}

/* 6. 4-element int array addition */
void test_arr4i_add(int *dst, const int *a, const int *b) {
    dst[0] = a[0] + b[0];
    dst[1] = a[1] + b[1];
    dst[2] = a[2] + b[2];
    dst[3] = a[3] + b[3];
}

/* 7. 4-element int vector copy */
void test_arr4i_copy(int *dst, const int *a) {
    dst[0] = a[0];
    dst[1] = a[1];
    dst[2] = a[2];
    dst[3] = a[3];
}

/* 8. 2-element double vector addition */
void test_vec2d_add(struct Vec2d *dst, const struct Vec2d *a, const struct Vec2d *b) {
    dst->x = a->x + b->x;
    dst->y = a->y + b->y;
}

/* 9. 4-element int array with scalar broadcast */
void test_arr4i_broadcast(int *dst, const int *a, int val) {
    dst[0] = a[0] + val;
    dst[1] = a[1] + val;
    dst[2] = a[2] + val;
    dst[3] = a[3] + val;
}

int main(void) {
    /* Test 1: Vec4i addition */
    {
        struct Vec4i a = { 10, 20, 30, 40 };
        struct Vec4i b = { 1, 2, 3, 4 };
        struct Vec4i dst = { 0 };
        test_vec4i_add(&dst, &a, &b);
        if (dst.x != 11 || dst.y != 22 || dst.z != 33 || dst.w != 44) {
            printf("FAIL test_vec4i_add: got (%d, %d, %d, %d)\n", dst.x, dst.y, dst.z, dst.w);
            return 1;
        }
    }

    /* Test 2: Vec4i subtraction */
    {
        struct Vec4i a = { 50, 40, 30, 20 };
        struct Vec4i b = { 5, 4, 3, 2 };
        struct Vec4i dst = { 0 };
        test_vec4i_sub(&dst, &a, &b);
        if (dst.x != 45 || dst.y != 36 || dst.z != 27 || dst.w != 18) {
            printf("FAIL test_vec4i_sub: got (%d, %d, %d, %d)\n", dst.x, dst.y, dst.z, dst.w);
            return 2;
        }
    }

    /* Test 3: Vec4i multiplication */
    {
        struct Vec4i a = { 2, 3, 4, 5 };
        struct Vec4i b = { 10, 20, 30, 40 };
        struct Vec4i dst = { 0 };
        test_vec4i_mul(&dst, &a, &b);
        if (dst.x != 20 || dst.y != 60 || dst.z != 120 || dst.w != 200) {
            printf("FAIL test_vec4i_mul: got (%d, %d, %d, %d)\n", dst.x, dst.y, dst.z, dst.w);
            return 3;
        }
    }

    /* Test 4: Vec4f addition */
    {
        struct Vec4f a = { 1.5f, 2.5f, 3.5f, 4.5f };
        struct Vec4f b = { 0.5f, 0.5f, 0.5f, 0.5f };
        struct Vec4f dst = { 0 };
        test_vec4f_add(&dst, &a, &b);
        if (fabsf(dst.x - 2.0f) > 1e-4f || fabsf(dst.y - 3.0f) > 1e-4f ||
            fabsf(dst.z - 4.0f) > 1e-4f || fabsf(dst.w - 5.0f) > 1e-4f) {
            printf("FAIL test_vec4f_add: got (%f, %f, %f, %f)\n", dst.x, dst.y, dst.z, dst.w);
            return 4;
        }
    }

    /* Test 5: Vec4f multiplication */
    {
        struct Vec4f a = { 2.0f, 3.0f, 4.0f, 5.0f };
        struct Vec4f b = { 1.5f, 2.0f, 2.5f, 3.0f };
        struct Vec4f dst = { 0 };
        test_vec4f_mul(&dst, &a, &b);
        if (fabsf(dst.x - 3.0f) > 1e-4f || fabsf(dst.y - 6.0f) > 1e-4f ||
            fabsf(dst.z - 10.0f) > 1e-4f || fabsf(dst.w - 15.0f) > 1e-4f) {
            printf("FAIL test_vec4f_mul: got (%f, %f, %f, %f)\n", dst.x, dst.y, dst.z, dst.w);
            return 5;
        }
    }

    /* Test 6: 4-element int array addition */
    {
        int a[4] = { 100, 200, 300, 400 };
        int b[4] = { 1, 2, 3, 4 };
        int dst[4] = { 0 };
        test_arr4i_add(dst, a, b);
        if (dst[0] != 101 || dst[1] != 202 || dst[2] != 303 || dst[3] != 404) {
            printf("FAIL test_arr4i_add: got (%d, %d, %d, %d)\n", dst[0], dst[1], dst[2], dst[3]);
            return 6;
        }
    }

    /* Test 7: 4-element int vector copy */
    {
        int a[4] = { 42, 84, 126, 168 };
        int dst[4] = { 0 };
        test_arr4i_copy(dst, a);
        if (dst[0] != 42 || dst[1] != 84 || dst[2] != 126 || dst[3] != 168) {
            printf("FAIL test_arr4i_copy: got (%d, %d, %d, %d)\n", dst[0], dst[1], dst[2], dst[3]);
            return 7;
        }
    }

    /* Test 8: 2-element double vector addition */
    {
        struct Vec2d a = { 1.25, 2.75 };
        struct Vec2d b = { 3.50, 4.25 };
        struct Vec2d dst = { 0 };
        test_vec2d_add(&dst, &a, &b);
        if (fabs(dst.x - 4.75) > 1e-6 || fabs(dst.y - 7.00) > 1e-6) {
            printf("FAIL test_vec2d_add: got (%f, %f)\n", dst.x, dst.y);
            return 8;
        }
    }

    /* Test 9: 4-element int array with scalar broadcast */
    {
        int a[4] = { 10, 20, 30, 40 };
        int dst[4] = { 0 };
        test_arr4i_broadcast(dst, a, 7);
        if (dst[0] != 17 || dst[1] != 27 || dst[2] != 37 || dst[3] != 47) {
            printf("FAIL test_arr4i_broadcast: got (%d, %d, %d, %d)\n", dst[0], dst[1], dst[2], dst[3]);
            return 9;
        }
    }

    printf("PASS: SLP Auto-Vectorization tests succeeded\n");
    return 0;
}
