#include <stdio.h>
int main(void) {
    int x = (int){42};
    int *p = (int[]){1,2,3};
    if (x != 42) return 1;
    if (*p != 1) return 2;
    printf("Compound literal test passed\n");
    return 0;
}
