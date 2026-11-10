// expect-unroll: yes
/* Loop unrolling (Sprint 150): bound forms (#define, enum, sizeof) and
 * float/double bodies. */
#include <stdio.h>

#define K 5
enum { E = 4 };

int bf1(void) {
    int s = 0;
    for (int i = 0; i < K; i++) s += i + 1;
    return s;
}

int bf2(void) {
    int s = 0;
    for (int i = 0; i < E; i++) s += i * 2;
    return s;
}

int bf3(void) {
    int arr[6] = {1, 2, 3, 4, 5, 6};
    int s = 0;
    for (int i = 0; i < (int)(sizeof(arr) / sizeof(arr[0])); i++) s += arr[i];
    return s;
}

int bf4(void) {
    float s = 0.0f;
    for (int i = 0; i < 4; i++) s += (float)(i + 1) * 1.5f;
    return (int)(s + 0.5f);
}

int bf5(void) {
    double s = 0.0;
    for (int i = 1; i < 5; i++) s += (double)i / 4.0;
    return (int)(s * 100.0 + 0.5);
}

int main(void) {
    printf("%d %d %d %d %d\n", bf1(), bf2(), bf3(), bf4(), bf5());
    return 0;
}
