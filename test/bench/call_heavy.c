/* Call-heavy benchmark for optimization profiling */

/* Simple functions for call overhead testing */
int add(int a, int b) { return a + b; }
int sub(int a, int b) { return a - b; }
int mul(int a, int b) { return a * b; }

/* Heavy call loop */
int heavy_calls(int n) {
    int total = 0;
    int a = 10;
    int b = 20;
    int i = 0;
    
    while (i < n) {
        total = add(total, add(a, b));
        total = sub(total, add(a, b));
        total = mul(total, 1);
        i = i + 1;
    }
    return total;
}

/* Nested function calls */
int nested_calls(int n) {
    int total = 0;
    int scale = 2;
    int i = 0;
    
    while (i < n) {
        total = add(total, mul(i, scale));
        total = add(total, sub(i, scale));
        i = i + 1;
    }
    return total;
}

/* Function call with loop invariants */
int get_base(void) { return 100; }
int get_multiplier(void) { return 5; }

int call_with_invariants(int n) {
    int total = 0;
    int base = get_base();
    int mult = get_multiplier();
    int i = 0;
    
    while (i < n) {
        total = add(total, mul(i, mult));
        total = add(total, base);
        i = i + 1;
    }
    return total;
}

/* Main function */
int main(void) {
    int r1 = heavy_calls(100);
    int r2 = nested_calls(100);
    int r3 = call_with_invariants(100);
    
    return r1 + r2 + r3;
}
