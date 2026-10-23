extern int target;
int *p = &target;
int main(void) {
    if (p != &target) return 1;
    *p = 42;
    return *p == 42 ? 0 : 1;
}
