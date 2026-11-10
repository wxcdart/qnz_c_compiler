// expect-unroll: yes
/* Loop unrolling (Sprint 150): interaction with surrounding structure.
 * Inner constant-trip loop inside a variable-bound outer loop (the inner
 * must unroll, the outer must not); loop nested in an if; two
 * unrollable loops in one function. */
#include <stdio.h>

int m1(int m) {
    int s = 0;
    for (int i = 0; i < m; i++)
        for (int j = 0; j < 4; j++)
            s += i * 10 + j;
    return s;
}

int m2(int flag) {
    int s = 1;
    if (flag) {
        for (int i = 0; i < 3; i++) s += i * 2 + 1;
    }
    return s;
}

int m3(void) {
    int a = 0, b = 0;
    for (int i = 0; i < 3; i++) a += i + 1;
    for (int j = 0; j < 5; j++) b += j * j;
    return a * 100 + b;
}

int main(void) {
    printf("%d %d %d %d\n", m1(3), m2(1), m3(), m2(0));
    return 0;
}
