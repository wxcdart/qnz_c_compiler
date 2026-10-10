/* Stack Arguments Test: >6 integer args (x86-64 SysV) */
#include <assert.h>

int sum7(int a, int b, int c, int d, int e, int f, int g) {
    return a + b + c + d + e + f + g;
}

int sum8(int a, int b, int c, int d, int e, int f, int g, int h) {
    return a + b + c + d + e + f + g + h;
}

int sum10(int a, int b, int c, int d, int e, int f, int g, int h, int i, int j) {
    return a + b + c + d + e + f + g + h + i + j;
}

int main(void) {
    /* 7 args: g on stack */
    assert(sum7(1,2,3,4,5,6,7) == 28);
    
    /* 8 args: g,h on stack */
    assert(sum8(1,2,3,4,5,6,7,8) == 36);
    
    /* 10 args: g,h,i,j on stack */
    assert(sum10(1,2,3,4,5,6,7,8,9,10) == 55);
    
    return 0;
}