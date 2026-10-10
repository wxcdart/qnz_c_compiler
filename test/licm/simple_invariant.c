/* LICM Test: simple_invariant */

int test(int *arr, int n, int factor) {
    int result = 0;
    for (int i = 0; i < n; i++) {
        result += arr[i] * factor;
    }
    return result;
}
