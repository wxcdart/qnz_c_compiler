#include <stdio.h>
#include <stdlib.h>

/* Test 1: Completely empty pure loop */
static int test_empty_loop(int n) {
    int x = 42;
    for (int i = 0; i < n; i++) {
        /* empty */
    }
    return x;
}

/* Test 2: Pure loop with dead computations */
static int test_dead_arith_loop(int n) {
    int res = 100;
    for (int i = 0; i < n; i++) {
        int a = i * 7 + 3;
        int b = a ^ 0x55;
        (void)b;
    }
    return res;
}

/* Test 3: Dead accumulator loop */
static int test_dead_accumulator(int n) {
    int dead_sum = 0;
    for (int i = 0; i < n; i++) {
        dead_sum += i;
    }
    return 777;
}

/* Test 4: Loop with side effects - MUST NOT be deleted! */
static int g_side_effect = 0;
static void record_side_effect(int val) {
    g_side_effect += val;
}

static int test_side_effect_loop(int n) {
    g_side_effect = 0;
    for (int i = 0; i < n; i++) {
        record_side_effect(1);
    }
    return g_side_effect;
}

/* Test 5: Loop writing to memory - MUST NOT be deleted! */
static int test_live_store_loop(int n) {
    int arr[10];
    for (int i = 0; i < 10; i++) arr[i] = 0;
    for (int i = 0; i < n && i < 10; i++) {
        arr[i] = i * 2;
    }
    return arr[3]; /* should be 6 */
}

int main(void) {
    if (test_empty_loop(100) != 42) {
        printf("FAIL: test_empty_loop\n");
        return 1;
    }

    if (test_dead_arith_loop(50) != 100) {
        printf("FAIL: test_dead_arith_loop\n");
        return 2;
    }

    if (test_dead_accumulator(1000) != 777) {
        printf("FAIL: test_dead_accumulator\n");
        return 3;
    }

    if (test_side_effect_loop(5) != 5) {
        printf("FAIL: test_side_effect_loop: %d != 5\n", test_side_effect_loop(5));
        return 4;
    }

    if (test_live_store_loop(10) != 6) {
        printf("FAIL: test_live_store_loop: %d != 6\n", test_live_store_loop(10));
        return 5;
    }

    printf("PASS: loop_delete_test\n");
    return 0;
}
