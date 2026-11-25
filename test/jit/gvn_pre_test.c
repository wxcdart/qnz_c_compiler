/*
 * gvn_pre_test.c - Test Cross-Block GVN, Diamond Hoisting, PRE & Cross-Block Memory Forwarding
 *
 * Verifies Sprint 226 optimizations across -O0..-O3 in JIT and AOT:
 * 1. Diamond Hoisting: Identical expressions in both branches of an if-else are hoisted.
 * 2. Dominator-Tree GVN: Expressions computed in dominators are eliminated in dominated blocks.
 * 3. Commutative GVN: Commutative ops (a + b == b + a) match across blocks.
 * 4. Partial Redundancy Elimination (PRE): Partially redundant computations across merge points.
 * 5. Cross-Block Memory Forwarding: Redundant loads across control flow splits without intervening stores.
 * 6. Multi-Diamond and Nested Conditionals: Complex control flow graphs with hoisted subexpressions.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>

/* Test 1: Diamond Hoisting */
static int test_diamond(int cond, int a, int b) {
    int x, y;
    if (cond > 0) {
        x = (a * b) + 10;
    } else {
        y = (a * b) + 20;
    }
    return cond > 0 ? x : y;
}

/* Test 2: Dominator-Tree GVN */
static int test_dom_gvn(int cond, int a, int b) {
    int v1 = (a * 7) + b;
    int res = 0;
    if (cond > 5) {
        int v2 = (a * 7) + b; /* Dominated by entry! */
        res = v2 * 2;
    } else {
        int v3 = (a * 7) + b; /* Dominated by entry! */
        res = v3 + 100;
    }
    return res + v1;
}

/* Test 3: Commutative Dominator GVN */
static int test_commutative_gvn(int cond, int a, int b) {
    int v1 = a + b;
    int res = 0;
    if (cond) {
        int v2 = b + a; /* Commutative duplicate */
        res = v2 * 3;
    } else {
        int v3 = b + a; /* Commutative duplicate */
        res = v3 - 5;
    }
    return res + v1;
}

/* Test 4: Partial Redundancy Elimination (PRE) */
static int test_pre(int cond, int a, int b) {
    int x = 0;
    if (cond) {
        x = a * b;
    }
    /* At merge point: a * b is partially redundant (already computed on cond=true path) */
    int y = a * b;
    return x + y;
}

/* Test 5: Cross-Block Memory Forwarding */
static int test_mem_forward(int cond, const int *p) {
    int val1 = *p;
    int res = 0;
    if (cond > 10) {
        int val2 = *p; /* Dominator load - no intervening stores or calls! */
        res = val2 * 4;
    } else {
        int val3 = *p; /* Dominator load */
        res = val3 + 7;
    }
    return res + val1;
}

/* Test 6: Nested Diamonds with Multiple Hoistable Expressions */
static int test_nested_diamonds(int c1, int c2, int a, int b) {
    int res = 0;
    if (c1) {
        if (c2) {
            res = (a * 13 + b) + 1;
        } else {
            res = (a * 13 + b) + 2;
        }
    } else {
        res = (a * 13 + b) + 3;
    }
    return res;
}

/* Test 7: Bitwise & Shift Operations across Diamonds */
static int test_bitwise_diamond(int cond, int a, int b) {
    int r;
    if (cond) {
        r = ((a << 3) ^ (b >> 1)) | 0xAA;
    } else {
        r = ((a << 3) ^ (b >> 1)) & 0x55;
    }
    return r;
}

int main(void) {
    /* Test 1 */
    assert(test_diamond(1, 3, 4) == 22);   /* (3 * 4) + 10 = 22 */
    assert(test_diamond(0, 3, 4) == 32);   /* (3 * 4) + 20 = 32 */

    /* Test 2 */
    /* v1 = 2 * 7 + 5 = 19 */
    /* cond > 5: 19 * 2 + 19 = 57 */
    /* cond <= 5: 19 + 100 + 19 = 138 */
    assert(test_dom_gvn(10, 2, 5) == 57);
    assert(test_dom_gvn(2, 2, 5) == 138);

    /* Test 3 */
    /* v1 = 10 + 20 = 30 */
    /* cond: 30 * 3 + 30 = 120 */
    /* !cond: 30 - 5 + 30 = 55 */
    assert(test_commutative_gvn(1, 10, 20) == 120);
    assert(test_commutative_gvn(0, 10, 20) == 55);

    /* Test 4 */
    /* cond=1: x = 15, y = 15 -> 30 */
    /* cond=0: x = 0, y = 15 -> 15 */
    assert(test_pre(1, 3, 5) == 30);
    assert(test_pre(0, 3, 5) == 15);

    /* Test 5 */
    int val = 42;
    /* cond=20: 42 * 4 + 42 = 210 */
    /* cond=5: 42 + 7 + 42 = 91 */
    assert(test_mem_forward(20, &val) == 210);
    assert(test_mem_forward(5, &val) == 91);

    /* Test 6 */
    /* a * 13 + b = 2 * 13 + 3 = 29 */
    assert(test_nested_diamonds(1, 1, 2, 3) == 30);
    assert(test_nested_diamonds(1, 0, 2, 3) == 31);
    assert(test_nested_diamonds(0, 0, 2, 3) == 32);

    /* Test 7 */
    /* a = 10 -> a << 3 = 80; b = 6 -> b >> 1 = 3; 80 ^ 3 = 83 (0x53) */
    /* 0x53 | 0xAA = 0xFB (251) */
    /* 0x53 & 0x55 = 0x51 (81) */
    assert(test_bitwise_diamond(1, 10, 6) == 251);
    assert(test_bitwise_diamond(0, 10, 6) == 81);

    printf("ALL GVN / PRE / DIAMOND TESTS PASSED!\n");
    return 0;
}
