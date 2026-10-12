/* System V AMD64 ABI: >16 byte struct returns via hidden pointer (sret) */
#include <stdio.h>
#include <stdlib.h>
#include "zero_cc.h"

typedef struct { 
    int a, b, c, d, e, f;  /* 6 * 4 = 24 bytes */
} Struct24;

Struct24 make_24(int a, int b, int c, int d, int e, int f) {
    Struct24 s = {a, b, c, d, e, f};
    return s;
}

int main(void) {
    Struct24 result = make_24(10, 20, 30, 40, 50, 60);
    
    if (result.a != 10) return 1;
    if (result.b != 20) return 2;
    if (result.c != 30) return 3;
    if (result.d != 40) return 4;
    if (result.e != 50) return 5;
    if (result.f != 60) return 6;
    
    printf("PASS: 24-byte struct return\n");
    return 0;
}
