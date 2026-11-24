/*
 * loop_unswitch_test.c - Test Loop Unswitching optimization pass (Sprint 216)
 *
 * Verifies that loops containing loop-invariant conditional branches (if (flag),
 * if (mode == 1)) are correctly unswitched into specialized loop variants,
 * eliminating per-iteration branch overhead and preserving exact semantics across
 * -O0, -O1, -O2, -O3 in both JIT and AOT modes.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>

#define N 32

/* Test 1: Invariant flag controlling arithmetic operation */
static void test_unswitch_flag(int flag) {
    int a[N];
    int b[N];
    for (int i = 0; i < N; i++) {
        b[i] = i * 2;
        a[i] = 0;
    }

    for (int i = 0; i < N; i++) {
        if (flag) {
            a[i] = b[i] + 10;
        } else {
            a[i] = b[i] - 10;
        }
    }

    for (int i = 0; i < N; i++) {
        if (flag) {
            assert(a[i] == i * 2 + 10);
        } else {
            assert(a[i] == i * 2 - 10);
        }
    }
}

/* Test 2: Invariant mode parameter with multiple values */
static int test_unswitch_mode(int *arr, int n, int mode) {
    int sum = 0;
    for (int i = 0; i < n; i++) {
        if (mode == 1) {
            sum += arr[i] * 2;
        } else {
            sum += arr[i];
        }
    }
    return sum;
}

/* Test 3: Invariant condition with pointer updates */
static void test_unswitch_stride(int *dst, const int *src, int n, int add_offset) {
    for (int i = 0; i < n; i++) {
        if (add_offset) {
            dst[i] = src[i] + 100;
        } else {
            dst[i] = src[i];
        }
    }
}

int main(void) {
    /* Test 1: Both flag paths */
    test_unswitch_flag(1);
    test_unswitch_flag(0);

    /* Test 2: Mode paths */
    int data[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    int s1 = test_unswitch_mode(data, 8, 1);
    int s0 = test_unswitch_mode(data, 8, 0);
    assert(s1 == 72); /* 36 * 2 */
    assert(s0 == 36);

    /* Test 3: Stride/offset path */
    int dst[8];
    test_unswitch_stride(dst, data, 8, 1);
    for (int i = 0; i < 8; i++) {
        assert(dst[i] == data[i] + 100);
    }
    test_unswitch_stride(dst, data, 8, 0);
    for (int i = 0; i < 8; i++) {
        assert(dst[i] == data[i]);
    }

    printf("Loop unswitching tests passed!\n");
    return 0;
}
