#include <stdio.h>
#include <assert.h>

/* Tail-recursive GCD */
int gcd(int a, int b) {
    if (b == 0) return a;
    return gcd(b, a % b);
}

/* Tail-recursive factorial with accumulator */
long long fact_tail(int n, long long acc) {
    if (n <= 1) return acc;
    return fact_tail(n - 1, n * acc);
}

/* Tail-recursive Fibonacci with accumulator */
int fib_tail(int n, int a, int b) {
    if (n == 0) return a;
    if (n == 1) return b;
    return fib_tail(n - 1, b, a + b);
}

/* Tail-recursive counter - fits stack at -O0, runs as a loop at -O1+ */
int count_tail(int n, int acc) {
    if (n <= 0) return acc;
    return count_tail(n - 1, acc + 1);
}

/* Float tail recursion */
double sum_float(int n, double acc) {
    if (n <= 0) return acc;
    return sum_float(n - 1, acc + 0.5);
}

/* Void tail recursion */
void void_countdown(int n, int *res) {
    if (n <= 0) return;
    (*res)++;
    void_countdown(n - 1, res);
}

int main(void) {
    assert(gcd(48, 18) == 6);
    assert(gcd(101, 103) == 1);
    assert(gcd(270, 192) == 6);

    assert(fact_tail(5, 1) == 120);
    assert(fact_tail(10, 1) == 3628800);

    assert(fib_tail(0, 0, 1) == 0);
    assert(fib_tail(1, 0, 1) == 1);
    assert(fib_tail(10, 0, 1) == 55);

    /* Counter: runs without overflow at -O0 and as O(1) stack loop at -O1+ */
    assert(count_tail(1000, 0) == 1000);

    /* Float accumulator */
    double f = sum_float(1000, 0.0);
    assert(f >= 499.9 && f <= 500.1);

    /* Void tail recursion */
    int vres = 0;
    void_countdown(1000, &vres);
    assert(vres == 1000);

    printf("All tail-call tests PASSED\n");
    return 0;
}
