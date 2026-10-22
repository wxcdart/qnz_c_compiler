/* Three distinct faults — multi-error recovery target:
 * one pass should report all three, not stop at the first. */
int add(int a, int b) { return a + b; }
int helper(void) { return 41; }
int main(void) {
    int x = add(1);        /* fault 1: too few arguments to add */
    int y = helpler();     /* fault 2: undeclared 'helpler' (~'helper') */
    return x + y + 5 return; /* fault 3: missing ';' before return */
}
