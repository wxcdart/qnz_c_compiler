/* LICM Test: simple_invariant */
int test(int *arr, int n, int factor) {
    int result = 0;
    for (int i = 0; i < n; i++) {
        result += arr[i] * factor;
    }
    return result;
}

int main() {
    int arr[5] = {1, 2, 3, 4, 5};
    return test(arr, 5, 3);  /* Expected: 1*3 + 2*3 + 3*3 + 4*3 + 5*3 = 45 */
}
