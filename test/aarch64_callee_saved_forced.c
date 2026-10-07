/* Force callee-saved register usage by using many local variables */
/* This should trigger the register allocator to use x19-x28 */

int test_func(int a, int b, int c, int d, int e, int f, int g, int h) {
    /* Force many variables to be allocated */
    volatile int v1 = a;
    volatile int v2 = b;
    volatile int v3 = c;
    volatile int v4 = d;
    volatile int v5 = e;
    volatile int v6 = f;
    volatile int v7 = g;
    volatile int v8 = h;
    volatile int v9 = v1 + v2;
    volatile int v10 = v3 + v4;
    volatile int v11 = v5 + v6;
    volatile int v12 = v7 + v8;
    volatile int v13 = v9 + v10;
    volatile int v14 = v11 + v12;
    volatile int v15 = v13 + v14;
    
    return v15;
}

int main(void) {
    int result = test_func(1, 2, 3, 4, 5, 6, 7, 8);
    /* Expected: (1+2) + (3+4) + (5+6) + (7+8) = 3 + 7 + 11 + 15 = 36 */
    return (result == 36) ? 0 : 1;
}
