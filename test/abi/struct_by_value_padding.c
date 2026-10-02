#include <stdio.h>

typedef struct { char c; int i; } Padded; // char + 3 bytes padding + int (total 8)

Padded make_padded(void) {
    Padded p = { 'A', 0x12345678 };
    return p;
}

int sum_padded(Padded p) {
    return p.c + p.i;
}

int main(void) {
    Padded p = make_padded();
    if (sum_padded(p) != ('A' + 0x12345678)) {
        fprintf(stderr, "FAIL padded %d\n", sum_padded(p));
        return 1;
    }
    printf("PASS\n");
    return 0;
}
