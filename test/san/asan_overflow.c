#include <stdio.h>

int main(void) {
    volatile int arr[10];
    for (int i = 0; i < 10; i++) {
        arr[i] = i;
    }
    /* Stack buffer overflow into right redzone */
    arr[10] = 999;
    return 0;
}
