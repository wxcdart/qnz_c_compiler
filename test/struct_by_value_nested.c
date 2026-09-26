#include <stdio.h>

typedef struct { int a; int b; int c; } Triple; // 12 bytes

Triple make_triple(int a, int b, int c) {
    Triple t = { a, b, c };
    return t;
}

int sum_triple(Triple t) {
    return t.a + t.b + t.c;
}

typedef struct { Triple inner; int d; } Combined; // likely 16 bytes

Combined make_combined(void) {
    Combined c = { {1, 2, 3}, 4 };
    return c;
}

int sum_combined(Combined c) {
    return c.inner.a + c.inner.b + c.inner.c + c.d;
}

int main(void) {
    Triple t = make_triple(5, 6, 7);
    if (sum_triple(t) != 18) {
        fprintf(stderr, "FAIL triple %d\n", sum_triple(t));
        return 1;
    }
    Combined c = make_combined();
    if (sum_combined(c) != 10) {
        fprintf(stderr, "FAIL combined %d\n", sum_combined(c));
        return 1;
    }
    printf("PASS\n");
    return 0;
}
