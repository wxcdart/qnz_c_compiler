#include <stdio.h>

int g_overflow_arr[4] = {1, 2, 3, 4};

int main(void) {
    // Volatile index to avoid constant folding / optimization
    volatile int idx = 4;
    printf("Accessing g_overflow_arr[%d]...\n", idx);
    int val = g_overflow_arr[idx];
    printf("Val: %d\n", val);
    return 0;
}
