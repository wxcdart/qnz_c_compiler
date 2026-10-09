#include <stdio.h>

/* Demo function with common subexpressions */
int common_expr_demo(int a, int b, int c) {
    int x = a + b;
    int y = a + b;  /* Common subexpression */
    int z = x + y;
    return z + c;
}

/* Demo function with loop */
int loop_demo(int n) {
    int sum = 0;
    int factor = 10;  /* Loop-invariant */
    for (int i = 0; i < n; i++) {
        sum += i * factor;  /* factor is invariant */
    }
    return sum;
}

/* Demo function with nested loops */
int nested_loop_demo(int n, int m) {
    int sum = 0;
    int outer_factor = 5;  /* Outer loop invariant */
    for (int i = 0; i < n; i++) {
        int inner_factor = outer_factor * 2;  /* Inner loop invariant */
        for (int j = 0; j < m; j++) {
            sum += i + j + inner_factor;
        }
    }
    return sum;
}

int main(void) {
    printf("Common expr: %d\n", common_expr_demo(1, 2, 3));
    printf("Loop demo: %d\n", loop_demo(10));
    printf("Nested loop: %d\n", nested_loop_demo(5, 5));
    return 0;
}
