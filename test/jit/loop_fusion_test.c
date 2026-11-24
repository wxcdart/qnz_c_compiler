/*
 * loop_fusion_test.c - Test Loop Fusion / Jamming optimization (Sprint 215)
 *
 * Verifies that adjacent loops with identical iteration spaces and compatible
 * dependencies are correctly fused into a single loop, and execute accurately
 * across -O0, -O1, -O2, -O3 in both JIT and AOT modes.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>

#define N 32

/* Test 1: Adjacent independent array updates */
static void test_independent_loops(void) {
    int a[N];
    int b[N];

    /* Initialize */
    for (int i = 0; i < N; i++) {
        a[i] = i * 2;
    }
    for (int i = 0; i < N; i++) {
        b[i] = i * 3;
    }

    /* Verify contents */
    for (int i = 0; i < N; i++) {
        assert(a[i] == i * 2);
        assert(b[i] == i * 3);
    }
}

/* Test 2: Adjacent producer-consumer loops */
static void test_producer_consumer(void) {
    int src[N];
    int mid[N];
    int dst[N];

    for (int i = 0; i < N; i++) {
        src[i] = i + 1;
    }

    /* Producer loop */
    for (int i = 0; i < N; i++) {
        mid[i] = src[i] * 10;
    }

    /* Consumer loop */
    for (int i = 0; i < N; i++) {
        dst[i] = mid[i] + 5;
    }

    for (int i = 0; i < N; i++) {
        assert(dst[i] == (i + 1) * 10 + 5);
    }
}

/* Test 3: Multiple adjacent loops */
static void test_three_loops(void) {
    int x[16];
    int y[16];
    int z[16];

    for (int i = 0; i < 16; i++) {
        x[i] = i + 10;
    }
    for (int i = 0; i < 16; i++) {
        y[i] = i + 20;
    }
    for (int i = 0; i < 16; i++) {
        z[i] = x[i] + y[i];
    }

    for (int i = 0; i < 16; i++) {
        assert(z[i] == (i + 10) + (i + 20));
    }
}

/* Test 4: Loops with different bounds (must NOT be fused, must preserve correct semantics) */
static void test_different_bounds(void) {
    int a[20];
    int b[10];

    for (int i = 0; i < 20; i++) {
        a[i] = 1;
    }
    for (int i = 0; i < 10; i++) {
        b[i] = 2;
    }

    for (int i = 0; i < 20; i++) {
        assert(a[i] == 1);
    }
    for (int i = 0; i < 10; i++) {
        assert(b[i] == 2);
    }
}

int main(void) {
    test_independent_loops();
    test_producer_consumer();
    test_three_loops();
    test_different_bounds();
    printf("Loop fusion tests passed!\n");
    return 0;
}
