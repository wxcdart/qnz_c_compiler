/* LICM Test: load_invariant */

int test(int *arr, int *mult, int n) {
    int result = 0;
    int m = *mult;
    for (int i = 0; i < n; i++) {
        result += arr[i] * m;
    }
    return result;
}
