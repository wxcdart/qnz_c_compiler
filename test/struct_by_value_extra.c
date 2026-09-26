#include <stdio.h>

typedef struct { int a; int b; int c; int d; } Quad;

Quad make_quad(int a, int b, int c, int d) {
    Quad q = { a, b, c, d };
    return q;
}

int sum_quad(Quad q) {
    return q.a + q.b + q.c + q.d;
}

typedef struct { int arr[10]; } Large;

Large make_large(void) {
    Large l = { { 1,2,3,4,5,6,7,8,9,10 } };
    return l;
}

int sum_large(Large l) {
    int s = 0;
    for (int i = 0; i < 10; i++) {
        s += l.arr[i];
    }
    return s;
}

int main(void) {
    Quad q = make_quad(1,2,3,4);
    if (sum_quad(q) != 10) {
        fprintf(stderr, "FAIL quad %d\n", sum_quad(q));
        return 1;
    }
    Large l = make_large();
    if (sum_large(l) != 55) {
        fprintf(stderr, "FAIL large %d\n", sum_large(l));
        return 1;
    }
    printf("PASS\n");
    return 0;
}
