/* LICM Test: complex_invariant */
int test(int *arr, int base, int n) {
    int result = 0;
    int t1 = base + 10;
    int t2 = t1 * 2;
    int factor = t2 - 5;
    for (int i = 0; i < n; i++) {
        result += arr[i] * factor;
    }
    return result;
}

int main() {
    int arr[5] = {1, 2, 3, 4, 5};
    return test(arr, 10, 5);  /* t1=20, t2=40, factor=35, Expected: 35*15 = 525 */
}
