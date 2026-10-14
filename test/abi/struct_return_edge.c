#include <stdio.h>
#include <stdlib.h>

typedef struct {
    int a, b, c, d, e; /* 20 bytes (> 16 bytes, uses sret) */
} Big20;

Big20 make_big(int a, int b, int c, int d, int e) {
    Big20 s = {a, b, c, d, e};
    return s;
}

int main(void) {
    /* 4.1: Direct member access on call return: make_big(...).c */
    int val_c = make_big(10, 20, 30, 40, 50).c;
    if (val_c != 30) {
        printf("FAIL 4.1: direct member access .c = %d (expected 30)\n", val_c);
        return 1;
    }

    /* 4.2: Direct assignment into existing local: r = make_big(...) */
    Big20 r;
    r = make_big(11, 22, 33, 44, 55);
    if (r.a != 11 || r.b != 22 || r.c != 33 || r.d != 44 || r.e != 55) {
        printf("FAIL 4.2: assignment r = make_big(...): %d %d %d %d %d\n", r.a, r.b, r.c, r.d, r.e);
        return 2;
    }

    printf("PASS: struct return edge cases\n");
    return 0;
}
