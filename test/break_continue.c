#include <stdio.h>
#include <stdlib.h>
#include "zero_cc.h"

int loop_sum(void) {
    int s = 0;
    // for loop with break and continue
    for (int i = 0; i < 10; i++) {
        if (i == 5) break;      // exit early at 5
        if (i == 2) continue;   // skip 2
        s = s + i;
    }
    int t = 0;
    int j = 0;
    // while loop with break and continue
    while (j < 5) {
        if (j == 3) break;      // exit at 3
        if (j == 1) { j++; continue; } // skip adding when j==1
        t = t + j;
        j++;
    }
    return s + t; // expected: (0+1+3+4) = 8 + (0+2+4) = 6 => 14
}

int main(void) {
    int res = loop_sum();
    if (res != 10) {
        fprintf(stderr, "FAIL: expected 14 got %d\n", res);
        return 1;
    }
    printf("PASS\n");
    return 0;
}
