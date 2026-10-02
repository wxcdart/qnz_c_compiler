#include <stdio.h>

typedef struct { int a; int b; } Pair;

Pair make_pair(int x, int y) {
    Pair p = { x, y };
    return p;
}

int sum_pair(Pair p) {
    return p.a + p.b;
}

typedef struct { int arr[6]; } Big;

Big make_big(void) {
    Big b = { { 1, 2, 3, 4, 5, 6 } };
    return b;
}

int sum_big(Big b) {
    int s = 0;
    for (int i = 0; i < 6; i++) {
        s += b.arr[i];
    }
    return s;
}

int main(void) {
    Pair p = make_pair(3, 4);
    if (sum_pair(p) != 7) {
        fprintf(stderr, "FAIL pair %d\n", sum_pair(p));
        return 1;
    }
    Big b = make_big();
    if (sum_big(b) != 21) {
        fprintf(stderr, "FAIL big %d\n", sum_big(b));
        return 1;
    }
    printf("PASS\n");
    return 0;
}
