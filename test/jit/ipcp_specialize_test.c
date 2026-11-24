/* Test Suite: Function Cloning & Inter-Procedural Constant Propagation (IPCP)
 * Sprint 217: Specialize static functions called with constant arguments.
 */

#include <stdio.h>
#include <stdlib.h>

#define ASSERT_EQ(a, b) do { \
    if ((a) != (b)) { \
        printf("FAIL: %s:%d: %lld != %lld\n", __FILE__, __LINE__, (long long)(a), (long long)(b)); \
        exit(1); \
    } \
} while (0)

/* Static function with control flow branching on constant 'op' flag */
static int apply_op(int op, int a, int b) {
    if (op == 1) {
        return a + b;
    } else if (op == 2) {
        return a * b;
    } else if (op == 3) {
        return a - b;
    } else {
        return a ^ b;
    }
}

/* Call sites with compile-time constants */
int test_add(int a, int b) { return apply_op(1, a, b); }
int test_mul(int a, int b) { return apply_op(2, a, b); }
int test_sub(int a, int b) { return apply_op(3, a, b); }
int test_xor(int a, int b) { return apply_op(0, a, b); }

/* Static function with constant loop trip count */
static int loop_accum(int count, int base) {
    int sum = base;
    for (int i = 0; i < count; i++) {
        sum += (i * 2 + 1);
    }
    return sum;
}

int test_loop_4(int base) { return loop_accum(4, base); }
int test_loop_8(int base) { return loop_accum(8, base); }

/* Static function with multiple constant arguments */
static int multi_const(int flag, int scale, int val) {
    if (flag) {
        return val * scale + 10;
    } else {
        return val - scale;
    }
}

int test_multi_1(int v) { return multi_const(1, 5, v); }
int test_multi_2(int v) { return multi_const(0, 3, v); }

int main(void) {
    /* Test branching specialization */
    ASSERT_EQ(test_add(10, 20), 30);
    ASSERT_EQ(test_mul(6, 7), 42);
    ASSERT_EQ(test_sub(50, 15), 35);
    ASSERT_EQ(test_xor(0xFF, 0x0F), 0xF0);

    /* Test loop trip count specialization */
    /* count=4: sum = 0 + 1 + 3 + 5 + 7 = 16 */
    ASSERT_EQ(test_loop_4(0), 16);
    ASSERT_EQ(test_loop_4(100), 116);
    /* count=8: sum = 0 + 1 + 3 + 5 + 7 + 9 + 11 + 13 + 15 = 64 */
    ASSERT_EQ(test_loop_8(0), 64);
    ASSERT_EQ(test_loop_8(50), 114);

    /* Test multi-parameter specialization */
    ASSERT_EQ(test_multi_1(4), 4 * 5 + 10);
    ASSERT_EQ(test_multi_2(20), 20 - 3);

    printf("ipcp_specialize_test: PASS\n");
    return 0;
}
