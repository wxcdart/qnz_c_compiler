/* System V AMD64 ABI: >16 byte struct returns via hidden pointer (sret) */
#include <stdio.h>
#include <stdlib.h>
#include "zero_cc.h"

typedef struct { 
    int a, b, c, d, e;  /* 5 * 4 = 20 bytes */
} Struct20;

Struct20 make_20(int a, int b, int c, int d, int e) {
    Struct20 s = {a, b, c, d, e};
    return s;
}

int main(void) {
    Struct20 result = make_20(1, 2, 3, 4, 5);
    
    if (result.a != 1) { printf("FAIL: a=%d\n", result.a); return 1; }
    if (result.b != 2) { printf("FAIL: b=%d\n", result.b); return 2; }
    if (result.c != 3) { printf("FAIL: c=%d\n", result.c); return 3; }
    if (result.d != 4) { printf("FAIL: d=%d\n", result.d); return 4; }
    if (result.e != 5) { printf("FAIL: e=%d\n", result.e); return 5; }
    
    printf("PASS: 20-byte struct return\n");
    return 0;
}
