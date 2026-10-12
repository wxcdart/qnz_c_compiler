/* System V AMD64 ABI: >16 byte struct returns via hidden pointer (sret) */
#include <stdio.h>
#include <stdlib.h>
#include "zero_cc.h"

typedef struct { 
    int a, b, c, d, e, f, g, h;  /* 8 * 4 = 32 bytes */
} Struct32;

Struct32 make_32(int a, int b, int c, int d, int e, int f, int g, int h) {
    Struct32 s = {a, b, c, d, e, f, g, h};
    return s;
}

int main(void) {
    Struct32 result = make_32(1, 2, 3, 4, 5, 6, 7, 8);
    
    if (result.a != 1) return 1;
    if (result.b != 2) return 2;
    if (result.c != 3) return 3;
    if (result.d != 4) return 4;
    if (result.e != 5) return 5;
    if (result.f != 6) return 6;
    if (result.g != 7) return 7;
    if (result.h != 8) return 8;
    
    printf("PASS: 32-byte struct return\n");
    return 0;
}
