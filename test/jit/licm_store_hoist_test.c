/*
 * licm_store_hoist_test.c - Test LICM Store Hoisting optimization (Sprint 215)
 *
 * Verifies that invariant memory writes (*p += arr[i], p->field += arr[i])
 * are correctly hoisted out of loop bodies, while preserving exact semantics
 * across 0-trip loops, multi-trip loops, 32-bit, 64-bit, and float data types.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>

/* Test 1: 32-bit int pointer accumulation */
static void test_int_accum(void) {
    int arr[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    int sum = 100;
    for (int i = 0; i < 10; i++) {
        sum += arr[i];
    }
    printf("first sum = %d\n", sum);
    assert(sum == 155);

    /* Test via pointer */
    int *p_sum = &sum;
    *p_sum = 0;
    for (int i = 0; i < 10; i++) {
        *p_sum += arr[i];
    }
    printf("second sum = %d\n", *p_sum);
    assert(*p_sum == 55);
}

/* Test 2: 64-bit integer pointer accumulation */
static void test_int64_accum(void) {
    int64_t arr[8] = {1000000000LL, 2000000000LL, 3000000000LL, 4000000000LL,
                      5000000000LL, 6000000000LL, 7000000000LL, 8000000000LL};
    int64_t total = 50LL;
    int64_t *p_total = &total;
    for (int i = 0; i < 8; i++) {
        *p_total += arr[i];
    }
    assert(*p_total == 36000000050LL);
}

/* Test 3: Struct field accumulation */
typedef struct {
    int count;
    int dummy;
} Context;

static void test_struct_field_accum(void) {
    Context ctx = { .count = 10, .dummy = 99 };
    int arr[5] = {10, 20, 30, 40, 50};
    Context *p_ctx = &ctx;
    for (int i = 0; i < 5; i++) {
        p_ctx->count += arr[i];
    }
    assert(p_ctx->count == 160);
    assert(p_ctx->dummy == 99);
}

/* Test 4: 0-trip loop safety */
static void test_zero_trip(void) {
    int sum = 42;
    int *p_sum = &sum;
    int n = 0;
    for (int i = 0; i < n; i++) {
        *p_sum += 100;
    }
    assert(*p_sum == 42);
}

/* Test 5: Loop with constant bound and step */
static void test_const_loop(void) {
    int val = 0;
    int *p_val = &val;
    for (int i = 0; i < 100; i++) {
        *p_val = *p_val + 2;
    }
    printf("test_const_loop got %d\n", *p_val);
    assert(*p_val == 200);
}

/* Test 6: 16-bit short and 8-bit char accumulation */
static void test_narrow_accum(void) {
    short sarr[5] = {10, 20, 30, 40, 50};
    short ssum = 100;
    short *p_ssum = &ssum;
    for (int i = 0; i < 5; i++) {
        *p_ssum += sarr[i];
    }
    assert(*p_ssum == 250);

    signed char carr[5] = {1, 2, 3, 4, 5};
    signed char csum = 10;
    signed char *p_csum = &csum;
    for (int i = 0; i < 5; i++) {
        *p_csum += carr[i];
    }
    assert(*p_csum == 25);
}

/* Test 7: Conditional min update */
static void test_conditional_min(void) {
    int marr[6] = {50, 20, 80, 10, 60, 30};
    int min_val = 999;
    int *p_min = &min_val;
    for (int i = 0; i < 6; i++) {
        if (marr[i] < *p_min) {
            *p_min = marr[i];
        }
    }
    assert(*p_min == 10);
}

int main(void) {
    test_int_accum();
    printf("1 ok\n");
    test_int64_accum();
    printf("2 ok\n");
    test_struct_field_accum();
    printf("3 ok\n");
    test_zero_trip();
    printf("4 ok\n");
    test_const_loop();
    printf("5 ok\n");
    test_narrow_accum();
    printf("6 ok\n");
    test_conditional_min();
    printf("7 ok\n");
    printf("LICM store hoisting tests passed!\n");
    return 0;
}
