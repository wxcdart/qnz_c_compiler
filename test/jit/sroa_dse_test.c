/*
 * test/jit/sroa_dse_test.c
 *
 * Sprint 231: Full Intra-Function SROA & Dead Store Elimination (DSE)
 * [Items #13 & #14 from optimization taxonomy]
 *
 * Tests:
 * 1. SROA on 2D Point struct (p.x, p.y) - promoted to registers.
 * 2. SROA on multi-typed struct (int, long, double, float).
 * 3. SROA with compound assignments (p.x += 5, p.y *= 2).
 * 4. SROA with pre/post increment and decrement (p.x++, ++p.y).
 * 5. SROA inside loops (struct member as accumulator/induction variable).
 * 6. DSE on overwritten scalar stores (earlier write dead).
 * 7. DSE on overwritten struct member stores.
 * 8. DSE on stores dead at function return (written, never read).
 * 9. DSE across control flow branches.
 * 10. Soundness verification: memory reads correctly observe live writes.
 */

#include <assert.h>
#include <stdio.h>

struct Point {
    int x;
    int y;
};

struct Mixed {
    int a;
    long b;
    float c;
    double d;
};

/* Test 1: SROA on 2D Point */
int test_sroa_point(void) {
    struct Point p;
    p.x = 15;
    p.y = 25;
    return p.x + p.y;
}

/* Test 2: SROA on multi-typed struct */
double test_sroa_mixed(void) {
    struct Mixed m;
    m.a = 10;
    m.b = 100L;
    m.c = 2.5f;
    m.d = 7.5;
    return m.a + m.b + m.c + m.d;
}

/* Test 3: SROA with compound assignment */
int test_sroa_compound(void) {
    struct Point p;
    p.x = 10;
    p.y = 20;
    p.x += 5;
    p.y *= 3;
    p.x -= 2;
    p.y /= 2;
    return p.x + p.y; /* (10 + 5 - 2) + (20 * 3 / 2) = 13 + 30 = 43 */
}

/* Test 4: SROA with increment/decrement */
int test_sroa_inc_dec(void) {
    struct Point p;
    p.x = 10;
    p.y = 20;
    int old_x = p.x++;
    int new_y = ++p.y;
    int post_dec = p.y--;
    int pre_dec = --p.x;
    return old_x + new_y + post_dec + pre_dec + p.x + p.y;
    /* old_x = 10 (p.x now 11)
     * new_y = 21 (p.y now 21)
     * post_dec = 21 (p.y now 20)
     * pre_dec = 10 (p.x now 10)
     * p.x = 10, p.y = 20
     * sum = 10 + 21 + 21 + 10 + 10 + 20 = 92
     */
}

/* Test 5: SROA loop accumulator */
int test_sroa_loop(void) {
    struct Point p;
    p.x = 0;
    p.y = 1;
    for (int i = 0; i < 10; i++) {
        p.x += i;
        p.y += 2;
    }
    return p.x + p.y;
}

/* Test 6: DSE overwritten scalar store */
int test_dse_overwritten(void) {
    int x;
    x = 100; /* Dead store: overwritten by 200 */
    x = 200;
    return x;
}

/* Test 7: DSE overwritten struct member store */
int test_dse_struct_overwritten(void) {
    struct Point p;
    p.x = 111; /* Dead store */
    p.y = 222; /* Dead store */
    p.x = 333;
    p.y = 444;
    return p.x + p.y;
}

/* Test 8: DSE store before exit */
int test_dse_before_exit(int a) {
    int unused = a * 2 + 1; /* Dead store */
    (void)unused;
    return 42;
}

/* Test 9: DSE across branch */
int test_dse_branch(int cond) {
    int x = 1; /* Dead store: both branches overwrite before read */
    if (cond) {
        x = 10;
    } else {
        x = 20;
    }
    return x;
}

/* Test 10: Soundness check: live store correctly read */
int test_soundness(int a, int b) {
    int x = a + 1;
    int y = b + 2;
    int sum1 = x + y;
    x = sum1 * 2; /* overwrites x */
    int sum2 = x + y;
    return sum1 + sum2;
}

int main(void) {
    printf("Testing Sprint 231: SROA & DSE...\n");

    /* 1. SROA Point */
    int r1 = test_sroa_point();
    printf("1: %d\n", r1);
    assert(r1 == 40);

    /* 2. SROA Mixed */
    double r2 = test_sroa_mixed();
    printf("2: %f\n", r2);
    assert(r2 == 120.0);

    /* 3. SROA Compound */
    int r3 = test_sroa_compound();
    printf("3: %d\n", r3);
    assert(r3 == 43);

    /* 4. SROA Inc/Dec */
    int r4 = test_sroa_inc_dec();
    printf("4: %d\n", r4);
    assert(r4 == 92);

    /* 5. SROA Loop */
    int r5 = test_sroa_loop();
    printf("5: %d\n", r5);
    /* sum(0..9) = 45; 1 + 2*10 = 21; 45 + 21 = 66 */
    assert(r5 == 66);

    /* 6. DSE Overwritten */
    int r6 = test_dse_overwritten();
    printf("6: %d\n", r6);
    assert(r6 == 200);

    /* 7. DSE Struct Overwritten */
    int r7 = test_dse_struct_overwritten();
    printf("7: %d\n", r7);
    assert(r7 == 777);

    /* 8. DSE Before Exit */
    int r8 = test_dse_before_exit(5);
    printf("8: %d\n", r8);
    assert(r8 == 42);

    /* 9. DSE Branch */
    printf("9a: %d, 9b: %d\n", test_dse_branch(1), test_dse_branch(0));
    assert(test_dse_branch(1) == 10);
    assert(test_dse_branch(0) == 20);

    /* 10. Soundness */
    printf("10: %d\n", test_soundness(3, 4));
    assert(test_soundness(3, 4) == 36);

    printf("All SROA & DSE tests passed successfully!\n");
    return 0;
}
