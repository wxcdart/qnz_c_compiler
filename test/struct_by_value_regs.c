#include <stdio.h>

typedef struct { char a; char b; } Small2; // 2 bytes, fits in a register

typedef struct { int a; int b; int c; } Mid12; // 12 bytes, passed in two registers or memory

// Function takes both structs and returns their sum as int
int sum_structs(Small2 s, Mid12 m) {
    return s.a + s.b + m.a + m.b + m.c;
}

// Returns Small2 by value
Small2 make_small2(void) {
    Small2 s = { 3, 4 };
    return s;
}

// Returns Mid12 by value
Mid12 make_mid12(void) {
    Mid12 m = { 1, 2, 3 };
    return m;
}

int main(void) {
    Small2 s = make_small2();
    Mid12 m = make_mid12();
    int result = sum_structs(s, m);
    if (result != 13) { // 3+4+1+2+3 = 13
        fprintf(stderr, "FAIL regs %d\n", result);
        return 1;
    }
    printf("PASS\n");
    return 0;
}
