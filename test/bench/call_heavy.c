// Benchmark: Function call intensive code
// Tests call overhead and inlining

static inline int square(int x) {
    return x * x;
}

static inline int cube(int x) {
    return x * x * x;
}

int main() {
    int sum = 0;
    
    // Many function calls
    for (int i = 0; i < 100; i++) {
        sum += square(i);
        sum += cube(i % 10);
    }
    
    // Recursive-like pattern
    for (int i = 0; i < 50; i++) {
        int a = square(i);
        int b = square(a);
        sum += b;
    }
    
    return sum;
}
