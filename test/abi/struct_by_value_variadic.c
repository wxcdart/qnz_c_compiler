#include <stdio.h>
#include <stdarg.h>

typedef struct { int a; } Small;

// Variadic function that sums the 'a' field of a number of Small structs passed by value.
int sum_variadic(int count, ...) {
    va_list ap;
    va_start(ap, count);
    int total = 0;
    for (int i = 0; i < count; ++i) {
        Small s = va_arg(ap, Small); // struct passed by value
        total += s.a;
    }
    va_end(ap);
    return total;
}

int main(void) {
    Small s1 = { 3 };
    Small s2 = { 5 };
    Small s3 = { 7 };
    int result = sum_variadic(3, s1, s2, s3);
    if (result != 15) {
        fprintf(stderr, "FAIL variadic %d\n", result);
        return 1;
    }
    printf("PASS\n");
    return 0;
}
