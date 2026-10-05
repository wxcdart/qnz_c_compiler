// Benchmark: Loop-heavy code
// Tests loop-invariant code motion and strength reduction

int main() {
    int sum = 0;
    int arr[100];
    int multiplier = 4;
    
    // Loop with invariant computation
    for (int i = 0; i < 100; i++) {
        int invariant = 10 + 20;  // Should be hoisted
        arr[i] = i * multiplier;  // Strength reduction candidate
        sum += arr[i];
    }
    
    // Nested loops
    for (int i = 0; i < 50; i++) {
        for (int j = 0; j < 50; j++) {
            sum += i * j;
        }
    }
    
    return sum;
}
