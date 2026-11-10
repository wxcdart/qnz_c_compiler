// expect-unroll: yes
/* Loop unrolling (Sprint 150): basic for-loop shapes.
 * Constant trip counts in the unroll range (<= 8): < and <= forms,
 * non-zero init, trip 1, several loops per function. */
#include <stdio.h>

int sum8(void) {
    int s = 0;
    for (int i = 0; i < 8; i++) s += i * 3 + 1;
    return s;  /* 92 */
}

int sum_le(void) {
    int s = 0;
    for (int i = 0; i <= 3; i++) s += i + 10;
    return s;  /* 46 */
}

int sum_off(void) {
    int s = 0;
    for (int i = 2; i < 7; i++) s += i;
    return s;  /* 20 */
}

int sum1(void) {
    int s = 7;
    for (int i = 0; i < 1; i++) s += i * 100 + 4;
    return s;  /* 11 */
}

int main(void) {
    printf("%d %d %d %d\n", sum8(), sum_le(), sum_off(), sum1());
    return 0;
}
