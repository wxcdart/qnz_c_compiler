#include <stdio.h>

typedef struct {
    int a, b, c, d, e;
} Big20;

typedef Big20 (*big_fn_t)(int, int, int, int, int);

Big20 make_big(int a, int b, int c, int d, int e) {
    Big20 s = {a, b, c, d, e};
    return s;
}

int test_inline_fp(void) {
    Big20 (*fn)(int, int, int, int, int) = make_big;
    Big20 r = fn(1, 2, 3, 4, 5);
    if (r.a != 1 || r.b != 2 || r.c != 3 || r.d != 4 || r.e != 5) {
        printf("FAIL: inline funcptr sret: %d %d %d %d %d\n", r.a, r.b, r.c, r.d, r.e);
        return 1;
    }
    return 0;
}

int test_typedef_fp(void) {
    big_fn_t fn2 = make_big;
    Big20 r2 = fn2(10, 20, 30, 40, 50);
    if (r2.a != 10 || r2.b != 20 || r2.c != 30 || r2.d != 40 || r2.e != 50) {
        printf("FAIL: typedef funcptr sret: %d %d %d %d %d\n", r2.a, r2.b, r2.c, r2.d, r2.e);
        return 1;
    }
    return 0;
}

int test_reassign_and_member_fp(void) {
    big_fn_t fn = make_big;
    Big20 r = {0, 0, 0, 0, 0};
    r = fn(100, 200, 300, 400, 500);
    if (r.a != 100 || r.e != 500) {
        printf("FAIL: reassigned funcptr sret: %d %d\n", r.a, r.e);
        return 1;
    }
    if (fn(7, 8, 9, 10, 11).c != 9) {
        printf("FAIL: member access on funcptr sret\n");
        return 1;
    }
    return 0;
}

int main(void) {
    if (test_inline_fp() != 0) return 1;
    if (test_typedef_fp() != 0) return 1;
    if (test_reassign_and_member_fp() != 0) return 1;
    printf("PASS: function pointer sret call\n");
    return 0;
}
