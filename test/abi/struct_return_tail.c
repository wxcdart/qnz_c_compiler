#include <stdio.h>
#include <stdlib.h>

typedef struct {
    int a, b, c, d, e;
} Big20;

Big20 make_inner(int a, int b, int c, int d, int e) {
    Big20 s = {a, b, c, d, e};
    return s;
}

Big20 make_tail(int a, int b, int c, int d, int e) {
    return make_inner(a, b, c, d, e);
}

int main(void) {
    Big20 res = make_tail(100, 200, 300, 400, 500);
    if (res.a != 100 || res.b != 200 || res.c != 300 || res.d != 400 || res.e != 500) {
        printf("FAIL: tail return: %d %d %d %d %d\n", res.a, res.b, res.c, res.d, res.e);
        return 1;
    }
    printf("PASS: struct return tail call\n");
    return 0;
}
