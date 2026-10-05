// AOT Test: Loops and Conditionals
// Tests: for loop, if/else, array access

int sum_array(int* arr, int size) {
    int sum = 0;
    for (int i = 0; i < size; i++) {
        sum += arr[i];
    }
    return sum;
}

int factorial(int n) {
    int result = 1;
    for (int i = 2; i <= n; i++) {
        result *= i;
    }
    return result;
}

int main() {
    int arr[] = {1, 2, 3, 4, 5};
    int sum = sum_array(arr, 5);
    if (sum != 15) return 1;
    
    int fact5 = factorial(5);
    if (fact5 != 120) return 2;
    
    int fact0 = factorial(0);
    if (fact0 != 1) return 3;
    
    return 0;
}
