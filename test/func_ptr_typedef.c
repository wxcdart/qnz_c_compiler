/* Test function pointer typedef */
typedef int (*func_ptr)(int, int);

int add(int a, int b) {
    return a + b;
}

int main() {
    func_ptr fp = add;
    int result = fp(2, 3);
    return result == 5 ? 0 : 1;
}
