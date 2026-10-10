/* Arithmetic-heavy benchmark for optimization profiling */

/* Heavy arithmetic in loops */
int arithmetic_loop(int n) {
    int result = 0;
    int a = 10, b = 20, c = 30;  /* Invariants */
    
    for (int i = 0; i < n; i++) {
        result += a + b + c;
        result *= 2;
        result -= a;
        result += b * c;
    }
    return result;
}

/* Multiple arithmetic operations */
int complex_arith(int x, int y, int n) {
    int total = 0;
    int scale = x * y;           /* Invariant */
    int offset = x + y;          /* Invariant */
    int diff = x - y;            /* Invariant */
    int prod = x * y * 2;        /* Invariant */
    
    for (int i = 0; i < n; i++) {
        total += scale + offset;
        total -= diff;
        total *= prod;
        total += i * 2;
    }
    return total;
}

/* Bitwise operations */
int bitwise_loop(int n) {
    int result = 0;
    int mask = 0xFF;             /* Invariant */
    int shift = 4;               /* Invariant */
    int value = 0x1234;          /* Invariant */
    
    for (int i = 0; i < n; i++) {
        result |= (value >> shift) & mask;
        result ^= mask;
        result <<= shift;
        result &= ~mask;
    }
    return result;
}

/* Division and modulo */
int div_mod_loop(int n) {
    int total = 0;
    int divisor = 7;             /* Invariant */
    int modulo = 5;              /* Invariant */
    
    for (int i = 0; i < n; i++) {
        total += i / divisor;
        total += i % modulo;
        total *= divisor;
    }
    return total;
}

int main(void) {
    int r1 = arithmetic_loop(100);
    int r2 = complex_arith(10, 20, 100);
    int r3 = bitwise_loop(100);
    int r4 = div_mod_loop(100);
    
    return r1 + r2 + r3 + r4;
}
