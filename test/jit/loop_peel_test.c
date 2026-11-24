/*
 * loop_peel_test.c - Test Loop Peeling optimization pass (Sprint 216)
 *
 * Verifies that loops with initial boundary conditions (e.g. if (i == 0))
 * have their initial iteration peeled into the preheader, resolving the
 * boundary check and enabling straight-line code in the remaining loop body.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>

#define N 16

/* Test 1: First-iteration initial value assignment */
static void test_first_iteration_special(void) {
    int arr[N];
    for (int i = 0; i < N; i++) {
        if (i == 0) {
            arr[i] = 1000;
        } else {
            arr[i] = arr[i - 1] + 1;
        }
    }

    assert(arr[0] == 1000);
    for (int i = 1; i < N; i++) {
        assert(arr[i] == 1000 + i);
    }
}

/* Test 2: Accumulator with first-iteration reset */
static int test_peel_accum(const int *src, int n) {
    int total = 0;
    for (int i = 0; i < n; i++) {
        if (i == 0) {
            total = src[i] * 5;
        } else {
            total += src[i];
        }
    }
    return total;
}

/* Test 3: 0-trip loop safety */
static void test_zero_trip(void) {
    int count = 99;
    int n = 0;
    for (int i = 0; i < n; i++) {
        if (i == 0) {
            count = 500;
        } else {
            count += 1;
        }
    }
    assert(count == 99);
}

int main(void) {
    test_first_iteration_special();

    int data[5] = {10, 20, 30, 40, 50};
    int res = test_peel_accum(data, 5);
    /* 10*5 + 20 + 30 + 40 + 50 = 50 + 140 = 190 */
    assert(res == 190);

    test_zero_trip();

    printf("Loop peeling tests passed!\n");
    return 0;
}
