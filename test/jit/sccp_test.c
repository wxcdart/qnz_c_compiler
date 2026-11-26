#include <stdio.h>
#include <stdlib.h>

static int dead_block_executed = 0;

__attribute__((noinline))
static void bad_path(void) {
    dead_block_executed = 1;
}

static int test_constant_branch_folding(void) {
    int x = 42;
    int y = 58;
    int sum = x + y; /* 100 */
    int res = 0;

    if (sum == 100) {
        res = 77;
    } else {
        bad_path();
        res = 999;
    }

    if (dead_block_executed) {
        printf("FAIL: dead block executed!\n");
        return 1;
    }
    if (res != 77) {
        printf("FAIL: res = %d, expected 77\n", res);
        return 1;
    }
    return 0;
}

static int test_dead_loop(void) {
    int sum = 5;
    while (0) {
        sum += 100;
        bad_path();
    }
    if (sum != 5 || dead_block_executed) {
        printf("FAIL: dead loop modified sum or executed\n");
        return 1;
    }
    return 0;
}

static int test_annihilators(int dynamic_val) {
    /* x * 0 = 0 and x & 0 = 0 should fold even if x is dynamic (BOT) */
    int a = dynamic_val * 0;
    int b = dynamic_val & 0;
    if (a != 0 || b != 0) {
        printf("FAIL: annihilator failed a=%d b=%d\n", a, b);
        return 1;
    }
    return 0;
}

static int test_multi_block_propagation(int flag) {
    int a = 15;
    int b = 25;
    int c = a + b; /* 40 */
    int result = 0;

    if (flag) {
        result = c * 2; /* 80 */
    } else {
        result = c + 10; /* 50 */
    }

    if (flag && result != 80) return 1;
    if (!flag && result != 50) return 1;
    return 0;
}

int main(int argc, char **argv) {
    (void)argv;
    if (test_constant_branch_folding() != 0) return 1;
    if (test_dead_loop() != 0) return 2;
    if (test_annihilators(argc) != 0) return 3;
    if (test_multi_block_propagation(1) != 0) return 4;
    if (test_multi_block_propagation(0) != 0) return 5;

    printf("SCCP tests passed successfully!\n");
    return 0;
}
