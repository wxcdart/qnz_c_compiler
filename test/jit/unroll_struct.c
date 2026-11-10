// expect-unroll: yes
/* Loop unrolling (Sprint 150): struct bodies (aggregate locals stay in
 * stack slots; per-iteration field updates must land in the right
 * iteration's copy). */
#include <stdio.h>

typedef struct { int x, y; } P;

int st1(void) {
    P pts[4] = {{1, 2}, {3, 4}, {5, 6}, {7, 8}};
    int s = 0;
    for (int i = 0; i < 4; i++) s += pts[i].x + pts[i].y;
    return s;
}

int st2(void) {
    P acc = {0, 0};
    for (int i = 0; i < 5; i++) {
        acc.x += i;
        acc.y += i * 10;
    }
    return acc.x * 100 + acc.y;
}

int main(void) {
    printf("%d %d\n", st1(), st2());
    return 0;
}
