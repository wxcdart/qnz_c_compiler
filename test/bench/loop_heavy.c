/* Loop-heavy benchmark for optimization profiling */

/* Simple loop - should detect invariants */
int sum_array(int *arr, int n) {
    int sum = 0;
    int factor = 2;  /* Invariant */
    for (int i = 0; i < n; i++) {
        sum += arr[i] * factor;  /* factor is invariant */
    }
    return sum;
}

/* Loop with multiple invariants */
int compute_weighted_sum(int *data, int n, int weight) {
    int sum = 0;
    int scale = weight * 2;      /* Invariant */
    int offset = n / 2;          /* Invariant */
    int threshold = 100;         /* Invariant */
    
    for (int i = 0; i < n; i++) {
        int val = data[i];
        if (val > threshold) {
            sum += val * scale;
        }
        sum += offset;  /* Invariant addition */
    }
    return sum;
}

/* Loop with invariant function call */
int get_constant(void) {
    return 42;
}

int use_constant(int n) {
    int total = 0;
    int c = get_constant();  /* Invariant */
    for (int i = 0; i < n; i++) {
        total += i + c;
    }
    return total;
}

/* Complex loop with multiple invariants */
int complex_loop(int *a, int *b, int n, int x, int y) {
    int result = 0;
    int ax = x * 2;           /* Invariant */
    int by = y * 3;           /* Invariant */
    int sum_xy = x + y;       /* Invariant */
    int diff_xy = x - y;      /* Invariant */
    
    for (int i = 0; i < n; i++) {
        result += a[i] * ax;
        result += b[i] * by;
        result += sum_xy;
        if (i > diff_xy) {
            result += diff_xy;
        }
    }
    return result;
}

/* Main function to run all benchmarks */
int main(void) {
    int arr[100];
    int data[100];
    int a[100];
    int b[100];
    
    /* Initialize arrays */
    for (int i = 0; i < 100; i++) {
        arr[i] = i + 1;
        data[i] = i + 1;
        a[i] = i + 1;
        b[i] = i + 1;
    }
    
    /* Run benchmarks */
    int r1 = sum_array(arr, 100);
    int r2 = compute_weighted_sum(data, 100, 5);
    int r3 = use_constant(100);
    int r4 = complex_loop(a, b, 100, 10, 20);
    
    return r1 + r2 + r3 + r4;
}
