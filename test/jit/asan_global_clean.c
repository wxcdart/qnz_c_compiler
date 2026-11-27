#include <stdio.h>

int g_data_scalar = 42;
int g_data_array[8] = {10, 20, 30, 40, 50, 60, 70, 80};
int g_bss_scalar;
int g_bss_array[8];

int main(void) {
    g_bss_scalar = 100;
    for (int i = 0; i < 8; i++) {
        g_bss_array[i] = i * 5;
    }

    int sum = g_data_scalar + g_bss_scalar;
    for (int i = 0; i < 8; i++) {
        sum += g_data_array[i] + g_bss_array[i];
    }

    if (sum != (42 + 100 + 360 + 140)) {
        return 1;
    }

    printf("asan_test passed\n");
    return 0;
}
