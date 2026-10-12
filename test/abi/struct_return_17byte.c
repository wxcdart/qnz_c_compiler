/* System V AMD64 ABI: >16 byte struct returns via hidden pointer (sret) */
#include <stdio.h>
#include <stdlib.h>
#include "zero_cc.h"

typedef struct { 
    char a;
    int b, c, d, e;  /* 1 + 16 = 17 bytes (with padding) */
} Struct17;

Struct17 make_17(char a, int b, int c, int d, int e) {
    Struct17 s = {a, b, c, d, e};
    return s;
}

int main(void) {
    Struct17 result = make_17('X', 100, 200, 300, 400);
    
    if (result.a != 'X') return 1;
    if (result.b != 100) return 2;
    if (result.c != 200) return 3;
    if (result.d != 300) return 4;
    if (result.e != 400) return 5;
    
    printf("PASS: 17-byte struct return\n");
    return 0;
}
