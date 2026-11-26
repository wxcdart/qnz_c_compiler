#include <stdio.h>
#include <stdlib.h>

/* Test 1: Simple if-then-else diamond */
static int test_diamond_simple(int cond) {
    int x;
    if (cond) {
        x = 42;
    } else {
        x = 99;
    }
    return x + 1;
}

/* Test 2: Diamond with multiple variables */
static int test_diamond_multi(int cond) {
    int a, b;
    if (cond > 5) {
        a = 10;
        b = 20;
    } else {
        a = 30;
        b = 40;
    }
    return a + b;
}

/* Test 3: Nested diamonds */
static int test_nested_diamonds(int a, int b) {
    int res;
    if (a > 0) {
        if (b > 0) {
            res = 1;
        } else {
            res = 2;
        }
    } else {
        if (b > 0) {
            res = 3;
        } else {
            res = 4;
        }
    }
    return res;
}

/* Test 4: Diamond inside a loop */
static int test_diamond_in_loop(int n) {
    int sum = 0;
    for (int i = 0; i < n; i++) {
        int term;
        if (i % 2 == 0) {
            term = i * 2;
        } else {
            term = i * 3;
        }
        sum += term;
    }
    return sum;
}

int main(void) {
    if (test_diamond_simple(1) != 43) {
        printf("FAIL: test_diamond_simple(1) = %d != 43\n", test_diamond_simple(1));
        return 1;
    }
    if (test_diamond_simple(0) != 100) {
        printf("FAIL: test_diamond_simple(0) = %d != 100\n", test_diamond_simple(0));
        return 2;
    }

    if (test_diamond_multi(10) != 30) {
        printf("FAIL: test_diamond_multi(10) = %d != 30\n", test_diamond_multi(10));
        return 3;
    }
    if (test_diamond_multi(2) != 70) {
        printf("FAIL: test_diamond_multi(2) = %d != 70\n", test_diamond_multi(2));
        return 4;
    }

    if (test_nested_diamonds(1, 1) != 1 ||
        test_nested_diamonds(1, -1) != 2 ||
        test_nested_diamonds(-1, 1) != 3 ||
        test_nested_diamonds(-1, -1) != 4) {
        printf("FAIL: test_nested_diamonds\n");
        return 5;
    }

    /* 0: sum = 0
     * 1: i=1 -> term=3 -> sum=3
     * 2: i=2 -> term=4 -> sum=7
     * 3: i=3 -> term=9 -> sum=16
     * 4: i=4 -> term=8 -> sum=24
     */
    if (test_diamond_in_loop(5) != 24) {
        printf("FAIL: test_diamond_in_loop(5) = %d != 24\n", test_diamond_in_loop(5));
        return 6;
    }

    printf("PASS: mem2reg_join_test\n");
    return 0;
}
