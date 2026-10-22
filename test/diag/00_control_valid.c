/* Control: valid program — must compile clean in both compilers. */
int add(int a, int b) { return a + b; }
int main(void) { return add(1, 2) - 3; }
