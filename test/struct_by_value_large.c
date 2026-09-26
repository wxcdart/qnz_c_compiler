#include <stdio.h>

typedef struct { int a; int b; int c; int d; int e; } LargeReturn; // 20 bytes (>16)

LargeReturn make_large_return(void) {
    LargeReturn lr = { 1, 2, 3, 4, 5 };
    return lr;
}

int sum_large_return(LargeReturn lr) {
    return lr.a + lr.b + lr.c + lr.d + lr.e;
}

int main(void) {
    LargeReturn lr = make_large_return();
    if (sum_large_return(lr) != 15) {
        fprintf(stderr, "FAIL large_return %d\n", sum_large_return(lr));
        return 1;
    }
    printf("PASS\n");
    return 0;
}
