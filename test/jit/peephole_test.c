#include <stdio.h>
#include <stdlib.h>

/* Self-identities */
static int test_sub_self(int x) { return x - x; }
static int test_xor_self(int x) { return x ^ x; }
static int test_and_self(int x) { return x & x; }
static int test_or_self(int x) { return x | x; }

/* Neutral elements */
static int test_add_zero(int x) { return x + 0; }
static int test_zero_add(int x) { return 0 + x; }
static int test_sub_zero(int x) { return x - 0; }
static int test_zero_sub(int x) { return 0 - x; }
static int test_mul_one(int x) { return x * 1; }
static int test_one_mul(int x) { return 1 * x; }
static int test_div_one(int x) { return x / 1; }
static int test_or_zero(int x) { return x | 0; }
static int test_xor_zero(int x) { return x ^ 0; }
static int test_and_neg_one(int x) { return x & -1; }
static int test_shl_zero(int x) { return x << 0; }
static int test_shr_zero(int x) { return x >> 0; }

/* Annihilators */
static int test_mul_zero(int x) { return x * 0; }
static int test_zero_mul(int x) { return 0 * x; }
static int test_and_zero(int x) { return x & 0; }
static int test_zero_and(int x) { return 0 & x; }

/* Inverses */
static int test_double_neg(int x) {
    int y = -x;
    return -y;
}

static int test_double_not(int x) {
    int y = ~x;
    return ~y;
}

int main(void) {
    int v = 12345;

    if (test_sub_self(v) != 0) { printf("FAIL: test_sub_self\n"); return 1; }
    if (test_xor_self(v) != 0) { printf("FAIL: test_xor_self\n"); return 2; }
    if (test_and_self(v) != v) { printf("FAIL: test_and_self\n"); return 3; }
    if (test_or_self(v) != v) { printf("FAIL: test_or_self\n"); return 4; }

    if (test_add_zero(v) != v) { printf("FAIL: test_add_zero\n"); return 5; }
    if (test_zero_add(v) != v) { printf("FAIL: test_zero_add\n"); return 6; }
    if (test_sub_zero(v) != v) { printf("FAIL: test_sub_zero\n"); return 7; }
    if (test_zero_sub(v) != -v) { printf("FAIL: test_zero_sub\n"); return 8; }
    if (test_mul_one(v) != v) { printf("FAIL: test_mul_one\n"); return 9; }
    if (test_one_mul(v) != v) { printf("FAIL: test_one_mul\n"); return 10; }
    if (test_div_one(v) != v) { printf("FAIL: test_div_one\n"); return 11; }
    if (test_or_zero(v) != v) { printf("FAIL: test_or_zero\n"); return 12; }
    if (test_xor_zero(v) != v) { printf("FAIL: test_xor_zero\n"); return 13; }
    if (test_and_neg_one(v) != v) { printf("FAIL: test_and_neg_one\n"); return 14; }
    if (test_shl_zero(v) != v) { printf("FAIL: test_shl_zero\n"); return 15; }
    if (test_shr_zero(v) != v) { printf("FAIL: test_shr_zero\n"); return 16; }

    if (test_mul_zero(v) != 0) { printf("FAIL: test_mul_zero\n"); return 17; }
    if (test_zero_mul(v) != 0) { printf("FAIL: test_zero_mul\n"); return 18; }
    if (test_and_zero(v) != 0) { printf("FAIL: test_and_zero\n"); return 19; }
    if (test_zero_and(v) != 0) { printf("FAIL: test_zero_and\n"); return 20; }

    if (test_double_neg(v) != v) { printf("FAIL: test_double_neg\n"); return 21; }
    if (test_double_not(v) != v) { printf("FAIL: test_double_not\n"); return 22; }

    printf("PASS: peephole_test\n");
    return 0;
}
