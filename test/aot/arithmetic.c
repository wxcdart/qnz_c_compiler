// AOT Test: Basic Arithmetic
// Tests: Integer operations, return values

int add(int a, int b) {
    return a + b;
}

int subtract(int a, int b) {
    return a - b;
}

int multiply(int a, int b) {
    return a * b;
}

int main() {
    int result = 0;
    result = add(10, 5);
    if (result != 15) return 1;
    
    result = subtract(10, 5);
    if (result != 5) return 2;
    
    result = multiply(10, 5);
    if (result != 50) return 3;
    
    return 0;
}
