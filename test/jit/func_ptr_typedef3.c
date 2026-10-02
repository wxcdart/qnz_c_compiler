/* Test function pointer as parameter */
typedef int (*cmp_func)(int, int);

int less(int a, int b) { return a < b; }
int greater(int a, int b) { return a > b; }

int compare(cmp_func f, int x, int y) {
    return f(x, y);
}

int main() {
    int r1 = compare(less, 5, 10);      /* 1 (true) */
    int r2 = compare(greater, 5, 10);   /* 0 (false) */
    int result = r1 + r2 * 2;
    return result == 1 ? 0 : 1;
}
