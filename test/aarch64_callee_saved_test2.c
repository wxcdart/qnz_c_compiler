/* Test that forces callee-saved register usage */
/* This function has many variables that will spill to callee-saved regs */

int complex_function(int a, int b, int c, int d, int e) {
    int x1 = a * b;
    int x2 = x1 + c;
    int x3 = x2 * d;
    int x4 = x3 + e;
    int x5 = x4 * a;
    int x6 = x5 + b;
    int x7 = x6 * c;
    int x8 = x7 + d;
    int x9 = x8 * e;
    int x10 = x9 + a;
    int x11 = x10 * b;
    int x12 = x11 + c;
    int x13 = x12 * d;
    int x14 = x13 + e;
    int x15 = x14 * a;
    return x15;
}

int main() {
    int result = complex_function(1, 2, 3, 4, 5);
    return result;  // Will return 1*2*3*4*5 + ... (some large number)
}
