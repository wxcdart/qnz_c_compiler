// Benchmark: Memory-intensive code
// Tests memory access patterns

int main() {
    int arr[1000];
    int sum = 0;
    
    // Initialize array
    for (int i = 0; i < 1000; i++) {
        arr[i] = i * 2;
    }
    
    // Sum with pointer arithmetic
    for (int i = 0; i < 1000; i++) {
        sum += arr[i];
    }
    
    // 2D array access
    int matrix[100][100];
    for (int i = 0; i < 100; i++) {
        for (int j = 0; j < 100; j++) {
            matrix[i][j] = i * 100 + j;
        }
    }
    
    for (int i = 0; i < 100; i++) {
        for (int j = 0; j < 100; j++) {
            sum += matrix[i][j];
        }
    }
    
    return sum;
}
