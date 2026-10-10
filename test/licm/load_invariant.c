/* LICM Test: load_invariant */
int test(int *arr, int *mult, int n) {
    int result = 0;
    int m = *mult;
    for (int i = 0; i < n; i++) {
        result += arr[i] * m;
    }
    return result;
}

int main() {
    int arr[5] = {1, 2, 3, 4, 5};
    int mult = 3;
    return test(arr, &mult, 5);  /* Expected: 45 */
}
