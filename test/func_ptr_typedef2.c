/* Test function pointer typedef with call */
typedef int (*math_op)(int, int);

int add(int a, int b) { return a + b; }
int mul(int a, int b) { return a * b; }

int main() {
    math_op op1 = add;
    math_op op2 = mul;
    int r1 = op1(10, 5);  /* 15 */
    int r2 = op2(10, 5);  /* 50 */
    return (r1 + r2) == 65 ? 0 : 1;
}
