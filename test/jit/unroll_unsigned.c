// expect-unroll: yes
/* Loop unrolling (Sprint 150): unsigned counters (CC_FLT_* CF/ZF
 * compare variants) and narrow unsigned types. */
#include <stdio.h>

int u1(void) {
    unsigned s = 0;
    for (unsigned i = 0; i < 6; i++) s += i * 4 + 1;
    return (int)s;
}

int u2(void) {
    unsigned char s = 0;
    for (unsigned char i = 0; i < 5; i++) s += i + 10;
    return (int)s;
}

int u3(void) {
    int s = 0;
    for (int i = 0; i <= 4u; i++) s += i;  /* unsigned comparison */
    return s;
}

int main(void) {
    printf("%d %d %d\n", u1(), u2(), u3());
    return 0;
}
