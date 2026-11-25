#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#define ASSERT_EQ(a, b) do { \
    if ((a) != (b)) { \
        fprintf(stderr, "Assertion failed: %s == %s (line %d): got %lld, expected %lld\n", \
                #a, #b, __LINE__, (long long)(a), (long long)(b)); \
        exit(1); \
    } \
} while (0)

#define ASSERT_FLOAT_EQ(a, b) do { \
    double diff = fabs((double)(a) - (double)(b)); \
    if (diff > 1e-4) { \
        fprintf(stderr, "Assertion failed: %s ~= %s (line %d): got %f, expected %f\n", \
                #a, #b, __LINE__, (double)(a), (double)(b)); \
        exit(1); \
    } \
} while (0)

/* Target functions for devirtualization */
static inline int add5(int x) {
    return x + 5;
}

static inline int sub2(int x) {
    return x - 2;
}

static inline int mul3(int x) {
    return x * 3;
}

static inline int square(int x) {
    return x * x;
}

static inline int cube(int x) {
    return x * x * x;
}

/* Global function pointers */
static int (*g_fp1)(int) = mul3;
int (*g_fp2)(int) = add5;

/* Higher-order function for inlining + devirtualization */
static inline int apply_fn(int (*fn)(int), int val) {
    return fn(val) + 1;
}

static inline int apply_twice(int (*fn)(int), int val) {
    return fn(fn(val));
}

/* 4-level nested inlining */
static inline int nest_l4(int x) {
    return x + 10;
}

static inline int nest_l3(int x) {
    return nest_l4(x) * 2;
}

static inline int nest_l2(int x) {
    return nest_l3(x) - 4;
}

static inline int nest_l1(int x) {
    return nest_l2(x) + 3;
}

/* Float / Double inlining */
static inline float lerpf(float a, float b, float t) {
    return a + (b - a) * t;
}

static inline double d_poly(double x, double c) {
    return x * x + c;
}

int main(void) {
    /* 1. Direct address-of & dereference calls */
    int r1 = (&add5)(10);
    ASSERT_EQ(r1, 15);

    int r2 = (*sub2)(20);
    ASSERT_EQ(r2, 18);

    int r3 = (**mul3)(4);
    ASSERT_EQ(r3, 12);

    /* 2. Local function pointer variables */
    int (*fp_local)(int) = add5;
    ASSERT_EQ(fp_local(7), 12);

    fp_local = sub2;
    ASSERT_EQ(fp_local(7), 5);

    fp_local = &square;
    ASSERT_EQ(fp_local(6), 36);

    /* 3. Global function pointers */
    ASSERT_EQ(g_fp1(8), 24);
    ASSERT_EQ(g_fp2(8), 13);

    /* 4. Inlining + Parameter Devirtualization */
    int r_app1 = apply_fn(square, 5); /* (5 * 5) + 1 = 26 */
    ASSERT_EQ(r_app1, 26);

    int r_app2 = apply_fn(cube, 3); /* (3 * 3 * 3) + 1 = 28 */
    ASSERT_EQ(r_app2, 28);

    int r_app3 = apply_twice(add5, 10); /* add5(add5(10)) = 20 */
    ASSERT_EQ(r_app3, 20);

    /* 5. 4-level deep inlining */
    /* x=5 -> l4=15 -> l3=30 -> l2=26 -> l1=29 */
    int r_nest = nest_l1(5);
    ASSERT_EQ(r_nest, 29);

    /* 6. Float / Double inlining */
    float fl_res = lerpf(10.0f, 20.0f, 0.25f);
    ASSERT_FLOAT_EQ(fl_res, 12.5f);

    double db_res = d_poly(3.0, 1.5);
    ASSERT_FLOAT_EQ(db_res, 10.5);

    printf("PASS: devirtualize_inlining_test\n");
    return 0;
}
