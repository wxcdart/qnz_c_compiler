/* LICM Test: multiple_invariants */
int test(int *arr, int n, int a, int b, int c) {
    int result = 0;
    int x = a + b;
    int y = x * c;
    for (int i = 0; i < n; i++) {
        result += arr[i] + y;
    }
    return result;
}

int main() {
    int arr[5] = {1, 2, 3, 4, 5};
    return test(arr, 5, 1, 2, 3);  /* x=3, y=9, Expected: (1+9)+(2+9)+(3+9)+(4+9)+(5+9) = 65 */
}
