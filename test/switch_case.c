#include <stdio.h>
#include <stdlib.h>
#include "zero_cc.h"

int switch_test(void) {
    int x = 2;
    switch (x) {
        case 1: return 10;
        case 2: return 20;
        case 3: return 30;
        default: return -1;
    }
    return -1; // unreachable
}

int switch_default(void) {
    int x = 5;
    switch (x) {
        case 1: return 10;
        case 2: return 20;
        case 3: return 30;
        default: return -1;
    }
    return -1; // unreachable
}

int main(void) {
    int r1 = switch_test();
    int r2 = switch_default();
    if (r1 == 20 && r2 == -1) {
        printf("PASS\n");
        return 0;
    } else {
        fprintf(stderr, "FAIL: got %d and %d\n", r1, r2);
        return 1;
    }
}
