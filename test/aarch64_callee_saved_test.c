/* Test callee-saved register preservation on AArch64 */

/* Function that should preserve callee-saved registers */
int callee(int x) {
    /* This will likely use callee-saved registers if optimized */
    int a = x * 2;
    int b = a + 10;
    int c = b * 3;
    return c;
}

/* Function that calls callee and uses result */
int caller(int input) {
    int result1 = callee(input);
    int result2 = callee(result1);
    int result3 = callee(result2);
    return result3;
}

int main() {
    int test = 5;
    int expected = 5 * 2 * 3;  // 30
    expected = expected * 2 * 3;  // 180
    expected = expected * 2 * 3;  // 1080
    
    int actual = caller(test);
    
    /* Return 0 if correct, non-zero if wrong */
    return (actual == expected) ? 0 : 1;
}
