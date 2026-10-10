/* LICM Invariant Test Cases
 * 
 * These test cases are designed to verify LICM trampoline implementation:
 * 1. Simple invariant: constant expression inside loop
 * 2. Load invariant: value loaded once, used many times
 * 3. Complex invariant: multi-step computation that's loop-invariant
 */

/* Test 1: Simple constant invariant */
int test_simple_invariant(int *arr, int n) {
    int result = 0;
    int factor = 5;  // Invariant: computed once outside loop
    for (int i = 0; i < n; i++) {
        result += arr[i] * factor;  // factor is invariant
    }
    return result;
}

/* Test 2: Load invariant */
int test_load_invariant(int *arr, int *multiplier, int n) {
    int result = 0;
    int m = *multiplier;  // Invariant: load once outside loop
    for (int i = 0; i < n; i++) {
        result += arr[i] * m;  // m is invariant
    }
    return result;
}

/* Test 3: Complex invariant */
int test_complex_invariant(int *arr, int base, int n) {
    int result = 0;
    int temp1 = base + 10;      // Invariant
    int temp2 = temp1 * 2;      // Invariant (depends on temp1)
    int factor = temp2 - 5;     // Invariant (depends on temp2)
    for (int i = 0; i < n; i++) {
        result += arr[i] * factor;  // factor is invariant
    }
    return result;
}

/* Test 4: Address calculation invariant */
int test_address_invariant(int **arrs, int n, int offset) {
    int result = 0;
    int *base = arrs[0] + offset;  // Invariant base pointer
    for (int i = 0; i < n; i++) {
        result += base[i];  // base is invariant
    }
    return result;
}

/* Test 5: Multiple invariants */
int test_multiple_invariants(int *arr, int n, int a, int b, int c) {
    int result = 0;
    int x = a + b;      // Invariant
    int y = x * c;      // Invariant
    int z = y - 10;     // Invariant
    for (int i = 0; i < n; i++) {
        result += arr[i] + z;  // z is invariant
    }
    return result;
}

/* High-iteration benchmark for performance testing */
int benchmark_large_loop(int *arr, int n, int factor) {
    int result = 0;
    for (int i = 0; i < n; i++) {
        result += arr[i] * factor;
    }
    return result;
}

/* Main for standalone testing */
int main() {
    int arr[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    int multiplier = 3;
    int *arrs[1] = {arr};
    
    int r1 = test_simple_invariant(arr, 10);
    int r2 = test_load_invariant(arr, &multiplier, 10);
    int r3 = test_complex_invariant(arr, 10, 10);
    int r4 = test_address_invariant(arrs, 10, 0);
    int r5 = test_multiple_invariants(arr, 10, 1, 2, 3);
    
    return r1 + r2 + r3 + r4 + r5;
}
