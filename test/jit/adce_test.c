#include <stdio.h>
#include <assert.h>

/* Dead loop: accumulator and induction variable are never used */
int test_dead_loop(int n) {
    int s = 0;
    for (int i = 0; i < n; i++) {
        s += i * i + 1;
    }
    return 42;
}

/* Nested dead loops: both loops compute unused values */
int test_nested_dead_loops(int n) {
    int acc = 0;
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
            acc += (i + 1) * (j + 2);
        }
    }
    return 100;
}

/* Dead conditional branch: neither arm produces an observable side effect or used value */
int test_dead_branch(int x) {
    int a = 0;
    if (x > 10) {
        a = x * 2 + 5;
    } else {
        a = x * 3 + 7;
    }
    return 55;
}

/* Live loop: writes to memory array (must NOT be eliminated) */
int test_live_store_loop(int *buf, int n) {
    for (int i = 0; i < n; i++) {
        buf[i] = i * 2;
    }
    return buf[n - 1];
}

/* Live loop: computes returned sum (must NOT be eliminated) */
int test_live_calc_loop(int n) {
    int sum = 0;
    while (n > 0) {
        sum += n;
        n--;
    }
    return sum;
}

int main(void) {
    int buf[10];
    int r1 = test_dead_loop(50);
    assert(r1 == 42);

    int r2 = test_dead_loop(1000);
    assert(r2 == 42);

    int r3 = test_nested_dead_loops(20);
    assert(r3 == 100);
    assert(test_nested_dead_loops(50) == 100);

    int r4 = test_dead_branch(5);
    printf("test_dead_branch(5) = %d\n", r4);
    assert(r4 == 55);
    int r5 = test_dead_branch(15);
    printf("test_dead_branch(15) = %d\n", r5);
    assert(r5 == 55);

    int r6 = test_live_store_loop(buf, 10);
    printf("test_live_store_loop = %d\n", r6);
    assert(r6 == 18);
    printf("buf[0]=%d, buf[4]=%d, buf[9]=%d\n", buf[0], buf[4], buf[9]);
    assert(buf[0] == 0 && buf[4] == 8 && buf[9] == 18);

    assert(test_live_calc_loop(10) == 55);
    assert(test_live_calc_loop(100) == 5050);

    printf("All ADCE tests PASSED\n");
    return 0;
}
